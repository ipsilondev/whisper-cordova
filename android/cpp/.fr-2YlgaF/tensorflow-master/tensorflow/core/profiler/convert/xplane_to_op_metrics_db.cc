/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/convert/xplane_to_op_metrics_db.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/convert/op_metrics_db_combiner.h"
#include "tensorflow/core/profiler/convert/op_stack.h"
#include "tensorflow/core/profiler/protobuf/op_metrics.pb.h"
#include "tensorflow/core/profiler/protobuf/xplane.pb.h"
#include "tensorflow/core/profiler/utils/cost_utils.h"
#include "tensorflow/core/profiler/utils/op_metrics_db_utils.h"
#include "tensorflow/core/profiler/utils/op_utils.h"
#include "tensorflow/core/profiler/utils/tf_op_utils.h"
#include "tensorflow/core/profiler/utils/tf_xplane_visitor.h"
#include "tensorflow/core/profiler/utils/timespan.h"
#include "tensorflow/core/profiler/utils/trace_utils.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_visitor.h"

namespace tensorflow {
namespace profiler {
namespace {

constexpr uint64_t kRootSymbolId = 0;

// Type of a TensorFlow Op activity, which is either beginning or ending an Op.
enum TfActivityType { kTfOpBegin, kTfOpEnd };

// Instant activity representing the begin or end of a host-side TF Op.
struct TfActivity {
  // The timestamp in picoseconds when this activity happened.
  uint64 timestamp_ps;
  // The ID of this Op.
  uint32 tf_op_id;
  // Type of this activity.
  TfActivityType activity_type;
  // Full TF op name and type of this activity (backed by XEvent::name).
  TfOp tf_op;
  // Whether it is eagerly executed.
  bool is_eager;
};

// TF Op metrics stored as element in OpStack.
struct TfOpInfo {
  explicit TfOpInfo(uint64 ts) : start_timestamp_ps(ts) {}

