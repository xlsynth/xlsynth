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

#include "xls/fuzzer/semantic_sum_raw_boundary_fuzz.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/stopwatch.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/ir/format_preference.h"
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

absl::Status CheckOrCreateWritableDirectory(const std::filesystem::path& path) {
  XLS_RETURN_IF_ERROR(RecursivelyCreateDir(path));
  if (!std::filesystem::is_directory(path)) {
    return absl::InvalidArgumentError(
        absl::StrCat(path.string(), " is not a directory"));
  }
  return absl::OkStatus();
}

absl::Status WriteFailureArtifacts(const std::filesystem::path& artifact_dir,
                                   std::string_view case_name,
                                   std::string_view program_text,
                                   std::string_view raw_value_text,
                                   const absl::Status& status,
                                   int64_t sequence_number) {
  std::filesystem::path failure_dir =
      artifact_dir / absl::StrCat(case_name, "_", sequence_number);
  XLS_RETURN_IF_ERROR(RecursivelyCreateDir(failure_dir));
  XLS_RETURN_IF_ERROR(
      SetFileContents(failure_dir / "sample.x", std::string(program_text)));
  XLS_RETURN_IF_ERROR(
      SetFileContents(failure_dir / "raw_value.txt", std::string(raw_value_text)));
  XLS_RETURN_IF_ERROR(
      SetFileContents(failure_dir / "diagnostic.txt", status.ToString()));
  return absl::OkStatus();
}

absl::StatusOr<RawBoundaryContext> PrepareContext(std::string_view program_text) {
  auto import_data = std::make_unique<dslx::ImportData>(
      dslx::CreateImportDataForTest());
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
    return absl::InvalidArgumentError("Raw-boundary sample parameter is not a sum type.");
  }

  std::string enum_variant_name;
  int64_t enum_payload_index = -1;
  std::vector<Bits> declared_enum_member_bits;
  int64_t enum_bit_count = 0;
  for (const dslx::SumTypeVariant& variant : sum_type->variants()) {
    for (int64_t i = 0; i < variant.size(); ++i) {
      auto* enum_type = dynamic_cast<const dslx::EnumType*>(&variant.GetMemberType(i));
      if (enum_type == nullptr) {
        continue;
      }
      XLS_ASSIGN_OR_RETURN(declared_enum_member_bits,
                           dslx::GetDeclaredEnumMemberBits(*enum_type));
      XLS_ASSIGN_OR_RETURN(enum_bit_count, enum_type->size().GetAsInt64());
      enum_variant_name = variant.variant().identifier();
      enum_payload_index = i;
      return RawBoundaryContext{
          .import_data = std::move(import_data),
          .tm = tm,
          .sum_type = sum_type,
          .enum_variant_name = std::move(enum_variant_name),
          .enum_payload_index = enum_payload_index,
          .declared_enum_member_bits = std::move(declared_enum_member_bits),
          .enum_bit_count = enum_bit_count,
      };
    }
  }
  return absl::InvalidArgumentError(
      "Raw-boundary sample did not contain an enum-typed payload variant.");
}

