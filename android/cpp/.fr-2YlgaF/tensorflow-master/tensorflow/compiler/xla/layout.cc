/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/layout.h"

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_join.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/printer.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

namespace xla {

TileProto Tile::ToProto() const {
  TileProto tile_proto;
  for (int64_t i : dimensions()) {
    tile_proto.add_dimensions(i);
  }
  return tile_proto;
}

void Tile::Print(Printer* printer) const {
  printer->Append("(");
  AppendJoin(printer, dimensions(), ",", [&](Printer* printer, int64_t dim) {
    if (dim >= 0) {
      printer->Append(dim);
    } else {
      if (dim == kCombineDimension) {
        printer->Append("*");
      } else {
        printer->Append("Invalid value ");
        printer->Append(dim);
      }
    }
  });
  printer->Append(")");
}

std::string Tile::ToString() const {
  StringPrinter printer;
  Print(&printer);
  return std::move(printer).ToString();
}

Layout::Layout() = default;

Layout::Layout(absl::Span<const int64_t> minor_to_major)
    : minor_to_major_(minor_to_major.begin(), minor_to_major.end()) {}

Layout::Layout(absl::Span<const int64_t> minor_to_major,
               absl::Span<const DimLevelType> dim_level_types,
               absl::Span<const bool> dim_unique,
               absl::Span<const bool> dim_ordered, absl::Span<const Tile> tiles,
               PrimitiveType index_primitive_type,
               PrimitiveType pointer_primitive_type, int64_t memory_space,
               std::unique_ptr<Shape> physical_shape,
               int64_t dynamic_shape_metadata_prefix_bytes)
    : dim_level_types_(dim_level_types.begin(), dim_level_types.end()),
      dim_unique_(dim_unique.begin(), dim_unique.end()),
      dim_ordered_(dim_ordered.begin(), dim_ordered.end()),
      minor_to_major_(minor_to_major.begin(), minor_to_major.end()),
      tiles_(tiles.begin(), tiles.end()),
      index_primitive_type_(index_primitive_type),
      pointer_primitive_type_(pointer_primitive_type),
      memory_space_(memory_space),
      physical_shape_(std::move(physical_shape)),
      dynamic_shape_metadata_prefix_bytes_(
          dynamic_shape_metadata_prefix_bytes) {}

Layout::Layout(const Layout& other)
    : dim_level_types_(other.dim_level_types_),
      dim_unique_(other.dim_unique_),
      dim_ordered_(other.dim_ordered_),
      minor_to_major_(other.minor_to_major_),
      tiles_(other.tiles_),
      index_primitive_type_(other.index_primitive_type_),
      pointer_primitive_type_(other.pointer_primitive_type_),
      memory_space_(other.memory_space_),
      physical_shape_(other.physical_shape_ != nullptr
                          ? std::make_unique<Shape>(*other.physical_shape_)
                          : nullptr),
      dynamic_shape_metadata_prefix_bytes_(
          other.dynamic_shape_metadata_prefix_bytes_) {}

Layout::Layout(Layout&& other) = default;

Layout::~Layout() = default;

Layout& Layout::operator=(const Layout& other) {
  if (this != &other) {
    dim_level_types_ = other.dim_level_types_;
    dim_unique_ = other.dim_unique_;
    dim_ordered_ = other.dim_ordered_;
    minor_to_major_ = other.minor_to_major_;
    tiles_ = other.tiles_;
    index_primitive_type_ = other.index_primitive_type_;
    pointer_primitive_type_ = other.pointer_primitive_type_;
    memory_space_ = other.memory_space_;
    if (other.physical_shape_ != nullptr) {
      physical_shape_ = std::make_unique<Shape>(*other.physical_shape_);
    } else {
      physical_shape_ = nullptr;
    }
    dynamic_shape_metadata_prefix_bytes_ =
        other.dynamic_shape_metadata_prefix_bytes_;
  }
  return *this;
}

Layout& Layout::operator=(Layout&& other) = default;

/* static */ Layout Layout::CreateFromProto(const LayoutProto& proto) {
  Layout layout;
  for (int dim_level_type : proto.dim_level_types()) {
    layout.add_dim_level_type(static_cast<DimLevelType>(dim_level_type));
  }
  for (bool dim_unique : proto.dim_unique()) {
    layout.add_dim_unique(dim_unique);
  }
  for (bool dim_ordered : proto.dim_ordered()) {
    layout.add_dim_ordered(dim_ordered);
  }
  layout.minor_to_major_.reserve(proto.minor_to_major_size());
  for (const int64_t dimension : proto.minor_to_major()) {
    layout.add_minor_to_major(dimension);
  }
  for (const TileProto& tile_proto : proto.tiles()) {
    *layout.add_tiles() = Tile::CreateFromProto(tile_proto);
  }
  layout.set_index_primitive_type(proto.index_primitive_type());
  layout.set_pointer_primitive_type(proto.pointer_primitive_type());
  layout.set_memory_space(proto.memory_space());
  if (proto.has_physical_shape()) {
    *layout.mutable_physical_shape() = Shape(proto.physical_shape());
  }
  layout.set_dynamic_shape_metadata_prefix_bytes(
      proto.dynamic_shape_metadata_prefix_bytes());
  return layout;
}

LayoutProto Layout::ToProto() const {
  LayoutProto proto;
  for (DimLevelType dim_level_type : dim_level_types()) {
    proto.add_dim_level_types(dim_level_type);
  }
  for (bool dim_unique : dim_unique()) {
    proto.add_dim_unique(dim_unique);
  }
  for (bool dim_ordered : dim_ordered()) {
    proto.add_dim_ordered(dim_ordered);
  }
  proto.mutable_minor_to_major()->Reserve(minor_to_major_size());
  for (const int64_t dimension : minor_to_major()) {
    proto.add_minor_to_major(dimension);
  }
  for (const Tile& tile : tiles()) {
    *proto.add_tiles() = tile.ToProto();
  }
  proto.set_index_primitive_type(index_primitive_type());
  proto.set_pointer_primitive_type(pointer_primitive_type());
  proto.set_memory_space(memory_space_);
  if (has_physical_shape()) {
    *proto.mutable_physical_shape() = physical_shape_->ToProto();
  }
  proto.set_dynamic_shape_metadata_prefix_bytes(
      dynamic_shape_metadata_prefix_bytes_);
  return proto;
}

namespace {
absl::string_view DimLevelTypeAbbrev(DimLevelType dim_level_type) {
  switch (dim_level_type) {
    case DIM_DENSE:
      return "D";
    case DIM_COMPRESSED:
      return "C";
    case DIM_SINGLETON:
      return "S";
    default:
      LOG(FATAL) << "Invalid DimLevelType value: " << dim_level_type;
  }
}
}  // namespace

void Layout::Print(Printer* printer) const {
  printer->Append("{");
  AppendJoin(printer, minor_to_major(), ",");

  bool colon_printed = false;
  auto print_colon = [&]() {
    if (colon_printed) return;
    printer->Append(":");
    colon_printed = true;
  };

  if (!dim_level_types().empty()) {
    auto print_one = [&](int i) {
      printer->Append(DimLevelTypeAbbrev(dim_level_type(i)));
      if (!dim_unique().empty() && !dim_unique(i)) {
        printer->Append("+");
      }
      if (!dim_ordered().empty() && !dim_ordered(i)) {
        printer->Append("~");
      }
    };
    print_colon();
    printer->Append("D(");
    print_one(0);
    for (int i = 1; i < dim_level_types().size(); ++i) {
      printer->Append(",");
      print_one(i);
    }
    printer->Append(")");
  }

  if (!tiles().empty()) {
    print_colon();
    printer->Append("T");
    for (const Tile& tile : tiles()) {
      tile.Print(printer);
    }
  }

  if (index_primitive_type() != PRIMITIVE_TYPE_INVALID) {
    print_colon();
    if (primitive_util::IsIntegralType(index_primitive_type())) {
      printer->Append("#(");
      printer->Append(
          primitive_util::LowercasePrimitiveTypeName(index_primitive_type()));
      printer->Append(")");
    } else {
      printer->Append("#(invalid)");
    }
  }

  if (pointer_primitive_type() != PRIMITIVE_TYPE_INVALID) {
    print_colon();
    if (primitive_util::IsIntegralType(pointer_primitive_type())) {
      printer->Append("*(");
      printer->Append(
          primitive_util::LowercasePrimitiveTypeName(pointer_primitive_type()));
      printer->Append(")");
    } else {
      printer->Append("*(invalid)");
    }
  }

  if (memory_space() != 0) {
    print_colon();
    printer->Append("S(");
    printer->Append(memory_space());
    printer->Append(")");
  }

  if (has_physical_shape()) {
    print_colon();
    printer->Append("P(");
    physical_shape_->Print(printer, /*print_layout=*/true);
    printer->Append(")");
  }

  if (dynamic_shape_metadata_prefix_bytes_ > 0) {
    print_colon();
    printer->Append("M(");
    printer->Append(dynamic_shape_metadata_prefix_bytes());
    printer->Append(")");
  }

  printer->Append("}");
}

std::string Layout::ToString() const {
  StringPrinter printer;
  Print(&printer);
  return std::move(printer).ToString();
}

bool Layout::Equal::operator()(const Layout& lhs, const Layout& rhs) {
  if (!LayoutUtil::IsDense(lhs) || !LayoutUtil::IsDense(rhs)) {
    if (lhs.dim_level_types() != rhs.dim_level_types()) {
      return false;
    }
  }
  if (lhs.minor_to_major() != rhs.minor_to_major()) {
    return false;
  }
  if (!ignore_tiles_ && lhs.tiles() != rhs.tiles()) {
    return false;
  }
  if (!ignore_index_primitive_type_ &&
      lhs.index_primitive_type() != rhs.index_primitive_type()) {
    return false;
  }
  if (!ignore_pointer_primitive_type_ &&
      lhs.pointer_primitive_type() != rhs.pointer_primitive_type()) {
    return false;
  }
  if (!ignore_memory_space_ && lhs.memory_space() != rhs.memory_space()) {
    return false;
  }
  if (!ignore_physical_shape_) {
    if (lhs.has_physical_shape() || rhs.has_physical_shape()) {
      if (!lhs.has_physical_shape() || !rhs.has_physical_shape()) {
        return false;
      }
      if (lhs.physical_shape() != rhs.physical_shape()) {
        return false;
      }
    }
  }
  return true;
}

bool Layout::operator==(const Layout& other) const {
  return Equal()(*this, other);
}

std::ostream& operator<<(std::ostream& out, const Tile& tile) {
  out << tile.ToString();
  return out;
}

std::ostream& operator<<(std::ostream& out, const Layout& layout) {
  out << layout.ToString();
  return out;
}

Shape* Layout::mutable_physical_shape() {
  if (physical_shape_ == nullptr) {
    physical_shape_ = std::make_unique<Shape>();
  }
  return physical_shape_.get();
}

void Layout::clear_physical_shape() { physical_shape_ = nullptr; }

}  // namespace xla
