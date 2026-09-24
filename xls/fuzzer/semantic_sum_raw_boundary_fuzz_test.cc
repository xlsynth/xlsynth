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

// Raw conversion seeds retain numeric-enum strictness. The generated property
// additionally executes bounded nested sum programs with literal wire images,
// independently checking transport, constructor padding, and observation depth
// in bytecode, the IR interpreter, and the JIT. Malformed images never pass
// through source-domain argument decoding.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include "xls/common/file/get_runfile_path.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/ir_convert/conversion_info.h"
#include "xls/dslx/ir_convert/convert_options.h"
#include "xls/dslx/ir_convert/function_converter.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type.h"
#include "xls/fuzzer/semantic_sum_seed_corpus.h"
#include "xls/interpreter/function_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/jit/function_jit.h"

namespace xls {
namespace {

// Keeps the parsed seed module alive and identifies its enum payload slot.
struct RawBoundaryContext {
  std::unique_ptr<dslx::ImportData> import_data;
  dslx::TypecheckedModule tm;
  const dslx::SumType* sum_type;
  std::string enum_variant_name;
  int64_t enum_payload_index;
  std::vector<Bits> declared_enum_member_bits;
  int64_t enum_bit_count;
};

// Resolves the checked-in corpus manifest from Bazel runfiles.
std::filesystem::path GetManifestPath() {
  return GetXlsRunfilePath(
             "xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto")
      .value();
}

// Parses one source module and finds the enum payload used by this test.
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
      std::vector<Bits> declared_enum_member_bits;
      declared_enum_member_bits.reserve(enum_type->members().size());
      for (const dslx::InterpValue& member : enum_type->members()) {
        declared_enum_member_bits.push_back(member.GetBitsOrDie());
      }
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

// Finds a named reviewed seed without depending on manifest position.
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

// Builds the fixed raw-boundary context from its reviewed source seed.
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

// Reads the enum payload bits from the active slot of a raw sum tuple.
absl::StatusOr<Bits> ExtractEnumPayloadBitsFromRawValue(
    const RawBoundaryContext& context, const Value& raw_value) {
  const Value& payload_tuple = raw_value.elements().at(1);
  const Value& payload_slot = payload_tuple.elements().at(0);
  return payload_slot.bits().Slice(0, context.enum_bit_count);
}

// Interprets payload bits as a declared member of the seed's enum type.
absl::StatusOr<dslx::InterpValue> MakeSemanticEnumPayloadValue(
    const RawBoundaryContext& context, const Bits& bits) {
  const dslx::SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::SumTypeEncoding::VariantInfo variant,
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

// Builds one otherwise well-formed raw sum with an undeclared enum payload.
absl::StatusOr<Value> MakeInvalidEnumRawValue(const RawBoundaryContext& context,
                                              uint64_t invalid_member_value) {
  const dslx::SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(dslx::SumTypeEncoding::VariantInfo variant,
                       encoding.GetVariant(context.enum_variant_name));
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  return Value::TupleOwned(std::vector<Value>{
      Value(UBits(variant.variant_index, tag_bit_count)),
      Value::TupleOwned(
          {Value(UBits(invalid_member_value, payload_slot_bit_count))})});
}

// Builds one raw sum with an out-of-range tag and arbitrary payload bits.
absl::StatusOr<Value> MakeMalformedTagRawValue(
    const RawBoundaryContext& context, uint16_t payload_bits) {
  const dslx::SumTypeEncoding encoding(*context.sum_type);
  XLS_ASSIGN_OR_RETURN(int64_t tag_bit_count, encoding.tag_bit_count());
  XLS_ASSIGN_OR_RETURN(int64_t payload_slot_bit_count,
                       encoding.payload_slot_bit_count());
  return Value::TupleOwned(std::vector<Value>{
      Value(UBits(context.sum_type->variant_count(), tag_bit_count)),
      Value::TupleOwned({Value(UBits(payload_bits, payload_slot_bit_count))})});
}

// Applies the manifest outcome contract to one reviewed raw IR value.
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
    const dslx::SumTypeEncoding encoding(*context.sum_type);
    absl::StatusOr<dslx::SumTypeEncoding::VariantInfo> variant =
        encoding.GetVariantByTagBits(raw_value.elements().at(0).bits());
    if (variant.status().code() == absl::StatusCode::kNotFound) {
      XLS_ASSIGN_OR_RETURN(Value roundtrip, actual->ConvertToIr());
      if (roundtrip != raw_value) {
        return absl::FailedPreconditionError(absl::StrCat(
            "Raw-boundary seed '", seed.seed_id(),
            "' did not preserve malformed raw bits through roundtrip."));
      }
      return absl::OkStatus();
    }
    XLS_RETURN_IF_ERROR(variant.status());
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

// Checks semantic-to-raw-to-semantic identity for one declared member.
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

// Checks that one undeclared enum encoding fails during raw conversion.
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

// Checks that one malformed tag round-trips without losing raw bits.
absl::Status VerifyMalformedTagPreservesRawImage(
    const RawBoundaryContext& context, uint16_t payload_bits) {
  XLS_ASSIGN_OR_RETURN(Value raw_value,
                       MakeMalformedTagRawValue(context, payload_bits));
  XLS_ASSIGN_OR_RETURN(dslx::InterpValue semantic_value,
                       dslx::ValueToInterpValue(raw_value, context.sum_type));
  XLS_ASSIGN_OR_RETURN(Value roundtrip, semantic_value.ConvertToIr());
  if (roundtrip != raw_value) {
    return absl::FailedPreconditionError(
        "Malformed raw boundary tag did not preserve its raw image.");
  }
  return absl::OkStatus();
}

// Verifies: reviewed raw-boundary seeds convert or reject as declared.
// Catches: changed conversion or diagnostic behavior for named raw fixtures.
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

// Sparse, out-of-order encodings distinguish member values from their indexes.
TEST(SemanticSumRawBoundaryFuzzTest, PreparedContextUsesDeclaredEnumValues) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context, PrepareContext(R"(
    enum Flavor: u3 { High = 5, Low = 1 }
    enum Choice { None, FlavorChoice(Flavor), Wide(u16) }
    fn main(x: Choice) -> bool { x == x }
  )"));
  const std::vector<Bits> expected = {UBits(5, 3), UBits(1, 3)};
  EXPECT_EQ(context.declared_enum_member_bits, expected);
  XLS_EXPECT_OK(VerifyDeclaredEnumRoundtrip(context, 0));
  XLS_EXPECT_OK(VerifyDeclaredEnumRoundtrip(context, 1));
  XLS_EXPECT_OK(VerifyUndeclaredEnumPayloadRejected(context, 0));
}

// Generates one declared member index from {0, 1} for the fixed seed sum.
// It validates round-trip identity and does not vary tags or payload layout.
void DeclaredEnumPayloadRoundtrips(uint64_t member_index) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(VerifyDeclaredEnumRoundtrip(context, member_index));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, DeclaredEnumPayloadRoundtrips)
    .WithDomains(fuzztest::ElementOf<uint64_t>({0, 1}));

// Generates one undeclared two-bit member value from {2, 3}.
// It validates rejection and does not fuzz malformed tags or source syntax.
void UndeclaredEnumPayloadIsRejected(uint64_t invalid_member_value) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(
      VerifyUndeclaredEnumPayloadRejected(context, invalid_member_value));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, UndeclaredEnumPayloadIsRejected)
    .WithDomains(fuzztest::ElementOf<uint64_t>({2, 3}));