absl::StatusOr<Bits> ExtractEnumPayloadBitsFromRawValue(
    const RawBoundaryContext& context, const Value& raw_value) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::Phase1SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(context.enum_variant_name));
  const Value& payload_tuple = raw_value.elements().at(1);
  const Value& payload_value =
      payload_tuple.elements().at(variant.payload_start + context.enum_payload_index);
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
    return absl::InvalidArgumentError("Enum payload variant no longer has enum type.");
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
          payload_slots.push_back(Value(UBits(invalid_member_value, context.enum_bit_count)));
        } else {
          XLS_ASSIGN_OR_RETURN(dslx::InterpValue zero,
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

absl::Status VerifyManifestRawSeed(const RawBoundaryContext& context,
                                   const fuzzer::SemanticSumSeed& seed,
                                   std::string_view program_text,
                                   const std::filesystem::path& artifact_dir,
                                   int64_t sequence_number) {
  XLS_ASSIGN_OR_RETURN(Value raw_value,
                       Parser::ParseTypedValue(seed.raw_ir_value_text()));
  absl::StatusOr<dslx::InterpValue> actual =
      dslx::ValueToInterpValue(raw_value, context.sum_type);
  if (seed.outcome() == fuzzer::SEMANTIC_SUM_SEED_OUTCOME_SHOULD_PASS) {
    if (!actual.ok()) {
      XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
          artifact_dir, seed.seed_id(), program_text, seed.raw_ir_value_text(),
          actual.status(), sequence_number));
      return actual.status();
    }
    XLS_ASSIGN_OR_RETURN(
        Bits enum_payload_bits, ExtractEnumPayloadBitsFromRawValue(context, raw_value));
    XLS_ASSIGN_OR_RETURN(dslx::InterpValue enum_value,
                         MakeSemanticEnumPayloadValue(context, enum_payload_bits));
    XLS_ASSIGN_OR_RETURN(dslx::InterpValue expected,
                         dslx::CreateSumValue(*context.sum_type,
                                              context.enum_variant_name,
                                              {enum_value}));
    if (*actual != expected) {
      absl::Status mismatch = absl::FailedPreconditionError(absl::StrCat(
          "Raw-boundary seed `", seed.seed_id(),
          "` produced semantic value `", actual->ToString(),
          "` but expected `", expected.ToString(), "`."));
      XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
          artifact_dir, seed.seed_id(), program_text, seed.raw_ir_value_text(),
          mismatch, sequence_number));
      return mismatch;
    }
    return absl::OkStatus();
  }

  if (actual.ok()) {
    absl::Status unexpected = absl::FailedPreconditionError(absl::StrCat(
        "Raw-boundary seed `", seed.seed_id(),
        "` unexpectedly converted successfully."));
    XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
        artifact_dir, seed.seed_id(), program_text, seed.raw_ir_value_text(),
        unexpected, sequence_number));
    return unexpected;
  }
  if (!seed.expected_diagnostic_substr().empty() &&
      !absl::StrContains(actual.status().message(),
                         seed.expected_diagnostic_substr())) {
    XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
        artifact_dir, seed.seed_id(), program_text, seed.raw_ir_value_text(),
        actual.status(), sequence_number));
    return absl::FailedPreconditionError(absl::StrCat(
        "Raw-boundary seed `", seed.seed_id(),
        "` failed with unexpected diagnostic: ", actual.status()));
  }
  return absl::OkStatus();
}

absl::Status VerifyValidRoundtrip(const RawBoundaryContext& context,
                                  absl::BitGenRef bit_gen,
                                  const std::filesystem::path& artifact_dir,
                                  int64_t sequence_number) {
  size_t member_index = absl::Uniform<size_t>(bit_gen, 0,
                                              context.declared_enum_member_bits.size());
  XLS_ASSIGN_OR_RETURN(
      dslx::InterpValue enum_value,
      MakeSemanticEnumPayloadValue(context,
                                   context.declared_enum_member_bits.at(member_index)));
  XLS_ASSIGN_OR_RETURN(dslx::InterpValue semantic_value,
                       dslx::CreateSumValue(*context.sum_type,
                                            context.enum_variant_name,
                                            {enum_value}));
  XLS_ASSIGN_OR_RETURN(Value raw_value, semantic_value.ConvertToIr());
  XLS_ASSIGN_OR_RETURN(dslx::InterpValue roundtrip,
                       dslx::ValueToInterpValue(raw_value, context.sum_type));
  if (roundtrip != semantic_value) {
    absl::Status mismatch = absl::FailedPreconditionError(
        absl::StrCat("Semantic raw roundtrip mismatch: `",
                     semantic_value.ToString(), "` vs `",
                     roundtrip.ToString(), "`."));
    XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
        artifact_dir, "valid_roundtrip", "<generated>",
        raw_value.ToString(FormatPreference::kHex), mismatch, sequence_number));
    return mismatch;
  }
  return absl::OkStatus();
}