  // Start timestamp in picoseconds.
  uint64 start_timestamp_ps;
  // Children duration in picoseconds.
  uint64 children_duration_ps = 0;
};

// Processes a TF-activity on particular core.
void ProcessOneTfActivity(const TfActivity& activity,
                          OpStack<TfOpInfo>* tf_op_stack,
                          TfMetricsDbData* tf_metrics_data) {
  uint32 tf_op_id = activity.tf_op_id;
  switch (activity.activity_type) {
    case kTfOpBegin: {
      tf_op_stack->Push(tf_op_id,
                        absl::make_unique<TfOpInfo>(activity.timestamp_ps));
      break;
    }
    case kTfOpEnd: {
      std::unique_ptr<TfOpInfo> info = tf_op_stack->Pop(tf_op_id);
      if (info == nullptr) {
        // This happens if TraceMes overlap.
        VLOG(1) << "No begin event found for TF activity id=" << tf_op_id
                << " name=" << activity.tf_op.name
                << " type=" << activity.tf_op.type;
        break;
      }
      Timespan tf_op_span =
          PicoSpan(info->start_timestamp_ps, activity.timestamp_ps);
      tf_metrics_data->tf_metrics_db_builder.EnterOp(
          activity.tf_op.name, activity.tf_op.type, activity.is_eager,
          tf_op_span.duration_ps(), info->children_duration_ps);
      TfOpInfo* parent_info = tf_op_stack->Top();
      if (parent_info != nullptr) {
        parent_info->children_duration_ps += tf_op_span.duration_ps();
      }
      if (IsInfeedEnqueueOp(activity.tf_op.type)) {
        tf_metrics_data->tf_metrics_db_builder.EnterHostInfeedEnqueue(
            tf_op_span);
      }
      break;
    }
  }
}

// Processes all TF-activities on the given core.
void ProcessTfActivities(std::vector<TfActivity>* tf_activities,
                         TfMetricsDbData* tf_metrics_db_data) {
  if (tf_activities->empty()) return;
  absl::c_stable_sort(*tf_activities,
                      [](const TfActivity& a, const TfActivity& b) {
                        return a.timestamp_ps < b.timestamp_ps;
                      });
  OpStack<TfOpInfo> tf_op_stack;
  for (const auto& tf_activity : *tf_activities) {
    ProcessOneTfActivity(tf_activity, &tf_op_stack, tf_metrics_db_data);
  }
  SetTotalTimePs(
      tf_metrics_db_data->tf_metrics_db,
      tf_activities->back().timestamp_ps - tf_activities->front().timestamp_ps);
}

void CollectTfActivities(const XLineVisitor& line,
                         const absl::flat_hash_map<int64_t, TfOp>& tf_ops,
                         std::vector<TfActivity>* tf_activities) {
  uint32 tf_op_id = 0;
  tf_activities->reserve(line.NumEvents() * 2);
  line.ForEachEvent([&tf_ops, &tf_op_id,
                     &tf_activities](const XEventVisitor& event) {
    const TfOp* tf_op = gtl::FindOrNull(tf_ops, event.Id());
    if (tf_op != nullptr) {
      ++tf_op_id;
      bool is_eager = false;
      if (absl::optional<XStatVisitor> stat =
              event.GetStat(StatType::kIsEager)) {
        is_eager = stat->IntValue();
      }
      Timespan span = event.GetTimespan();
      tf_activities->push_back(
          {span.begin_ps(), tf_op_id, kTfOpBegin, *tf_op, is_eager});
      tf_activities->push_back(
          {span.end_ps(), tf_op_id, kTfOpEnd, *tf_op, is_eager});
    }
  });
}

struct OpKey {
  std::optional<uint64_t> program_id;
  std::optional<uint64_t> symbol_id;
};
OpKey GetOpKeyFromHloEventMetadata(
    const XEventMetadataVisitor& hlo_event_metadata) {
  OpKey op_key;
  hlo_event_metadata.ForEachStat([&](const XStatVisitor& stat) {
    if (stat.Type().has_value()) {
      switch (static_cast<StatType>(*stat.Type())) {
        case StatType::kProgramId:
          op_key.program_id = stat.IntOrUintValue();
          break;
        case StatType::kSymbolId:
          op_key.symbol_id = stat.IntOrUintValue();
          break;
        default:
          break;
      }
    }
  });
  return op_key;
}

void SetOpMetadataFromHloEventMetadata(
    const XEventMetadataVisitor& hlo_event_metadata, OpMetrics* op_metrics) {
  if (hlo_event_metadata.HasDisplayName()) {
    op_metrics->set_name(std::string(hlo_event_metadata.DisplayName()));
    op_metrics->set_long_name(std::string(hlo_event_metadata.Name()));
  } else {
    op_metrics->set_name(std::string(hlo_event_metadata.Name()));
  }
  hlo_event_metadata.ForEachStat([&](const XStatVisitor& stat) {
    if (stat.Type().has_value()) {
      switch (static_cast<StatType>(*stat.Type())) {
        case StatType::kHloCategory:
          op_metrics->set_category(std::string(stat.StrOrRefValue()));
          break;
        case StatType::kTfOp:
          op_metrics->set_provenance(std::string(stat.StrOrRefValue()));
          break;
        case StatType::kFlops:
          op_metrics->set_flops(stat.IntOrUintValue());
          break;
        case StatType::kBytesAccessed:
          op_metrics->set_bytes_accessed(stat.IntOrUintValue());
          break;
        default:
          break;
      }
    }
  });
  hlo_event_metadata.ForEachChild(
      [&](const XEventMetadataVisitor& child_hlo_event_metadata) {
        OpMetrics* child = op_metrics->mutable_children()->add_metrics_db();
        child->set_occurrences(1);
        SetOpMetadataFromHloEventMetadata(child_hlo_event_metadata, child);
      });
}

void SetOpMetricsFromHloEvent(const XEventVisitor& hlo_event,
                              OpMetrics* op_metrics) {
  uint64_t duration_ps = hlo_event.DurationPs();
  uint64_t min_duration_ps = duration_ps;
  uint64_t self_duration_ps = duration_ps;
  uint64_t dma_stall_ps = 0;
  hlo_event.ForEachStat([&](const XStatVisitor& stat) {
    if (!stat.Type()) return;
    switch (static_cast<StatType>(*stat.Type())) {
      case StatType::kMinDurationPs:
        min_duration_ps = stat.IntValue();
        break;
      case StatType::kSelfDurationPs:
        self_duration_ps = stat.IntValue();
        break;
      case StatType::kDmaStallDurationPs:
        dma_stall_ps = stat.IntValue();
        break;
      default:
        break;
    }
  });
  if (op_metrics->occurrences() == 0) {
    SetOpMetadataFromHloEventMetadata(hlo_event.Metadata(), op_metrics);
    op_metrics->set_occurrences(hlo_event.NumOccurrences());
    op_metrics->set_time_ps(duration_ps);
    op_metrics->set_min_time_ps(min_duration_ps);
    op_metrics->set_self_time_ps(self_duration_ps);
    op_metrics->set_dma_stall_ps(dma_stall_ps);
  } else {
    op_metrics->set_occurrences(op_metrics->occurrences() +
                                hlo_event.NumOccurrences());
    op_metrics->set_time_ps(op_metrics->time_ps() + duration_ps);
    op_metrics->set_min_time_ps(
        std::min<uint64_t>(op_metrics->min_time_ps(), min_duration_ps));
    op_metrics->set_self_time_ps(op_metrics->self_time_ps() + self_duration_ps);
    op_metrics->set_dma_stall_ps(op_metrics->dma_stall_ps() + dma_stall_ps);
  }
}

void AdjustFlopsAndBytesAccessed(OpMetrics& op_metrics) {
  op_metrics.set_flops(op_metrics.flops() * op_metrics.occurrences());
  op_metrics.set_bytes_accessed(op_metrics.bytes_accessed() *
                                op_metrics.occurrences());
}

}  // namespace

absl::flat_hash_map<int64_t, TfOp> CollectTfOpsFromHostThreadsXPlane(
    const XPlane& host_trace) {
  absl::flat_hash_map<int64_t, TfOp> tf_ops;
  for (const auto& id_metadata : host_trace.event_metadata()) {
    const XEventMetadata& metadata = id_metadata.second;
    // On the host, we have added some user-specified TraceMe's in addition to
    // the TraceMe's added to every TensorFlow op by the system. These
    // user-inserted TraceMe's have "unknown" type. We don't count them in
    // Tf-stats.
    TfOp tf_op = ParseTfOpFullname(metadata.name());
    if (tf_op.category != Category::kUnknown) {
      tf_ops.try_emplace(metadata.id(), tf_op);
    }
  }
  return tf_ops;
}

TfMetricsDbData ConvertHostThreadsXLineToTfMetricsDbData(
    const XLineVisitor& line,
    const absl::flat_hash_map<int64_t, TfOp>& tf_ops) {
  TfMetricsDbData tf_metrics_db_data;
  if (!tf_ops.empty()) {
    std::vector<TfActivity> tf_activities;
    CollectTfActivities(line, tf_ops, &tf_activities);
    ProcessTfActivities(&tf_activities, &tf_metrics_db_data);
  }
  return tf_metrics_db_data;
}

void ConsumeTfMetricsDbData(TfMetricsDbData src, OpMetricsDbCombiner* dst) {
  AddIdleOp(src.tf_metrics_db);
  // Host OpMetricsDb does not need to update the number of cores a certain op
  // occurs.
  dst->Combine(src.tf_metrics_db, /*update_num_cores=*/false);
  src.tf_metrics_db.Clear();
}

OpMetricsDb ConvertHostThreadsXPlaneToOpMetricsDb(const XPlane& host_trace) {
  absl::flat_hash_map<int64_t, TfOp> tf_ops =
      CollectTfOpsFromHostThreadsXPlane(host_trace);
  OpMetricsDb result;
  OpMetricsDbCombiner combiner(&result);
  XPlaneVisitor plane = CreateTfXPlaneVisitor(&host_trace);
  plane.ForEachLine([&tf_ops, &combiner](const XLineVisitor& line) {
    ConsumeTfMetricsDbData(
        ConvertHostThreadsXLineToTfMetricsDbData(line, tf_ops), &combiner);
  });
  return result;
}

OpMetricsDb ConvertTpuDeviceTraceXPlaneToOpMetricsDb(
    const XPlane& device_trace) {
  OpMetricsDb result;
  XPlaneVisitor plane = CreateTfXPlaneVisitor(&device_trace);
  using OpMetricBySymbol =
      absl::flat_hash_map</*symbol_id=*/uint64_t, OpMetrics>;
  absl::flat_hash_map</*program_id=*/uint64_t, OpMetricBySymbol> flat_op_metric;

  uint64_t total_op_time_ps = 0;

  plane.ForEachLine([&](const XLineVisitor& line) {
    line.ForEachEvent([&](const XEventVisitor& event) {
      OpKey key = GetOpKeyFromHloEventMetadata(event.Metadata());
      if (!key.program_id.has_value() || !key.symbol_id.has_value()) return;
      OpMetricBySymbol& op_metric_by_symbol =
          flat_op_metric[key.program_id.value()];
      if (key.symbol_id != kRootSymbolId) {
        OpMetrics& op_metrics = op_metric_by_symbol[key.symbol_id.value()];
        SetOpMetricsFromHloEvent(event, &op_metrics);
        total_op_time_ps += op_metrics.self_time_ps();
      }
    });
  });

  for (auto& [program_id, op_metric_by_symbol] : flat_op_metric) {
    for (auto& [symbol_id, op_metrics] : op_metric_by_symbol) {
      AdjustFlopsAndBytesAccessed(op_metrics);
      result.add_metrics_db()->Swap(&op_metrics);
    }
  }
  result.set_total_op_time_ps(total_op_time_ps);
  auto total_time_ps = plane.GetStat(StatType::kTotalProfileDurationPs);
  SetTotalTimePs(result, total_time_ps->IntOrUintValue());
  AddIdleOp(result);
  return result;
}

OpMetricsDb ConvertDeviceTraceXPlaneToOpMetricsDb(const XPlane& device_trace) {
  OpMetricsDb result;
  DeviceOpMetricsDbBuilder device_op_metrics_db_builder(&result);

  int64_t first_op_offset_ps = kint64max;
  int64_t last_op_offset_ps = 0;

  TfOpRoofLineCostEstimator op_level_cost_estimator;
  XPlaneVisitor plane = CreateTfXPlaneVisitor(&device_trace);
  plane.ForEachLine([&](const XLineVisitor& line) {
    if (IsDerivedThreadId(line.Id())) return;
    line.ForEachEvent([&](const XEventVisitor& event) {
      first_op_offset_ps = std::min(first_op_offset_ps, event.OffsetPs());
      last_op_offset_ps = std::max(last_op_offset_ps, event.EndOffsetPs());

      absl::string_view tf_op_full_name;
      bool is_eager = false;
      event.ForEachStat([&](const XStatVisitor& stat) {
        if (stat.Type() == StatType::kTfOp) {
          tf_op_full_name = stat.StrOrRefValue();
        } else if (stat.Type() == StatType::kIsEager) {
          is_eager = stat.IntValue();
        }
      });
      if (tf_op_full_name.empty()) return;
      TfOp tf_op = ParseTfOpFullname(tf_op_full_name);
      TfOpRoofLineCostEstimator::OpRoofLineStats costs;
      if (tf_op.category != Category::kUnknown) {
        costs = op_level_cost_estimator.Predict(event);
      }
      device_op_metrics_db_builder.EnterOp(
          /*program_id=*/0, absl::StrCat(tf_op.name, "/", event.Name()),
          tf_op.type, tf_op_full_name, is_eager,
          /*occurrences=*/1, event.DurationPs(),
          /*children_time_ps=*/0, costs.flops, costs.bytes_accessed);
    });
  });
  SetTotalTimePs(
      result, last_op_offset_ps ? last_op_offset_ps - first_op_offset_ps : 0);
  AddIdleOp(result);
  return result;
}

}  // namespace profiler
}  // namespace tensorflow
