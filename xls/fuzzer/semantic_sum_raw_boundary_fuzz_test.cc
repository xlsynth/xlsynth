// Copyright 2026 The XLS Authors
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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/fuzzing/fuzztest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/value.h"

namespace xls {
namespace {

struct RawBoundaryContext {
  std::unique_ptr<dslx::ImportData> import_data;
  dslx::TypecheckedModule tm;
  const dslx::SumType* sum_type;
  std::string enum_variant_name;
  int64_t enum_payload_index;
  std::vector<Bits> declared_enum_member_bits;
  int64_t enum_bit_count;
};

std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

absl::StatusOr<RawBoundaryContext> PrepareContext(
    std::string_view program_text) {
  auto import_data =
      std::make_unique<dslx::ImportData>(dslx::CreateImportDataForTest());
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(program_text, "raw_boundary.x", "raw_boundary",
                              import_data.get()));
  XLS_ASSIGN_OR_RETURN(dslx::Function * function,
                       tm.module->GetMemberOrError<dslx::Function>("main"));
  XLS_ASSIGN_OR_RETURN(dslx::FunctionType * function_type,
                       tm.type_info->GetItemAs<dslx::FunctionType>(function));
  if (function_type->params().empty()) {
    return absl::InvalidArgumentError(
        "Raw-boundary sample did not expose a function parameter.");
  }
  auto* sum_type =
      dynamic_cast<const dslx::SumType*>(function_type->params().front().get());
  if (sum_type == nullptr) {
    return absl::InvalidArgumentError(
        "Raw-boundary sample parameter is not a sum type.");
  }

  for (const dslx::SumTypeVariant& variant : sum_type->variants()) {
    for (int64_t i = 0; i < variant.size(); ++i) {
      auto* enum_type =
          dynamic_cast<const dslx::EnumType*>(&variant.GetMemberType(i));
      if (enum_type == nullptr) {
        continue;
      }
      XLS_ASSIGN_OR_RETURN(std::vector<Bits> declared_enum_member_bits,
                           dslx::GetDeclaredEnumMemberBits(*enum_type));
      XLS_ASSIGN_OR_RETURN(int64_t enum_bit_count,
                           enum_type->size().GetAsInt64());
      return RawBoundaryContext{
          .import_data = std::move(import_data),
          .tm = tm,
          .sum_type = sum_type,
          .enum_variant_name = variant.variant().identifier(),
          .enum_payload_index = i,
          .declared_enum_member_bits = std::move(declared_enum_member_bits),
          .enum_bit_count = enum_bit_count,
      };
    }
  }
  return absl::InvalidArgumentError(
      "Raw-boundary sample did not contain an enum-typed payload variant.");
}

absl::StatusOr<const fuzzer::SemanticSumSeed*> FindSeed(
    const fuzzer::SemanticSumSeedManifest& manifest, std::string_view seed_id) {
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (seed.seed_id() == seed_id) {
      return &seed;
    }
  }
  return absl::NotFoundError(
      absl::StrCat("Could not find semantic-sum seed '", seed_id, "'."));
}

absl::StatusOr<RawBoundaryContext> LoadRawBoundaryContext(
    const std::filesystem::path& manifest_path) {
  XLS_ASSIGN_OR_RETURN(fuzzer::SemanticSumSeedManifest manifest,
                       LoadSemanticSumSeedManifest(manifest_path));
  XLS_ASSIGN_OR_RETURN(const fuzzer::SemanticSumSeed* program_seed,
                       FindSeed(manifest, "raw_boundary_valid_enum_payload"));
  XLS_ASSIGN_OR_RETURN(std::string program_text,
                       ReadSemanticSumSeedText(manifest_path, *program_seed));
  return PrepareContext(program_text);
}

absl::StatusOr<Bits> ExtractEnumPayloadBitsFromRawValue(
    const RawBoundaryContext& context, const Value& raw_value) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::Phase1SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(context.enum_variant_name));
  const Value& payload_tuple = raw_value.elements().at(1);
  const Value& payload_value = payload_tuple.elements().at(
      variant.payload_start + context.enum_payload_index);
  return payload_value.bits();
}

