/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/kernels/data/flat_map_dataset_op.h"

#include <string>
#include <utility>

#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/graph_constructor.h"
#include "tensorflow/core/common_runtime/graph_runner.h"
#include "tensorflow/core/common_runtime/input_colocation_exemption_registry.h"
#include "tensorflow/core/data/captured_function.h"
#include "tensorflow/core/data/dataset_utils.h"
#include "tensorflow/core/data/name_utils.h"
#include "tensorflow/core/data/serialization_utils.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/random/random.h"

namespace tensorflow {
namespace data {

// See documentation in ../../ops/dataset_ops.cc for a high-level
// description of the following op.

/* static */ constexpr const char* const FlatMapDatasetOp::kDatasetType;
/* static */ constexpr const char* const FlatMapDatasetOp::kInputDataset;
/* static */ constexpr const char* const FlatMapDatasetOp::kOtherArguments;
/* static */ constexpr const char* const FlatMapDatasetOp::kFunc;
/* static */ constexpr const char* const FlatMapDatasetOp::kTarguments;
/* static */ constexpr const char* const FlatMapDatasetOp::kOutputTypes;
/* static */ constexpr const char* const FlatMapDatasetOp::kOutputShapes;

constexpr char kCycleLength[] = "cycle_length";
constexpr char kElementIndex[] = "element_index";
constexpr char kInputsSize[] = "inputs_size";
constexpr char kInputs[] = "inputs";
constexpr char kCurrentElementIteratorUninitialized[] =
    "current_element_iterator_uninitialized";
constexpr char kExhausted[] = "exhausted";

class FlatMapDatasetOp::Dataset : public DatasetBase {
 public:
  Dataset(OpKernelContext* ctx, const DatasetBase* input,
          std::unique_ptr<CapturedFunction> captured_func,
          const DataTypeVector& output_types,
          const std::vector<PartialTensorShape>& output_shapes)
      : DatasetBase(DatasetContext(ctx)),
        input_(input),
        captured_func_(std::move(captured_func)),
        output_types_(output_types),
        output_shapes_(output_shapes) {
    input_->Ref();
  }

  ~Dataset() override { input_->Unref(); }

  std::unique_ptr<IteratorBase> MakeIteratorInternal(
      const string& prefix) const override {
    return std::make_unique<Iterator>(Iterator::Params{
        this, name_utils::IteratorPrefix(kDatasetType, prefix)});
  }

  const DataTypeVector& output_dtypes() const override { return output_types_; }

  const std::vector<PartialTensorShape>& output_shapes() const override {
    return output_shapes_;
  }

  string DebugString() const override {
    return name_utils::DatasetDebugString(kDatasetType);
  }

  Status InputDatasets(std::vector<const DatasetBase*>* inputs) const override {
    inputs->push_back(input_);
    return OkStatus();
  }

  Status CheckExternalState() const override {
    TF_RETURN_IF_ERROR(captured_func_->CheckExternalState());
    return input_->CheckExternalState();
  }

 protected:
  Status AsGraphDefInternal(SerializationContext* ctx,
                            DatasetGraphDefBuilder* b,
                            Node** output) const override {
    Node* input_graph_node = nullptr;
    TF_RETURN_IF_ERROR(b->AddInputDataset(ctx, input_, &input_graph_node));
    std::vector<Node*> other_arguments;
    DataTypeVector other_arguments_types;
    TF_RETURN_IF_ERROR(captured_func_->AddToGraph(ctx, b, &other_arguments,
                                                  &other_arguments_types));
    AttrValue f;
    b->BuildAttrValue(captured_func_->func(), &f);
    AttrValue other_arguments_types_attr;
    b->BuildAttrValue(other_arguments_types, &other_arguments_types_attr);

    TF_RETURN_IF_ERROR(b->AddDataset(
        this, {std::make_pair(0, input_graph_node)},  // Single tensor inputs.
        {std::make_pair(1, other_arguments)},         // Tensor list inputs.
        {std::make_pair(kFunc, f),
         std::make_pair(kTarguments, other_arguments_types_attr)},  // Attrs
        output));
    return OkStatus();
  }

 private:
  class Iterator : public DatasetIterator<Dataset> {
   public:
    explicit Iterator(const Params& params)
        : DatasetIterator<Dataset>(params) {}

    bool SymbolicCheckpointCompatible() const override { return true; }