// Generates arbitrary payload bits under the fixed malformed tag.
// It validates raw-image preservation and does not vary declarations.
void MalformedSumTagPreservesRawImage(uint16_t payload_bits) {
  XLS_ASSERT_OK_AND_ASSIGN(RawBoundaryContext context,
                           LoadRawBoundaryContext(GetManifestPath()));
  XLS_ASSERT_OK(VerifyMalformedTagPreservesRawImage(context, payload_bits));
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, MalformedSumTagPreservesRawImage)
    .WithDomains(fuzztest::Arbitrary<uint16_t>());

enum class RawOperation {
  kForward,
  kWrap,
  kBind,
  kInspect,
  kInspectInvalid,
  kCompare,
  kConstantPattern,
};

// Both boundary inputs are literal tuples. In particular, neither creation of
// bytecode arguments nor the expected result invokes semantic sum conversion.
struct RawSumArgument {
  dslx::InterpValue bytecode;
  Value ir;
};

RawSumArgument MakeRawSumArgument(uint64_t tag, int64_t tag_width,
                                  uint64_t payload, int64_t payload_width) {
  return {
      .bytecode = dslx::InterpValue::MakeTuple(
          {dslx::InterpValue::MakeUBits(tag_width, tag),
           dslx::InterpValue::MakeTuple(
               {dslx::InterpValue::MakeUBits(payload_width, payload)})}),
      .ir =
          Value::Tuple({Value(UBits(tag, tag_width)),
                        Value::Tuple({Value(UBits(payload, payload_width))})}),
  };
}