absl::StatusOr<dslx::InterpValue> MakeSemanticEnumPayloadValue(
    const RawBoundaryContext& context, const Bits& bits) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::Phase1SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(context.enum_variant_name));
  auto* enum_type = dynamic_cast<const dslx::EnumType*>(
      &variant.variant->GetMemberType(context.enum_payload_index));
  if (enum_type == nullptr) {
    return absl::InvalidArgumentError(
        "Enum payload variant no longer has enum type.");
  }
  XLS_ASSIGN_OR_RETURN(
      dslx::InterpValue bits_value,
      dslx::InterpValue::MakeBits(dslx::InterpValueTag::kUBits, bits));
  return dslx::CastBitsToEnum(bits_value, *enum_type);
}

absl::StatusOr<Value> MakeInvalidEnumRawValue(const RawBoundaryContext& context,
                                              uint64_t invalid_member_value) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::Phase1SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(context.enum_variant_name));
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  std::vector<Value> payload_slots;
  payload_slots.reserve(encoding.payload_slot_count());
  XLS_RETURN_IF_ERROR(encoding.VisitPayloadAssemblyOrder(
      variant,
      [&](int64_t active_index) -> absl::Status {
        if (active_index == context.enum_payload_index) {
          payload_slots.push_back(
              Value(UBits(invalid_member_value, context.enum_bit_count)));
        } else {
          XLS_ASSIGN_OR_RETURN(
              dslx::InterpValue zero,
              dslx::CreateZeroValueFromType(
                  variant.variant->GetMemberType(active_index)));
          XLS_ASSIGN_OR_RETURN(Value zero_value, zero.ConvertToIr());
          payload_slots.push_back(std::move(zero_value));
        }
        return absl::OkStatus();
      },
      [&](const dslx::Type& inactive_type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(dslx::InterpValue zero,
                             dslx::CreateZeroValueFromType(inactive_type));
        XLS_ASSIGN_OR_RETURN(Value zero_value, zero.ConvertToIr());
        payload_slots.push_back(std::move(zero_value));
        return absl::OkStatus();
      }));
  return Value::TupleOwned(
      std::vector<Value>{Value(UBits(variant.variant_index, tag_bit_count)),
                         Value::TupleOwned(std::move(payload_slots))});
}

absl::StatusOr<Value> MakeInvalidTagRawValue(const RawBoundaryContext& context,
                                             uint16_t payload_bits) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  std::vector<Value> payload_slots;
  payload_slots.reserve(encoding.payload_slot_count());
  XLS_RETURN_IF_ERROR(encoding.ForEachPayloadType(
      [&](const dslx::Type& slot_type) -> absl::Status {
        XLS_ASSIGN_OR_RETURN(dslx::InterpValue zero,
                             dslx::CreateZeroValueFromType(slot_type));
        XLS_ASSIGN_OR_RETURN(Value zero_value, zero.ConvertToIr());
        payload_slots.push_back(std::move(zero_value));
        return absl::OkStatus();
      }));
  if (payload_slots.empty()) {
    return absl::FailedPreconditionError(
        "Raw-boundary fixture did not contain payload slots.");
  }
  payload_slots.back() = Value(UBits(payload_bits, 16));
  return Value::TupleOwned(std::vector<Value>{
      Value(UBits(context.sum_type->variant_count(), tag_bit_count)),
      Value::TupleOwned(std::move(payload_slots))});
}

absl::Status VerifyManifestRawSeed(const RawBoundaryContext& context,
                                   const fuzzer::SemanticSumSeed& seed) {
  XLS_ASSIGN_OR_RETURN(Value raw_value,
                       Parser::ParseTypedValue(seed.raw_ir_value_text()));
  absl::StatusOr<dslx::InterpValue> actual =
      dslx::ValueToInterpValue(raw_value, context.sum_type);
  if (seed.outcome() == fuzzer::SEMANTIC_SUM_SEED_OUTCOME_SHOULD_PASS) {
    if (!actual.ok()) {
      return actual.status();
    }
    XLS_ASSIGN_OR_RETURN(
        Bits enum_payload_bits,
        ExtractEnumPayloadBitsFromRawValue(context, raw_value));
    XLS_ASSIGN_OR_RETURN(
        dslx::InterpValue enum_value,
        MakeSemanticEnumPayloadValue(context, enum_payload_bits));
    XLS_ASSIGN_OR_RETURN(
        dslx::InterpValue expected,
        dslx::CreateSumValue(*context.sum_type, context.enum_variant_name,
                             {enum_value}));
    if (*actual != expected) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Raw-boundary seed '", seed.seed_id(), "' produced semantic value '",
          actual->ToString(), "' but expected '", expected.ToString(), "'."));
    }
    return absl::OkStatus();
  }

  if (actual.ok()) {
    return absl::FailedPreconditionError(
        absl::StrCat("Raw-boundary seed '", seed.seed_id(),
                     "' unexpectedly converted successfully."));
  }
  if (!seed.expected_diagnostic_substr().empty() &&
      !absl::StrContains(actual.status().message(),
                         seed.expected_diagnostic_substr())) {
    return absl::FailedPreconditionError(
        absl::StrCat("Raw-boundary seed '", seed.seed_id(),
                     "' failed with unexpected diagnostic: ", actual.status()));
  }
  return absl::OkStatus();
}

