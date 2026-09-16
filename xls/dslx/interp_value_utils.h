// Copyright 2021 The XLS Authors
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
#ifndef XLS_DSLX_INTERP_VALUE_UTILS_H_
#define XLS_DSLX_INTERP_VALUE_UTILS_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/frontend/ast_node.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/type.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/value.h"

namespace xls::dslx {

// Converts the given (Bits-typed) InterpValue to an array of equal- or
// smaller-sized Bits-typed values.
absl::StatusOr<InterpValue> CastBitsToArray(const InterpValue& bits_value,
                                            const ArrayType& array_type);

// Converts the given Bits-typed value into an enum-typed value.
absl::StatusOr<InterpValue> CastBitsToEnum(const InterpValue& bits_value,
                                           const EnumType& enum_type);

// Creates a zero-valued InterpValue with the same structure as the input.
absl::StatusOr<InterpValue> CreateZeroValue(const InterpValue& value);

// Validates the runtime representation and source-domain validity for the
// given DSLX type. Rejects malformed tags, including active nested sums, but
// accepts nonzero inactive padding without rewriting the value.
absl::Status ValidateInterpValueMatchesType(const InterpValue& value,
                                            const Type& type);

// Creates a canonical zero-like InterpValue from the given Type for
// interpreter/support-code internals. Semantic sums are rejected because their
// zero-value rule depends on discriminants and belongs to DSLX `zero!`.
absl::StatusOr<InterpValue> CreateZeroValueFromType(const Type& type);

namespace internal {

// Observations of one immutable match scrutinee, discarded before its selected
// arm executes. Paths identify tuple/struct/array or active payload members,
// not Type objects (which may be cloned between arms). Retained payloads are
// owned here, since the interpreter's per-arm matchee is temporary.
// Every path must keep the same value and semantic type throughout this
// lifetime; reusing an owner for another scrutinee could skip required checks.
class MatchValueObservation {
 public:
  using Path = std::vector<int64_t>;

  // As GetSumPayloadValues, with stable storage shared by subsequent shallow
  // and complete observations at this path. Does not inspect nested sum tags.
  absl::StatusOr<const std::vector<InterpValue>*> GetSumPayloadValues(
      const SumType& type, const InterpValue& value, const Path& path);

  // Validates the complete constant first, then completes/reuses validation of
  // this scrutinee subtree before comparing meaningful values. No equality
  // results or failed checks are retained.
  absl::StatusOr<bool> EqualsConstant(const InterpValue& constant,
                                      const InterpValue& value,
                                      const Type& type, const Path& path);

 private:
  friend class ValueTraversal;

