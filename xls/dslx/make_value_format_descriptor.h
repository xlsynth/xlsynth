// Copyright 2023 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_DSLX_MAKE_VALUE_FORMAT_DESCRIPTOR_H_
#define XLS_DSLX_MAKE_VALUE_FORMAT_DESCRIPTOR_H_

#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/value_format_descriptor.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {

// Builds owned formatting metadata, sharing repeated immutable sum descriptions
// within this call. The result does not retain Type or AST pointers.
// Channel types, including channel arrays, are rejected.
absl::StatusOr<ValueFormatDescriptor> MakeValueFormatDescriptor(
    const Type& type, FormatPreference field_preference);

// Builds formatting metadata for traced call arguments and return values.
// Channels and entire channel arrays are opaque handle leaves, even inside
// aggregates. Other types use MakeValueFormatDescriptor's construction.
// This does not describe the channels' payload messages.
absl::StatusOr<ValueFormatDescriptor> MakeTraceCallFormatDescriptor(
    const Type& type, FormatPreference field_preference);

// Supplies owned enum descriptors so a caller can reuse expensive immutable
// enum tables across separate aggregate builds. Called synchronously for enum
// leaves only; neither the provider nor borrowed Type pointers are retained.
using EnumFormatDescriptorProvider =
    absl::FunctionRef<absl::StatusOr<ValueFormatDescriptor>(const EnumType&)>;

absl::StatusOr<ValueFormatDescriptor> MakeValueFormatDescriptor(
    const Type& type, FormatPreference field_preference,
    EnumFormatDescriptorProvider enum_format_provider);

// Allows a single owner to share default-format sum descriptions when the
// same nested sum is reached through separately constructed root descriptors.
// The provider is called synchronously only for nested sums; the root sum is
// built locally so its owner can publish the completed descriptor afterward.
// Both providers must describe types in the same owner using default format.
using NestedSumFormatDescriptorProvider =
    absl::FunctionRef<absl::StatusOr<ValueFormatDescriptor>(const SumType&)>;

absl::StatusOr<ValueFormatDescriptor>
MakeDefaultValueFormatDescriptorWithNestedSumProvider(
    const Type& type, EnumFormatDescriptorProvider enum_format_provider,
    NestedSumFormatDescriptorProvider nested_sum_format_provider);

}  // namespace xls::dslx

#endif  // XLS_DSLX_MAKE_VALUE_FORMAT_DESCRIPTOR_H_