    Status Initialize(IteratorContext* ctx) override {
      TF_RETURN_IF_ERROR(
          dataset()->input_->MakeIterator(ctx, this, prefix(), &input_impl_));
      return dataset()->captured_func_->Instantiate(
          ctx, &instantiated_captured_func_);
    }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      mutex_lock l(mu_);
      do {
        if (!input_impl_) {
          *end_of_sequence = true;
          return OkStatus();
        }
        if (current_element_iterator_) {
          // We are currently processing a mapped element, so try to get the
          // next subelement.
          bool end_of_element;
          auto nested_ctx = MakeNestedIteratorContext(ctx);
          TF_RETURN_IF_ERROR(current_element_iterator_->GetNext(
              &nested_ctx, out_tensors, &end_of_element));
          ctx->MergeCheckpoint(nested_ctx.checkpoint());
          if (!end_of_element) {
            // Produce the subelement as output.
            *end_of_sequence = false;
            return OkStatus();
          }

          // We have reached the end of the current element, so maybe move on
          // to the next element.
          ctx->PurgeCheckpoint(current_element_iterator_->prefix());
          current_element_iterator_.reset();
        }

        // Get the next element from the input dataset.
        inputs_.clear();
        TF_RETURN_IF_ERROR(
            input_impl_->GetNext(ctx, &inputs_, end_of_sequence));
        if (*end_of_sequence) {
          input_impl_.reset();
          return OkStatus();
        }

        TF_RETURN_IF_ERROR(
            BuildCurrentElementIteratorLocked(ctx, /*is_get_next=*/true));
      } while (true);
    }

    Status SkipInternal(IteratorContext* ctx, int num_to_skip,
                        bool* end_of_sequence, int* num_skipped) override {
      mutex_lock l(mu_);
      *num_skipped = 0;
      while (*num_skipped < num_to_skip) {
        if (!input_impl_) {
          *end_of_sequence = true;
          return OkStatus();
        }
        if (!current_element_iterator_) {
          // Get the next element from the input dataset.
          inputs_.clear();
          TF_RETURN_IF_ERROR(
              input_impl_->GetNext(ctx, &inputs_, end_of_sequence));
          if (*end_of_sequence) {
            input_impl_.reset();
            *end_of_sequence = true;
            return OkStatus();
          }
          TF_RETURN_IF_ERROR(
              BuildCurrentElementIteratorLocked(ctx, /*is_get_next=*/false));
        }
        bool end_of_element;
        int last_num_skipped;
        TF_RETURN_IF_ERROR(current_element_iterator_->Skip(
            MakeNestedIteratorContext(ctx), num_to_skip - *num_skipped,
            &end_of_element, &last_num_skipped));
        *num_skipped += last_num_skipped;
        if (end_of_element) {
          // We have reached the end of the current element, so maybe move on
          // to the next element.
          current_element_iterator_.reset();
        }
      }
      *end_of_sequence = false;
      return OkStatus();
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeInterleaveManyNode(
          std::move(args),
          {model::MakeNonTunableParameter(kCycleLength, /*value=*/1)});
    }