  // Decoding alone is not validation; a shallow check does not validate active
  // nested tags. Complete validity is recorded only after all descendants pass.
  enum class Validation { kNone, kShallow, kComplete };
  struct Observation {
    std::optional<std::vector<InterpValue>> payload;
    Validation validation = Validation::kNone;
  };
  // Map nodes keep payload references stable during recursive insertion.
  std::map<Path, Observation> observations_;
};

// Creates a shape-correct internal placeholder, including empty enums and
// sums, when support code needs a complete value that will not be observed.
absl::StatusOr<InterpValue> CreateInternalPlaceholderValueFromType(
    const Type& type);

// Restores structure, signedness and enum identity from a packed image.
// Tuple/struct members are MSB-first; array element zero is LSB-first. Checks
// the width but preserves raw sum tags/padding. Source-domain consumers must
// also call ValidateInterpValueMatchesType.
absl::StatusOr<InterpValue> UnflattenValueForType(const Type& type,
                                                  const Bits& bits);

// Assembles a sum from payloads already constructed by the trusted zero-value
// visitor. The caller must have produced each payload for its declared type;
// skipping recursive revalidation keeps nested zero construction linear.
// Use CreateSumValue for unvalidated payloads.
absl::StatusOr<InterpValue> CreateSumValueFromValidatedZeroPayload(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values);

// Assembles payloads already produced for a known constructor by the trusted
// source-value generator. The slot width must belong to the same sum type.
absl::StatusOr<InterpValue> CreateSumValueFromValidatedGeneratedPayload(
    const SumType& type, int64_t variant_index, int64_t payload_slot_bit_count,
    absl::Span<const InterpValue> payload_values);

}  // namespace internal

// Creates a declared sum constructor, zeroing only its newly introduced
// padding. Existing nested sums retain every bit without observing their tags.
// Validates payload shape and ordinary numeric-enum membership.
absl::StatusOr<InterpValue> CreateSumValue(
    const SumType& type, std::string_view variant_name,
    absl::Span<const InterpValue> payload_values);

// As above, for a constructor already resolved in source-declaration order.
// Reuses the type's cached payload width without building a variant table.
absl::StatusOr<InterpValue> CreateSumValue(
    const SumType& type, int64_t variant_index,
    absl::Span<const InterpValue> payload_values);

// Checks the outer constructor and returns its active payload members, ignoring
// inactive padding. Nested sum images are preserved without observing their
// tags; explicit source validation must use ValidateInterpValueMatchesType.
absl::StatusOr<std::vector<InterpValue>> GetSumPayloadValues(
    const SumType& type, const InterpValue& value);

// Compares declared constructors and meaningful payloads recursively, ignoring
// sum padding. Validates all active constructors in both operands, even if an
// earlier member differs. Does not change raw InterpValue equality or hashing
// and never rewrites either operand.
absl::StatusOr<bool> SemanticValuesEqual(const InterpValue& lhs,
                                         const InterpValue& rhs,
                                         const Type& type);

// Finds the first index in the LHS and RHS sequences at which values differ or
// nullopt if the two are equal.
absl::StatusOr<std::optional<int64_t>> FindFirstDifferingIndex(
    absl::Span<const InterpValue> lhs, absl::Span<const InterpValue> rhs);

// As above, comparing elements with their DSLX semantic type rather than raw
// representation equality.
absl::StatusOr<std::optional<int64_t>> FindFirstDifferingIndex(
    absl::Span<const InterpValue> lhs, absl::Span<const InterpValue> rhs,
    const Type& element_type);

// Converts the values to matched the signedness of the concrete type.
//
// Converts bits-typed Values contained within the given Value to match the
// signedness of the Type. Examples:
//
// invocation: sign_convert_value(s8, u8:64)
// returns: s8:64
//
// invocation: sign_convert_value(s3, u8:7)
// returns: s3:-1
//
// invocation: sign_convert_value((s8, u8), (u8:42, u8:10))
// returns: (s8:42, u8:10)
//
// This conversion functionality is required because the Values used in the DSLX
// may be signed while Values in IR interpretation and Verilog simulation are
// always unsigned.
//
// Args:
//   type: Type to match.
//   value: Input value.
//
// Returns:
//   Sign-converted value.
absl::StatusOr<InterpValue> SignConvertValue(const Type& type,
                                             const InterpValue& value);

// As above, but a handy vectorized form for application on parameters of a
// function.
absl::StatusOr<std::vector<InterpValue>> SignConvertArgs(
    const FunctionType& fn_type, absl::Span<const InterpValue> args);

// Converts an (IR) value to an interpreter value.
//
// Semantic-sum reconstruction and validation require `type` to identify the
// nominal sum. Without `type`, encoded sums are converted as ordinary tuples
// without semantic-sum validation.
absl::StatusOr<InterpValue> ValueToInterpValue(const Value& v,
                                               const Type* type = nullptr);

// Parses a semicolon-delimited list of values.
//
// Example input:
//  bits[32]:6; (bits[8]:2, bits[16]:4)
//
// Returned bits values are always unsigned.
//
// Note: these values are parsed to InterpValues, but really they are just IR
// values that we're converting into InterpValues. Things like enums or structs
// (via named tuples) can't be parsed via this mechanism, it's fairly
// specialized for the scenario we've created in our fuzzing process.
absl::StatusOr<std::vector<InterpValue>> ParseArgs(std::string_view args_text);

// Does the above, but for a series of argument strings, one per line of input.
absl::StatusOr<std::vector<std::vector<InterpValue>>> ParseArgsBatch(
    std::string_view args_text);

// Converts an InterpValue of type u8[len] to a string. Assumes the InterpValue
// is utf8 encoded like everything else.
absl::StatusOr<std::string> InterpValueAsString(const InterpValue& v);

// Creates a ChannelReference InterpValue. `type` is the type of the channel
// node not the payload type. `type` may be an array in which case an array of
// ChannelReferences is returned. `channel_instance_allocator`, if specified, is
// called to set the instance ID of each ChannelReference as they are created.
absl::StatusOr<InterpValue> CreateChannelReference(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator =
        std::nullopt);

// Creates a ChannelReference or ChannelArray InterpValue. `type` is the type of
// the channel node not the payload type. `type` may be an array in which case a
// ChannelArray InterpValue is returned (recursively handling nested arrays).
// `channel_instance_allocator`, if specified, is called to set the instance ID
// of each channel as they are created. `definer`, if specified, sets the AST
// definition node associated with the channel reference or channel array.
absl::StatusOr<InterpValue> CreateChannelReferenceOrArray(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator =
        std::nullopt,
    std::optional<const AstNode*> definer = std::nullopt);

// Creates a pair of ChannelReference InterpValues. The first element has
// channel direction "out" while the second element has channel direction
// "in". As with `CreateChannelReference` this function can produce arrays of
// channel references (or arrays of arrays, etc). Corresponding
// ChannelReferences in the first and second elements will have the same channel
// instance id (if any). This is similar in form to what a DSLX channel
// declaration produces. For example,
//
//   let (foo_s, foo_r) = chan<u32>("foo");
absl::StatusOr<std::pair<InterpValue, InterpValue>> CreateChannelReferencePair(
    const Type* type,
    std::optional<absl::FunctionRef<int64_t()>> channel_instance_allocator =
        std::nullopt,
    std::optional<const AstNode*> definer = std::nullopt);

// Gets the definer of the given channel or channel array.
const AstNode* GetChannelOrArrayDefiner(const InterpValue& channel_or_array);

// Gets the ID of a channel or channel array.
int64_t GetChannelOrArrayId(const InterpValue& channel_or_array);

// Gets the direction of a channel or channel array.
ChannelDirection GetChannelOrArrayDirection(
    const InterpValue& channel_or_array);

// Returns all leaf ChannelReference InterpValues from a ChannelReference or
// ChannelArray InterpValue in row-major order.
std::vector<InterpValue> GetLeafChannelReferences(
    const InterpValue& channel_or_array);

// Formats an InterpValue to a string according to format preference.
absl::StatusOr<std::string> FormatInterpValue(const InterpValue& value,
                                              FormatPreference preference);

}  // namespace xls::dslx

#endif  // XLS_DSLX_INTERP_VALUE_UTILS_H_
