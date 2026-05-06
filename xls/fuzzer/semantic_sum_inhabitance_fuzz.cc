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

#include "xls/fuzzer/semantic_sum_inhabitance_fuzz.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/stopwatch.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/fuzzer/value_generator.h"
#include "xls/ir/value.h"

namespace xls {
namespace {

absl::Status CheckOrCreateWritableDirectory(const std::filesystem::path& path) {
  XLS_RETURN_IF_ERROR(RecursivelyCreateDir(path));
  if (!std::filesystem::is_directory(path)) {
    return absl::InvalidArgumentError(
        absl::StrCat(path.string(), " is not a directory"));
  }
  return absl::OkStatus();
}

absl::StatusOr<bool> TypeIsInhabited(const dslx::Type& type);

absl::StatusOr<bool> SumVariantIsInhabited(const dslx::SumTypeVariant& variant) {
  for (int64_t i = 0; i < variant.size(); ++i) {
    XLS_ASSIGN_OR_RETURN(bool member_is_inhabited,
                         TypeIsInhabited(variant.GetMemberType(i)));
    if (!member_is_inhabited) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<bool> TypeIsInhabited(const dslx::Type& type) {
  if (dslx::GetBitsLike(type).has_value()) {
    return true;
  }
  if (auto* enum_type = dynamic_cast<const dslx::EnumType*>(&type)) {
    return !enum_type->members().empty();
  }
  if (auto* tuple_type = dynamic_cast<const dslx::TupleType*>(&type)) {
    for (const std::unique_ptr<dslx::Type>& member : tuple_type->members()) {
      XLS_ASSIGN_OR_RETURN(bool member_is_inhabited, TypeIsInhabited(*member));
      if (!member_is_inhabited) {
        return false;
      }
    }
    return true;
  }
  if (auto* array_type = dynamic_cast<const dslx::ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t size, array_type->size().GetAsInt64());
    if (size == 0) {
      return true;
    }
    return TypeIsInhabited(array_type->element_type());
  }
  if (auto* sum_type = dynamic_cast<const dslx::SumType*>(&type)) {
    for (const dslx::SumTypeVariant& variant : sum_type->variants()) {
      XLS_ASSIGN_OR_RETURN(bool variant_is_inhabited,
                           SumVariantIsInhabited(variant));
      if (variant_is_inhabited) {
        return true;
      }
    }
    return false;
  }
  return true;
}

absl::Status VerifyGeneratedValue(const dslx::Type& type,
                                  const dslx::InterpValue& value);

absl::Status VerifyGeneratedSumValue(const dslx::SumType& sum_type,
                                     const dslx::InterpValue& value) {
  const dslx::Phase1SumTypeEncoding encoding(sum_type);
  const std::vector<dslx::InterpValue>& elements = value.GetValuesOrDie();
  if (elements.size() != 2) {
    return absl::FailedPreconditionError("Generated sum value was not encoded as a pair.");
  }
  XLS_ASSIGN_OR_RETURN(uint64_t variant_index, elements.at(0).GetBitValueUnsigned());
  if (variant_index >= sum_type.variant_count()) {
    return absl::FailedPreconditionError("Generated sum selected an invalid variant index.");
  }
  const dslx::SumTypeVariant& variant = sum_type.variants().at(variant_index);
  XLS_ASSIGN_OR_RETURN(bool variant_is_inhabited, SumVariantIsInhabited(variant));
  if (!variant_is_inhabited) {
    return absl::FailedPreconditionError(
        absl::StrCat("Generated uninhabited sum variant `",
                     variant.variant().identifier(), "`."));
  }
  const std::vector<dslx::InterpValue>& payload_slots =
      elements.at(1).GetValuesOrDie();
  XLS_ASSIGN_OR_RETURN(dslx::Phase1SumTypeEncoding::VariantInfo variant_info,
                       encoding.GetVariant(variant.variant().identifier()));
  XLS_RETURN_IF_ERROR(encoding.ForEachActivePayloadSlot(
      variant_info,
      [&](int64_t slot_index, int64_t active_index,
          const dslx::Type& member_type) -> absl::Status {
        return VerifyGeneratedValue(member_type, payload_slots.at(slot_index));
      }));
  return absl::OkStatus();
}

absl::Status VerifyGeneratedValue(const dslx::Type& type,
                                  const dslx::InterpValue& value) {
  if (auto* tuple_type = dynamic_cast<const dslx::TupleType*>(&type)) {
    const std::vector<dslx::InterpValue>& members = value.GetValuesOrDie();
    for (int64_t i = 0; i < tuple_type->size(); ++i) {
      XLS_RETURN_IF_ERROR(
          VerifyGeneratedValue(tuple_type->GetMemberType(i), members.at(i)));
    }
    return absl::OkStatus();
  }
  if (auto* array_type = dynamic_cast<const dslx::ArrayType*>(&type)) {
    for (const dslx::InterpValue& element : value.GetValuesOrDie()) {
      XLS_RETURN_IF_ERROR(VerifyGeneratedValue(array_type->element_type(), element));
    }
    return absl::OkStatus();
  }
  if (auto* sum_type = dynamic_cast<const dslx::SumType*>(&type)) {
    return VerifyGeneratedSumValue(*sum_type, value);
  }
  XLS_ASSIGN_OR_RETURN(Value raw_value, value.ConvertToIr());
  XLS_ASSIGN_OR_RETURN(dslx::InterpValue roundtrip,
                       dslx::ValueToInterpValue(raw_value, &type));
  if (roundtrip != value) {
    return absl::FailedPreconditionError(
        absl::StrCat("Generated value did not roundtrip through raw form: `",
                     value.ToString(), "` vs `", roundtrip.ToString(), "`."));
  }
  return absl::OkStatus();
}

absl::StatusOr<dslx::TypeRefTypeAnnotation*> MakeTypeAnnotation(
    dslx::Module* module, std::string_view name) {
  XLS_ASSIGN_OR_RETURN(dslx::TypeDefinition type_definition,
                       module->GetTypeDefinition(name));
  auto* type_ref = module->Make<dslx::TypeRef>(dslx::FakeSpan(), type_definition);
  return module->Make<dslx::TypeRefTypeAnnotation>(
      dslx::FakeSpan(), type_ref, std::vector<dslx::ExprOrType>{});
}

absl::StatusOr<dslx::SumType> MakePartiallyInhabitedEnumPayloadSumType(
    dslx::Module& module) {
  const dslx::Span kFakeSpan = dslx::FakeSpan();

  auto* enum_name = module.Make<dslx::NameDef>(kFakeSpan, "Empty", nullptr);
  auto* u2_type = module.Make<dslx::BuiltinTypeAnnotation>(
      kFakeSpan, dslx::BuiltinType::kU2,
      module.GetOrCreateBuiltinNameDef(dslx::BuiltinType::kU2));
  auto* enum_def = module.Make<dslx::EnumDef>(
      kFakeSpan, enum_name, u2_type, std::vector<dslx::EnumMember>{},
      /*is_public=*/false);
  enum_name->set_definer(enum_def);
  XLS_RETURN_IF_ERROR(module.AddTop(enum_def, /*make_collision_error=*/nullptr));

  auto* sum_name =
      module.Make<dslx::NameDef>(kFakeSpan, "MaybeImpossible", nullptr);
  auto* unit_name = module.Make<dslx::NameDef>(kFakeSpan, "Unit", nullptr);
  auto* impossible_name =
      module.Make<dslx::NameDef>(kFakeSpan, "Impossible", nullptr);
  auto* unit_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, unit_name, dslx::SumVariant::PayloadKind::kUnit,
      std::vector<dslx::TypeAnnotation*>{},
      std::vector<dslx::StructMemberNode*>{});
  XLS_ASSIGN_OR_RETURN(dslx::TypeRefTypeAnnotation * enum_type_annotation,
                       MakeTypeAnnotation(&module, "Empty"));
  auto* impossible_variant = module.Make<dslx::SumVariant>(
      kFakeSpan, impossible_name, dslx::SumVariant::PayloadKind::kTuple,
      std::vector<dslx::TypeAnnotation*>{enum_type_annotation},
      std::vector<dslx::StructMemberNode*>{});
  auto* sum_def = module.Make<dslx::SumDef>(
      kFakeSpan, sum_name, std::vector<dslx::ParametricBinding*>{},
      std::vector<dslx::SumVariant*>{unit_variant, impossible_variant},
      /*is_public=*/false);
  sum_name->set_definer(sum_def);
  XLS_RETURN_IF_ERROR(module.AddTop(sum_def, /*make_collision_error=*/nullptr));

  std::vector<dslx::SumTypeVariant> variants;
  variants.push_back(dslx::SumTypeVariant::MakeUnit(*unit_variant));
  std::vector<std::unique_ptr<dslx::Type>> payload_members;
  payload_members.push_back(std::make_unique<dslx::EnumType>(
      *enum_def, dslx::TypeDim::CreateU32(2), /*is_signed=*/false,
      std::vector<dslx::InterpValue>{}));
  variants.push_back(dslx::SumTypeVariant::MakeTuple(
      *impossible_variant, std::move(payload_members)));
  return dslx::SumType(*sum_def, std::move(variants));
}

absl::StatusOr<dslx::Function*> GetEntryFunction(dslx::Module& module) {
  auto functions = module.GetFunctionByName();
  auto it = functions.find("main");
  if (it != functions.end()) {
    return it->second;
  }
  if (functions.size() == 1) {
    return functions.begin()->second;
  }
  return absl::NotFoundError(absl::StrCat(
      "Expected a `main` function or exactly one top-level function in module `",
      module.name(), "`"));
}

absl::StatusOr<std::vector<std::unique_ptr<dslx::Type>>> GetFunctionParamTypes(
    const std::string& seed_text, std::string_view seed_id) {
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSIGN_OR_RETURN(
      dslx::TypecheckedModule tm,
      dslx::ParseAndTypecheck(seed_text, absl::StrCat(seed_id, ".x"), seed_id,
                              &import_data));
  XLS_ASSIGN_OR_RETURN(dslx::Function * function, GetEntryFunction(*tm.module));
  XLS_ASSIGN_OR_RETURN(dslx::FunctionType * function_type,
                       tm.type_info->GetItemAs<dslx::FunctionType>(function));
  std::vector<std::unique_ptr<dslx::Type>> params;
  for (const std::unique_ptr<dslx::Type>& param : function_type->params()) {
    params.push_back(param->CloneToUnique());
  }
  return params;
}

}  // namespace

absl::StatusOr<SemanticSumInhabitanceFuzzStats> RunSemanticSumInhabitanceFuzz(
    const SemanticSumInhabitanceFuzzOptions& options) {
  if (!options.duration.has_value() && !options.iteration_count.has_value()) {
    return absl::InvalidArgumentError(
        "Inhabitance fuzz options require duration or iteration_count.");
  }
  XLS_RETURN_IF_ERROR(CheckOrCreateWritableDirectory(options.artifact_dir));

  SemanticSumInhabitanceFuzzStats stats;
  absl::BitGen nondeterministic_gen;
  std::mt19937_64 deterministic_gen(options.seed.value_or(0));
  absl::BitGenRef bit_gen = options.seed.has_value()
                                ? absl::BitGenRef(deterministic_gen)
                                : absl::BitGenRef(nondeterministic_gen);

  XLS_RETURN_IF_ERROR(ReplaySemanticSumSeeds(
      options.manifest_path, fuzzer::SEMANTIC_SUM_SEED_SURFACE_INHABITANCE,
      [&](const fuzzer::SemanticSumSeed& seed,
          const std::string& seed_text) -> absl::Status {
        if (seed.outcome() != fuzzer::SEMANTIC_SUM_SEED_OUTCOME_SHOULD_PASS) {
          return absl::OkStatus();
        }
        XLS_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<dslx::Type>> params,
                             GetFunctionParamTypes(seed_text, seed.seed_id()));
        std::vector<const dslx::Type*> param_ptrs;
        param_ptrs.reserve(params.size());
        for (const std::unique_ptr<dslx::Type>& param : params) {
          param_ptrs.push_back(param.get());
        }
        for (int64_t i = 0; i < 4; ++i) {
          XLS_ASSIGN_OR_RETURN(std::vector<dslx::InterpValue> values,
                               GenerateInterpValues(bit_gen, param_ptrs));
          for (int64_t j = 0; j < values.size(); ++j) {
            XLS_RETURN_IF_ERROR(VerifyGeneratedValue(*params.at(j), values.at(j)));
            ++stats.seeded_values_verified;
          }
        }
        return absl::OkStatus();
      }));