    Status SaveInternal(SerializationContext* ctx,
                        IteratorStateWriter* writer) override {
      TF_RETURN_IF_ERROR(ctx->HandleCheckExternalStateStatus(
          dataset()->captured_func_->CheckExternalState()));
      mutex_lock l(mu_);
      TF_RETURN_IF_ERROR(writer->WriteScalar(
          full_name(kExhausted), static_cast<int64_t>(!input_impl_)));
      if (input_impl_) {
        TF_RETURN_IF_ERROR(SaveInput(ctx, writer, input_impl_));
        TF_RETURN_IF_ERROR(
            writer->WriteScalar(full_name(kElementIndex), element_index_));
        TF_RETURN_IF_ERROR(writer->WriteScalar(
            full_name(kCurrentElementIteratorUninitialized),
            static_cast<int64_t>(!current_element_iterator_)));
        if (current_element_iterator_) {
          TF_RETURN_IF_ERROR(
              writer->WriteScalar(full_name(kInputsSize), inputs_.size()));
          for (int i = 0; i < inputs_.size(); i++) {
            TF_RETURN_IF_ERROR(writer->WriteTensor(
                full_name(strings::StrCat(kInputs, "[", i, "]")), inputs_[i]));
          }
          TF_RETURN_IF_ERROR(SaveInput(ctx, writer, current_element_iterator_));
        }
      }
      return OkStatus();
    }

    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      mutex_lock l(mu_);
      input_impl_.reset();
      element_index_ = 0;
      current_element_iterator_.reset();
      inputs_.clear();
      int64_t input_exhausted;
      TF_RETURN_IF_ERROR(
          reader->ReadScalar(full_name(kExhausted), &input_exhausted));
      if (!static_cast<bool>(input_exhausted)) {
        TF_RETURN_IF_ERROR(
            dataset()->input_->MakeIterator(ctx, this, prefix(), &input_impl_));
        TF_RETURN_IF_ERROR(RestoreInput(ctx, reader, input_impl_));
        {
          int64_t temp;
          TF_RETURN_IF_ERROR(
              reader->ReadScalar(full_name(kElementIndex), &temp));
          element_index_ = temp;
        }
        int64_t current_element_iterator_uninitialized;
        TF_RETURN_IF_ERROR(
            reader->ReadScalar(full_name(kCurrentElementIteratorUninitialized),
                               &current_element_iterator_uninitialized));
        if (!static_cast<bool>(current_element_iterator_uninitialized)) {
          size_t inputs_size;
          {
            int64_t temp;
            TF_RETURN_IF_ERROR(
                reader->ReadScalar(full_name(kInputsSize), &temp));
            inputs_size = static_cast<size_t>(temp);
          }
          inputs_.reserve(inputs_size);
          for (int i = 0; i < inputs_size; i++) {
            inputs_.emplace_back();
            TF_RETURN_IF_ERROR(reader->ReadTensor(
                ctx->flr(), full_name(strings::StrCat(kInputs, "[", i, "]")),
                &inputs_.back()));
          }

          element_index_--;
          TF_RETURN_IF_ERROR(
              BuildCurrentElementIteratorLocked(ctx, /*is_get_next=*/false));
          TF_RETURN_IF_ERROR(
              RestoreInput(ctx, reader, current_element_iterator_));
        }
      }
      return OkStatus();
    }

   private:
    Status BuildCurrentElementIteratorLocked(IteratorContext* ctx,
                                             bool is_get_next)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      // NOTE: We intentionally ignore resource modeling outside GetNext().
      std::shared_ptr<model::Node> node = is_get_next ? model_node() : nullptr;
      return MakeIteratorFromInputElement(
          ctx, this, inputs_, element_index_++, *instantiated_captured_func_,
          prefix(), &current_element_iterator_, node);
    }

    mutex mu_;
    size_t element_index_ TF_GUARDED_BY(mu_) = 0;
    std::unique_ptr<IteratorBase> input_impl_ TF_GUARDED_BY(mu_);
    std::unique_ptr<IteratorBase> current_element_iterator_ TF_GUARDED_BY(mu_);
    std::vector<Tensor> inputs_ TF_GUARDED_BY(mu_);
    std::unique_ptr<InstantiatedCapturedFunction> instantiated_captured_func_;
  };

  const DatasetBase* const input_;
  const std::unique_ptr<CapturedFunction> captured_func_;
  const DataTypeVector output_types_;
  const std::vector<PartialTensorShape> output_shapes_;
};

FlatMapDatasetOp::FlatMapDatasetOp(OpKernelConstruction* ctx)
    : UnaryDatasetOpKernel(ctx), graph_def_version_(ctx->graph_def_version()) {
  OP_REQUIRES_OK(ctx, FunctionMetadata::Create(ctx, kFunc, /*params=*/{},
                                               &func_metadata_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr(kOutputTypes, &output_types_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr(kOutputShapes, &output_shapes_));
}

void FlatMapDatasetOp::MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                                   DatasetBase** output) {
  std::unique_ptr<CapturedFunction> captured_func;
  OP_REQUIRES_OK(ctx,
                 CapturedFunction::Create(ctx, func_metadata_, kOtherArguments,
                                          &captured_func));
  *output = new Dataset(ctx, input, std::move(captured_func), output_types_,
                        output_shapes_);
}

namespace {

REGISTER_KERNEL_BUILDER(Name("FlatMapDataset").Device(DEVICE_CPU),
                        FlatMapDatasetOp);
REGISTER_INPUT_COLOCATION_EXEMPTION("FlatMapDataset");

}  // namespace
}  // namespace data
}  // namespace tensorflow