absl::Status VerifyDeclaredEnumRoundtrip(const RawBoundaryContext& context,
                                         uint64_t member_index) {
  XLS_ASSIGN_OR_RETURN(
      dslx::InterpValue enum_value,
      MakeSemanticEnumPayloadValue(
          context, context.declared_enum_member_bits.at(member_index)));
  XLS_ASSIGN_OR_RETURN(
      dslx::InterpValue semantic_value,
      dslx::CreateSumValue(*context.sum_type, context.enum_variant_name,
                           {enum_value}));
  XLS_ASSIGN_OR_RETURN(Value raw_value, semantic_value.ConvertToIr());
  XLS_ASSIGN_OR_RETURN(dslx::InterpValue roundtrip,
                       dslx::ValueToInterpValue(raw_value, context.sum_type));
  if (roundtrip != semantic_value) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Semantic raw roundtrip mismatch: '", semantic_value.ToString(),
        "' vs '", roundtrip.ToString(), "'."));
  }
  return absl::OkStatus();
}

absl::Status VerifyUndeclaredEnumPayloadRejected(
    const RawBoundaryContext& context, uint64_t invalid_member_value) {
  XLS_ASSIGN_OR_RETURN(Value raw_value,
                       MakeInvalidEnumRawValue(context, invalid_member_value));
  absl::StatusOr<dslx::InterpValue> actual =
      dslx::ValueToInterpValue(raw_value, context.sum_type);
  if (actual.ok()) {
    return absl::FailedPreconditionError(
        "Undeclared enum payload unexpectedly converted successfully.");
  }
  if (!absl::StrContains(actual.status().message(), "declared member")) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Undeclared enum payload failed with unexpected diagnostic: ",
        actual.status()));
  }
  return absl::OkStatus();
}

absl::Status VerifyInvalidTagRejected(const RawBoundaryContext& context,
                                      uint16_t payload_bits) {
  XLS_ASSIGN_OR_RETURN(Value raw_value,
                       MakeInvalidTagRawValue(context, payload_bits));
  absl::StatusOr<dslx::InterpValue> actual =
      dslx::ValueToInterpValue(raw_value, context.sum_type);
  if (actual.ok()) {
    return absl::FailedPreconditionError(
        "Invalid sum tag unexpectedly converted successfully.");
  }
  if (!absl::StrContains(actual.status().message(), "invalid tag")) {
    return absl::FailedPreconditionError(
        absl::StrCat("Invalid sum tag failed with unexpected diagnostic: ",
                     actual.status()));
  }
  return absl::OkStatus();
}

TEST(SemanticSumRawBoundaryFuzzTest, ReplaysManifestCases) {
  std::filesystem::path manifest_path = GetManifestPath();
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(manifest_path));
  int64_t verified = 0;
  XLS_ASSERT_OK(ReplaySemanticSumSeeds(
      manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_RAW_BOUNDARY,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string&) -> absl::Status {
        ++verified;
        return VerifyManifestRawSeed(context, seed);
      }));
  EXPECT_EQ(verified, 3);
}

void DeclaredEnumPayloadRoundtrips(uint64_t member_index) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(VerifyDeclaredEnumRoundtrip(context, member_index));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, DeclaredEnumPayloadRoundtrips)
    .WithDomains(fuzztest::ElementOf<uint64_t>({0, 1}));

void UndeclaredEnumPayloadIsRejected(uint64_t invalid_member_value) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(
      VerifyUndeclaredEnumPayloadRejected(context, invalid_member_value));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, UndeclaredEnumPayloadIsRejected)
    .WithDomains(fuzztest::ElementOf<uint64_t>({2, 3}));

void InvalidSumTagIsRejected(uint16_t payload_bits) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(VerifyInvalidTagRejected(context, payload_bits));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, InvalidSumTagIsRejected)
    .WithDomains(fuzztest::Arbitrary<uint16_t>());

}  // namespace
}  // namespace xls