std::string NestedRawProgram(int64_t narrow_width, int64_t wide_width,
                             RawOperation operation) {
  std::string program = absl::StrCat(
      "enum Message: u2 { Wide(u", wide_width, ") = 0, Narrow(u", narrow_width,
      ") = 2 }\n",
      "enum Envelope: u1 { Wrapped(Message) = 0, Wide(u16) = 1 }\n");
  switch (operation) {
    case RawOperation::kForward:
      absl::StrAppend(&program, "fn main(x: Message) -> Message { x }\n");
      break;
    case RawOperation::kWrap:
      absl::StrAppend(
          &program,
          "fn main(x: Message) -> Envelope { Envelope::Wrapped(x) }\n");
      break;
    case RawOperation::kBind:
      absl::StrAppend(&program, "fn main(x: Envelope) -> Message { match x {\n",
                      "Envelope::Wrapped(value) => value,\n",
                      "Envelope::Wide(_) => Message::Narrow(u", narrow_width,
                      ":0),\n", "} }\n");
      break;
    case RawOperation::kInspect:
    case RawOperation::kInspectInvalid:
      absl::StrAppend(&program, "fn main(x: Envelope) -> u16 { match x {\n",
                      "Envelope::Wrapped(inner) => match inner {\n",
                      "Message::Wide(value) => value as u16,\n",
                      "Message::Narrow(value) => value as u16,\n");
      if (operation == RawOperation::kInspectInvalid) {
        absl::StrAppend(&program, "invalid!(raw) => raw as u16,\n");
      }
      absl::StrAppend(&program, "}, Envelope::Wide(value) => value,\n} }\n");
      break;
    case RawOperation::kCompare:
      absl::StrAppend(&program,
                      "fn main(x: Envelope, y: Envelope) -> (bool, bool, "
                      "Envelope, Envelope) {\n",
                      "(x == y, x != y, x, y) }\n");
      break;
    case RawOperation::kConstantPattern:
      absl::StrAppend(&program, "fn main(x: Envelope) -> bool { ",
                      "const EXPECTED = Envelope::Wrapped(Message::Narrow(u",
                      narrow_width, ":1));\n",
                      "match x { EXPECTED => "
                      "true, _ => false } }\n");
      break;
  }
  return program;
}

struct RawInput {
  uint8_t tag;
  uint8_t payload_bits;
  uint8_t other_tag;
  uint8_t other_payload_bits;
  uint16_t outer_padding_bits;
};