absl::Status VerifyInvalidBoundaryCase(const RawBoundaryContext& context,
                                       absl::BitGenRef bit_gen,
                                       const std::filesystem::path& artifact_dir,
                                       int64_t sequence_number) {
  const dslx::Phase1SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  bool mutate_tag = absl::Bernoulli(bit_gen, 0.5);
  Value raw_value;
  if (mutate_tag) {
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
    raw_value = Value::TupleOwned(
        std::vector<Value>{Value(UBits(context.sum_type->variant_count(),
                                       tag_bit_count)),
                           Value::TupleOwned(std::move(payload_slots))});
  } else {
    uint64_t invalid_enum_value = 0;
    while (true) {
      invalid_enum_value = absl::Uniform<uint64_t>(
          bit_gen, 0, uint64_t{1} << context.enum_bit_count);
      bool is_declared = false;
      for (const Bits& bits : context.declared_enum_member_bits) {
        if (bits.ToUint64().value() == invalid_enum_value) {
          is_declared = true;
          break;
        }
      }
      if (!is_declared) {
        break;
      }
    }
    XLS_ASSIGN_OR_RETURN(raw_value,
                         MakeInvalidEnumRawValue(context, invalid_enum_value));
  }
  absl::StatusOr<dslx::InterpValue> actual =
      dslx::ValueToInterpValue(raw_value, context.sum_type);
  if (actual.ok()) {
    absl::Status unexpected = absl::FailedPreconditionError(
        "Invalid raw boundary case unexpectedly converted successfully.");
    XLS_RETURN_IF_ERROR(WriteFailureArtifacts(
        artifact_dir, "invalid_boundary", "<generated>",
        raw_value.ToString(FormatPreference::kHex), unexpected,
        sequence_number));
    return unexpected;
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<SemanticSumRawBoundaryFuzzStats> RunSemanticSumRawBoundaryFuzz(
    const SemanticSumRawBoundaryFuzzOptions& options) {
  if (!options.duration.has_value() && !options.iteration_count.has_value()) {
    return absl::InvalidArgumentError(
        "Raw-boundary fuzz options require duration or iteration_count.");
  }
  XLS_RETURN_IF_ERROR(CheckOrCreateWritableDirectory(options.artifact_dir));
  XLS_ASSIGN_OR_RETURN(fuzzer::SemanticSumSeedManifest manifest,
                       LoadSemanticSumSeedManifest(options.manifest_path));

  std::optional<const fuzzer::SemanticSumSeed*> program_seed;
  for (const fuzzer::SemanticSumSeed& seed : manifest.seeds()) {
    if (seed.seed_id() == "raw_boundary_valid_enum_payload") {
      program_seed = &seed;
      break;
    }
  }
  if (!program_seed.has_value()) {
    return absl::NotFoundError(
        "Could not find raw_boundary_valid_enum_payload seed.");
  }
  XLS_ASSIGN_OR_RETURN(std::string program_text,
                       ReadSemanticSumSeedText(options.manifest_path,
                                               **program_seed));
  XLS_ASSIGN_OR_RETURN(RawBoundaryContext context, PrepareContext(program_text));

  SemanticSumRawBoundaryFuzzStats stats;
  XLS_RETURN_IF_ERROR(ReplaySemanticSumSeeds(
      options.manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_RAW_BOUNDARY,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string&) -> absl::Status {
        ++stats.manifest_seed_cases_verified;
        return VerifyManifestRawSeed(context, seed, program_text,
                                     options.artifact_dir,
                                     stats.manifest_seed_cases_verified);
      }));

  absl::BitGen nondeterministic_gen;
  std::mt19937_64 deterministic_gen(options.seed.value_or(0));
  absl::BitGenRef bit_gen = options.seed.has_value()
                                ? absl::BitGenRef(deterministic_gen)
                                : absl::BitGenRef(nondeterministic_gen);
  Stopwatch stopwatch;
  while (true) {
    if (options.iteration_count.has_value() &&
        stats.valid_roundtrips_verified + stats.invalid_rejections_verified >=
            *options.iteration_count) {
      break;
    }
    if (options.duration.has_value() &&
        stopwatch.GetElapsedTime() >= *options.duration) {
      break;
    }
    bool do_valid = absl::Bernoulli(bit_gen, 0.5);
    if (do_valid) {
      XLS_RETURN_IF_ERROR(VerifyValidRoundtrip(
          context, bit_gen, options.artifact_dir,
          stats.manifest_seed_cases_verified + stats.valid_roundtrips_verified +
              stats.invalid_rejections_verified + 1));
      ++stats.valid_roundtrips_verified;
    } else {
      XLS_RETURN_IF_ERROR(VerifyInvalidBoundaryCase(
          context, bit_gen, options.artifact_dir,
          stats.manifest_seed_cases_verified + stats.valid_roundtrips_verified +
              stats.invalid_rejections_verified + 1));
      ++stats.invalid_rejections_verified;
    }
  }

  XLS_RETURN_IF_ERROR(SetFileContents(
      options.artifact_dir / "summary.txt",
      absl::StrCat("manifest_seed_cases_verified=",
                   stats.manifest_seed_cases_verified,
                   "\nvalid_roundtrips_verified=", stats.valid_roundtrips_verified,
                   "\ninvalid_rejections_verified=",
                   stats.invalid_rejections_verified, "\n")));
  return stats;
}

}  // namespace xls