  dslx::FileTable file_table;
  dslx::Module module("semantic_sum_inhabitance_fuzz", /*fs_path=*/std::nullopt,
                      file_table);
  XLS_ASSIGN_OR_RETURN(dslx::SumType partial_sum,
                       MakePartiallyInhabitedEnumPayloadSumType(module));
  std::vector<std::unique_ptr<dslx::Type>> tuple_members;
  tuple_members.push_back(partial_sum.CloneToUnique());
  tuple_members.push_back(dslx::BitsType::MakeU8());
  auto nested_tuple =
      std::make_unique<dslx::TupleType>(std::move(tuple_members));
  auto nested_array = std::make_unique<dslx::ArrayType>(
      partial_sum.CloneToUnique(), dslx::TypeDim::CreateU32(2));
  std::vector<const dslx::Type*> random_types = {
      &partial_sum, nested_tuple.get(), nested_array.get()};

  Stopwatch stopwatch;
  while (true) {
    if (options.iteration_count.has_value() &&
        stats.generated_values_verified >= *options.iteration_count) {
      break;
    }
    if (options.duration.has_value() &&
        stopwatch.GetElapsedTime() >= *options.duration) {
      break;
    }
    size_t index = absl::Uniform<size_t>(bit_gen, 0, random_types.size());
    XLS_ASSIGN_OR_RETURN(dslx::InterpValue value,
                         GenerateInterpValue(bit_gen, *random_types.at(index),
                                             absl::Span<const dslx::InterpValue>()));
    XLS_RETURN_IF_ERROR(VerifyGeneratedValue(*random_types.at(index), value));
    ++stats.generated_values_verified;
  }

  XLS_RETURN_IF_ERROR(SetFileContents(
      options.artifact_dir / "summary.txt",
      absl::StrCat("seeded_values_verified=", stats.seeded_values_verified,
                   "\ngenerated_values_verified=",
                   stats.generated_values_verified, "\n")));
  return stats;
}

}  // namespace xls