// All compilation owners remain local to this invocation. The JIT is reused
// sequentially; each input still gets fresh arguments, results, and an oracle.
void NestedRawInputBatchesRespectObservation(
    uint8_t narrow_width, uint8_t wide_width, RawOperation operation,
    const std::vector<RawInput>& inputs) {
  const std::string program =
      NestedRawProgram(narrow_width, wide_width, operation);
  SCOPED_TRACE(program);
  dslx::ImportData import_data = dslx::CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(dslx::TypecheckedModule tm,
                           dslx::ParseAndTypecheck(program, "nested_raw.x",
                                                   "nested_raw", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(dslx::Function * function,
                           tm.module->GetMemberOrError<dslx::Function>("main"));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto bytecode,
      dslx::BytecodeEmitter::Emit(&import_data, tm.type_info, *function,
                                  dslx::ParametricEnv()));
  dslx::PackageConversionData package{
      .package = std::make_unique<Package>("nested_raw_package")};
  dslx::PackageData package_data{.conversion_info = &package};
  dslx::FunctionConverter converter(
      package_data, tm.module, &import_data, dslx::ConvertOptions(),
      /*proc_data=*/nullptr, /*channel_scope=*/nullptr, /*is_top=*/true);
  XLS_ASSERT_OK(converter.HandleFunction(function, tm.type_info,
                                         /*parametric_env=*/nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(xls::Function * ir_function,
                           package.package->GetFunction("__nested_raw__main"));
  XLS_ASSERT_OK_AND_ASSIGN(auto jit, FunctionJit::Create(ir_function));

  for (const RawInput& input : inputs) {
    const auto& [tag, payload_bits, other_tag, other_payload_bits,
                 outer_padding_bits] = input;
    const uint64_t wide_mask = (uint64_t{1} << wide_width) - 1;
    const uint64_t narrow_mask = (uint64_t{1} << narrow_width) - 1;
    const uint64_t payload = payload_bits & wide_mask;
    const uint64_t other_payload = other_payload_bits & wide_mask;
    const int64_t inner_width = 2 + wide_width;
    const uint64_t inner_bits = (uint64_t{tag} << wide_width) | payload;
    const uint64_t other_inner_bits =
        (uint64_t{other_tag} << wide_width) | other_payload;
    const uint64_t outer_padding =
        (uint64_t{outer_padding_bits} << inner_width) & 0xffff;
    const RawSumArgument inner =
        MakeRawSumArgument(tag, 2, payload, wide_width);
    const RawSumArgument outer =
        MakeRawSumArgument(0, 1, outer_padding | inner_bits, 16);
    // Deliberately use different padding in the two otherwise comparable
    // values.
    const RawSumArgument other_outer = MakeRawSumArgument(
        0, 1,
        (outer_padding ^ (0xffff & ~((uint64_t{1} << inner_width) - 1))) |
            other_inner_bits,
        16);
    const bool malformed = tag != 0 && tag != 2;
    const bool other_malformed = other_tag != 0 && other_tag != 2;
    std::vector<dslx::InterpValue> bytecode_args;
    std::vector<Value> ir_args;
    Value expected;
    bool source_observes_malformed = false;
    switch (operation) {
      case RawOperation::kForward:
      case RawOperation::kWrap:
        bytecode_args = {inner.bytecode};
        ir_args = {inner.ir};
        expected = operation == RawOperation::kForward
                       ? inner.ir
                       : MakeRawSumArgument(0, 1, inner_bits, 16).ir;
        break;
      case RawOperation::kBind:
        bytecode_args = {outer.bytecode};
        ir_args = {outer.ir};
        expected = inner.ir;
        break;
      case RawOperation::kInspect:
      case RawOperation::kInspectInvalid: {
        bytecode_args = {outer.bytecode};
        ir_args = {outer.ir};
        source_observes_malformed = malformed;
        // Without invalid!, lowered hardware projects undeclared tags through
        // the final Narrow arm. Source interpretation must fail at this depth.
        const uint64_t observed =
            operation == RawOperation::kInspectInvalid && malformed
                ? inner_bits
                : (tag == 0 ? payload : payload & narrow_mask);
        expected = Value(UBits(observed, 16));
        break;
      }
      case RawOperation::kCompare: {
        bytecode_args = {outer.bytecode, other_outer.bytecode};
        ir_args = {outer.ir, other_outer.ir};
        source_observes_malformed = malformed || other_malformed;
        // Bytecode rejects either malformed operand. Lowered equality instead
        // compares its tag and the final Narrow constructor's active bits.
        const uint64_t meaningful_mask = tag == 0 ? wide_mask : narrow_mask;
        const bool equal =
            tag == other_tag &&
            (payload & meaningful_mask) == (other_payload & meaningful_mask);
        expected =
            Value::Tuple({Value(UBits(equal, 1)), Value(UBits(!equal, 1)),
                          outer.ir, other_outer.ir});
        break;
      }
      case RawOperation::kConstantPattern:
        bytecode_args = {outer.bytecode};
        ir_args = {outer.ir};
        source_observes_malformed = malformed;
        expected = Value(UBits(tag == 2 && (payload & narrow_mask) == 1, 1));
        break;
    }

    SCOPED_TRACE(outer.ir.ToString());
    SCOPED_TRACE(other_outer.ir.ToString());
    absl::StatusOr<dslx::InterpValue> interpreted =
        dslx::BytecodeInterpreter::Interpret(&import_data, bytecode.get(),
                                             bytecode_args, std::nullopt);
    if (source_observes_malformed) {
      ASSERT_FALSE(interpreted.ok());
      EXPECT_EQ(interpreted.status().code(), absl::StatusCode::kInternal);
      const std::string_view observer =
          operation == RawOperation::kCompare ? "equality" : "observer";
      EXPECT_TRUE(
          absl::StrContains(interpreted.status().message(),
                            absl::StrCat("Semantic sum ", observer,
                                         " received a malformed value")))
          << interpreted.status();
    } else {
      XLS_ASSERT_OK(interpreted.status());
      XLS_ASSERT_OK_AND_ASSIGN(Value actual, interpreted->ConvertToIr());
      EXPECT_EQ(actual, expected);
    }

    // These results obey the lowered hardware contract, including the
    // separately specified malformed fallback, even when bytecode rejected
    // observation above.
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> ir_result,
                             InterpretFunction(ir_function, ir_args));
    EXPECT_EQ(ir_result.value, expected);
    XLS_ASSERT_OK_AND_ASSIGN(InterpreterResult<Value> jit_result,
                             jit->Run(ir_args));
    EXPECT_EQ(jit_result.value, expected);
  }
}

void NestedRawOperationsRespectObservation(
    uint8_t narrow_width, uint8_t wide_width, RawOperation operation,
    uint8_t tag, uint8_t payload_bits, uint8_t other_tag,
    uint8_t other_payload_bits, uint16_t outer_padding_bits) {
  NestedRawInputBatchesRespectObservation(
      narrow_width, wide_width, operation,
      {{tag, payload_bits, other_tag, other_payload_bits, outer_padding_bits}});
}

TEST(SemanticSumRawBoundaryFuzzTest, NestedRawOperationWitnesses) {
  for (RawOperation operation :
       {RawOperation::kForward, RawOperation::kWrap, RawOperation::kBind,
        RawOperation::kInspect, RawOperation::kInspectInvalid,
        RawOperation::kCompare, RawOperation::kConstantPattern}) {
    for (uint8_t tag : {uint8_t{0}, uint8_t{2}, uint8_t{3}}) {
      NestedRawOperationsRespectObservation(4, 8, operation, tag, 0xf1, tag,
                                            0x01, 0x3f);
    }
  }
  // Meaningful-bit and tag differences must survive the padding comparison.
  NestedRawOperationsRespectObservation(2, 5, RawOperation::kCompare, 2, 0x11,
                                        2, 0x12, 0x1ff);
  NestedRawOperationsRespectObservation(2, 5, RawOperation::kCompare, 3, 0x11,
                                        1, 0x11, 0x1ff);
}

FUZZ_TEST(SemanticSumRawBoundaryFuzzTest, NestedRawOperationsRespectObservation)
    .WithDomains(
        fuzztest::InRange<uint8_t>(1, 4), fuzztest::InRange<uint8_t>(5, 8),
        fuzztest::ElementOf<RawOperation>(
            {RawOperation::kForward, RawOperation::kWrap, RawOperation::kBind,
             RawOperation::kInspect, RawOperation::kInspectInvalid,
             RawOperation::kCompare, RawOperation::kConstantPattern}),
        fuzztest::InRange<uint8_t>(0, 3), fuzztest::Arbitrary<uint8_t>(),
        fuzztest::InRange<uint8_t>(0, 3), fuzztest::Arbitrary<uint8_t>(),
        fuzztest::Arbitrary<uint16_t>());

TEST(SemanticSumRawBoundaryFuzzTest, NestedRawBatchInputIsolation) {
  // Alternate valid and malformed inputs, equality outcomes, and dirty padding.
  // Reversing the same sequence must not change any input's independent oracle.
  std::vector<RawInput> inputs = {
      {0, 0xff, 0, 0xff, 0},      {3, 0xf1, 3, 0x01, 0xffff},
      {2, 0xf1, 2, 0x01, 0xffff}, {1, 0xff, 0, 0xff, 0},
      {2, 0x01, 2, 0x02, 0},      {0, 0, 0, 0, 0xffff}};
  for (RawOperation operation :
       {RawOperation::kForward, RawOperation::kWrap, RawOperation::kBind,
        RawOperation::kInspect, RawOperation::kInspectInvalid,
        RawOperation::kCompare, RawOperation::kConstantPattern}) {
    NestedRawInputBatchesRespectObservation(4, 8, operation, inputs);
    std::reverse(inputs.begin(), inputs.end());
    NestedRawInputBatchesRespectObservation(4, 8, operation, inputs);
  }
}

// The scalar property remains available for single-input corpus replay. The
// batched property amortizes compilation, but changes coverage feedback and
// shape frequency; a batch may shrink to one input for failure reproduction.
FUZZ_TEST(SemanticSumRawBoundaryFuzzTest,
          NestedRawInputBatchesRespectObservation)
    .WithDomains(fuzztest::InRange<uint8_t>(1, 4),
                 fuzztest::InRange<uint8_t>(5, 8),
                 fuzztest::ElementOf<RawOperation>(
                     {RawOperation::kForward, RawOperation::kWrap,
                      RawOperation::kBind, RawOperation::kInspect,
                      RawOperation::kInspectInvalid, RawOperation::kCompare,
                      RawOperation::kConstantPattern}),
                 fuzztest::VectorOf(fuzztest::StructOf<RawInput>(
                                        fuzztest::InRange<uint8_t>(0, 3),
                                        fuzztest::Arbitrary<uint8_t>(),
                                        fuzztest::InRange<uint8_t>(0, 3),
                                        fuzztest::Arbitrary<uint8_t>(),
                                        fuzztest::Arbitrary<uint16_t>()))
                     .WithMinSize(1)
                     .WithMaxSize(16));

}  // namespace
}  // namespace xls
