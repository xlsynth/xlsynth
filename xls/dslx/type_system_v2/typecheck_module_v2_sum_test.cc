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

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/substitute.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_cloner.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/semantics_analysis.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/ir_convert/convert_options.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/typecheck_test_utils.h"
#include "xls/dslx/type_system_v2/matchers.h"
#include "xls/dslx/type_system_v2/type_system_test_utils.h"
#include "xls/dslx/type_system_v2/typecheck_module_v2.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/warning_collector.h"
#include "xls/dslx/warning_kind.h"

namespace xls::dslx {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::Field;
using ::testing::HasSubstr;

TEST(TypecheckV2Test, SemanticSumCanonicalizationPreservesConfiguredValues) {
  constexpr std::string_view kProgram = R"(
#![feature(type_inference_v2)]
#![feature(generics)]
enum E { Unit, Payload(u32) }
$0
const VALUE = configured_value_or<u32>("K", u32:1);
)";
  for (std::string_view constructor : {"", "const X = E::Unit;"}) {
    SCOPED_TRACE(constructor);
    ImportData import_data = CreateImportDataForTest();
    ConvertOptions options;
    options.configured_values = {"K:7"};
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule result,
        ParseAndTypecheck(absl::Substitute(kProgram, constructor), "config.x",
                          "config", &import_data, /*comments=*/nullptr,
                          options));
    XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * value,
                             result.module->GetConstantDef("VALUE"));
    EXPECT_THAT(result.type_info->GetConstExpr(value),
                IsOkAndHolds(InterpValue::MakeU32(7)));
  }
}

TEST(TypecheckV2Test, SemanticSumTupleConstructor) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}
const X = MaybeU32::Some(u32:7);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "MaybeU32 { None | Some(uN[32]) }")));
}

TEST(TypecheckV2Test, SemanticSumUnitConstantRejectsTotalWidthOverflow) {
  EXPECT_THAT(R"(
enum S { Unit, Huge(u1[65535][65537]) }
const X = S::Unit;
)",
              TypecheckFails(
                  HasSubstr("shared sum bit count exceeds 4294967295 bits")));
  XLS_EXPECT_OK(TypecheckV2(R"(
enum S { Unit, Array(u1[3][5]) }
const X = S::Unit;
)"));
}

TEST(TypecheckV2Test, SemanticSumDeclarationRejectsTotalWidthOverflow) {
  for (std::string_view program : {
           R"(enum S { Unit, Huge(u1[65535][65537]) })",
           R"(enum S: u3 { Huge(u1[65535][65537]) = 0 })",
           R"(
enum Inner { Data(u1[65535][65537]) }
enum Outer { Unit, Nested(Inner) }
)",
       }) {
    SCOPED_TRACE(program);
    EXPECT_THAT(
        program,
        TypecheckFails(
            AllOf(HasSubstr("TypeInferenceError: fake.x:"),
                  HasSubstr("shared sum bit count exceeds 4294967295 bits"))));
  }
}

TEST(TypecheckV2Test, SemanticSumDeclarationAllowsMaximumTotalWidth) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Implicit { Data(u1[65535][65537]) }
enum Explicit: u3 { Data(u1[65534][65538]) = 0 }
fn consume(a: Implicit, b: Explicit) { () }
)"));
  XLS_ASSERT_OK_AND_ASSIGN(
      Function * function,
      result.tm.module->GetMemberOrError<Function>("consume"));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  ASSERT_EQ(type->params().size(), 2);
  for (const auto& param : type->params()) {
    EXPECT_THAT(param->GetTotalBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(4294967295)));
  }
}

TEST(TypecheckV2Test, GenericSemanticSumChecksTotalWidthWhenConcretized) {
  constexpr std::string_view kDefinition = R"(
#![feature(generics)]
enum S<N: u32> { Unit, Payload(u1[65535][N]) }
)";
  XLS_EXPECT_OK(TypecheckV2(kDefinition));
  XLS_EXPECT_OK(TypecheckV2(
      absl::Substitute("$0\nfn consume(value: S<u32:2>) { () }", kDefinition)));
  for (std::string_view use : {
           "fn consume(value: S<u32:65537>) { () }",
           R"(
enum Outer<T: type> { Wrap(T) }
fn consume(value: Outer<S<u32:65537>>) { () }
)",
       }) {
    SCOPED_TRACE(use);
    EXPECT_THAT(absl::Substitute("$0\n$1", kDefinition, use),
                TypecheckFails(
                    HasSubstr("shared sum bit count exceeds 4294967295 bits")));
  }
}

TEST(TypecheckV2Test, SemanticSumTagErrorPrecedesTotalWidthOverflow) {
  EXPECT_THAT(
      R"(
enum S: u1 { A = 0, Huge(u1[65535][65537]) = 0 }
)",
      TypecheckFails(HasSubstr("Semantic sum `S` has duplicate discriminant")));
}

TEST(TypecheckV2Test, SemanticSumTupleConstructorRejectsTooFewArguments) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}
const X = MaybeU32::Some();
)",
      TypecheckFails(HasSubstr("Expected 1 argument(s) but got 0.")));
}

TEST(TypecheckV2Test, SemanticSumPatternPayloadInfersGenericCallArgument) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum E { V(s12), U }
fn main(x: s12) -> s12 {
  assert_eq(match E::V(x) { E::V(y) => y, _ => s12:0 }, x);
  assert_eq(match (x,) { (y,) => y }, x);
  x
}
)"));
}

TEST(TypecheckV2Test, GenericSemanticSumPatternPayloadInfersCallArgumentWidth) {
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn identity<N: u32>(value: uN[N]) -> uN[N] { value }
fn main(value: Option<u8>) -> u8 {
  identity(match value {
    Option::None => u8:0,
    Option::Some(payload) => payload,
  })
}
)"));
}

TEST(TypecheckV2Test, SemanticSumTupleConstructorRejectsTooManyArguments) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}
const X = MaybeU32::Some(u32:7, u32:8);
)",
      TypecheckFails(HasSubstr("Expected 1 argument(s) but got 2.")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructor) {
  EXPECT_THAT(
      R"(
enum MaybePoint {
  None,
  Point { x: u32, y: u32 },
}
const X = MaybePoint::Point { x: u32:1, y: u32:2 };
)",
      TypecheckSucceeds(HasNodeWithType(
          "X", "MaybePoint { None | Point { x: uN[32], y: uN[32] } }")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructorRejectsSplat) {
  EXPECT_THAT(
      R"(
enum E { V { x: u32 } }
fn f(x: E) -> E { E::V { ..x } }
)",
      TypecheckFails(HasSubstr(
          "Struct-style sum constructors do not support splat syntax.")));
}

TEST(TypecheckV2Test, SemanticSumConstructorsCanonicalizeToSumInstances) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Option {
  None,
  Some(u32),
  Pair { lhs: u32, rhs: u32 },
}

const UNIT = Option::None;
const TUPLE = Option::Some(u32:7);
const STRUCT = Option::Pair { lhs: u32:3, rhs: u32:4 };
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * unit,
                           result.tm.module->GetConstantDef("UNIT"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * tuple,
                           result.tm.module->GetConstantDef("TUPLE"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * named,
                           result.tm.module->GetConstantDef("STRUCT"));

  const auto* unit_instance =
      absl::down_cast<const SumInstance*>(unit->value());
  const auto* tuple_instance =
      absl::down_cast<const SumInstance*>(tuple->value());
  const auto* struct_instance =
      absl::down_cast<const SumInstance*>(named->value());
  EXPECT_TRUE(unit_instance->is_unit());
  EXPECT_TRUE(tuple_instance->is_tuple());
  EXPECT_TRUE(struct_instance->is_struct());
}

TEST(TypecheckV2Test, GenericSemanticSumUnitsHaveConcreteTypeAndCanonicalAst) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32 = {u32:8}> { None, Some(uN[N]) }
const D = E::None;
fn f() -> E<u32:16> { E::None }
)"));
  EXPECT_THAT(
      TypeInfoToString(result.tm),
      IsOkAndHolds(AllOf(
          HasNodeWithType("D", "E<u32:8> { None | Some(uN[8]) }"),
          HasNodeWithType("f", "() -> E<u32:16> { None | Some(uN[16]) }"))));

  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * d,
                           result.tm.module->GetConstantDef("D"));
  const auto* defaulted = dynamic_cast<const SumInstance*>(d->value());
  ASSERT_NE(defaulted, nullptr);
  EXPECT_TRUE(defaulted->is_unit());
  EXPECT_EQ(defaulted->constructor_ref()->attr(), "None");

  std::optional<Function*> f = result.tm.module->GetFunction("f");
  ASSERT_TRUE(f.has_value());
  ASSERT_FALSE((*f)->body()->empty());
  const auto* contextual = dynamic_cast<const SumInstance*>(
      ToAstNode((*f)->body()->statements().back()->wrapped()));
  ASSERT_NE(contextual, nullptr);
  EXPECT_TRUE(contextual->is_unit());
  EXPECT_EQ(contextual->constructor_ref()->attr(), "None");
}

TEST(TypecheckV2Test, SemanticSumConstructorPreservesShadowingLocalEnumAlias) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum E: u8 { A = 1 }
enum F: u8 { A = 2 }
enum S { K(u8) }
fn f() -> u8 {
  type E = F;
  let wrapped = S::K(E::A as u8);
  match wrapped { S::K(x) => x }
}
const_assert!(f() == u8:2);
)"));
}

TEST(TypecheckV2Test,
     SemanticSumConstructorPreservesNonShadowingLocalEnumAlias) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum E: u8 { A = 1 }
enum F: u8 { A = 2 }
enum S { K(u8) }
fn f() -> u8 {
  type G = F;
  let wrapped = S::K(G::A as u8);
  match wrapped { S::K(x) => x }
}
const_assert!(f() == u8:2);
)"));
}

TEST(TypecheckV2Test, ShadowingLocalEnumAliasWithoutSumConstructor) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum E: u8 { A = 1 }
enum F: u8 { A = 2 }
fn f() -> u8 {
  type E = F;
  E::A as u8
}
const_assert!(f() == u8:2);
)"));
}

TEST(TypecheckV2Test, SemanticSumConstructorsPreserveShadowingLocalSumAlias) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum A { Unit, K(u8), Record { value: u8 } }
enum B { Unit, K(u16), Record { value: u16 } }
fn f() -> (B, B, B) {
  type A = B;
  (A::Unit, A::K(u16:7), A::Record { value: u16:8 })
}
)"));
}

TEST(TypecheckV2Test, SemanticSumPayloadPreservesShadowingLocalTypeAlias) {
  XLS_EXPECT_OK(TypecheckV2(R"(
type Word = u8;
enum S { K(u16) }
fn f() -> S {
  type Word = u16;
  S::K(Word:7)
}
)"));
}

TEST(TypecheckV2Test, SemanticSumPayloadPreservesExpressionLocalAlias) {
  XLS_EXPECT_OK(TypecheckV2(R"(
enum E: u8 { A = 1 }
enum F: u8 { A = 2 }
enum S { K(u8) }
fn f() -> u8 {
  let wrapped = S::K({
    type E = F;
    let value: E = E::A;
    value as u8
  });
  match wrapped { S::K(x) => x }
}
const_assert!(f() == u8:2);
)"));
}

TEST(TypecheckV2Test, SemanticSumConstructorPreservesImportedLocalAlias) {
  constexpr std::string_view kImported = R"(
pub enum S { K(u16) }
)";
  constexpr std::string_view kProgram = R"(
import imported;
enum S { K(u8) }
fn f() -> imported::S {
  type S = imported::S;
  S::K(u16:7)
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_EXPECT_OK(TypecheckV2(kProgram, "main", &import_data));
}

TEST(TypecheckV2Test, SemanticSumProcChannelCanonicalizesInProcContext) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}

proc Passthrough {
  in_ch: chan<Option> in;
  out_ch: chan<Option> out;

  init { () }

  config(in_ch: chan<Option> in, out_ch: chan<Option> out) {
    (in_ch, out_ch)
  }

  next(_: ()) {
    let (tok, value) = recv(join(), in_ch);
    send(tok, out_ch, value);
  }
}

proc Main {
  data_out: chan<Option> out;
  data_in: chan<Option> in;

  init { () }

  config() {
    let (input_p, input_c) = chan<Option>("input");
    let (output_p, output_c) = chan<Option>("output");
    spawn Passthrough(input_c, output_p);
    (input_p, output_c)
  }

  next(_: ()) {
    let _ = send(join(), data_out, Option::Some(u32:42));
    ()
  }
}
)",
      TypecheckSucceeds(HasNodeWithType("Option::Some(u32:42)",
                                        "Option { None | Some(uN[32]) }")));
}

TEST(TypecheckV2Test, ImportedSemanticSumConstructorCanonicalizes) {
  constexpr std::string_view kImported = R"(
pub enum Option {
  None,
  Some(u32),
}
)";
  constexpr std::string_view kProgram = R"(
import imported;

type T = imported::Option;
const X: T = imported::Option::Some(u32:7);
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(kProgram, "main", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * x,
                           result.tm.module->GetConstantDef("X"));
  EXPECT_EQ(x->value()->kind(), AstNodeKind::kSumInstance);
  // The alias borrows a ColonRef through its TypeRef, while X's value becomes
  // a SumInstance. Canonicalization must preserve that structural type edge.
  XLS_ASSERT_OK_AND_ASSIGN(TypeAlias * alias,
                           result.tm.module->GetMemberOrError<TypeAlias>("T"));
  const auto* annotation =
      absl::down_cast<const TypeRefTypeAnnotation*>(&alias->type_annotation());
  const TypeDefinition& definition = annotation->type_ref()->type_definition();
  ASSERT_TRUE(std::holds_alternative<ColonRef*>(definition));
  EXPECT_EQ(std::get<ColonRef*>(definition)->ToString(), "imported::Option");
}

TEST(TypecheckV2Test, ImportedGenericStructKeepsFinalSumDeclaration) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
#![feature(generics)]
pub struct Box<T: type> { value: T }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
enum E { Unit, Payload(u32) }
$0
fn read(x: imported::Box<E>) -> E { x.value }
)";

  // The constructor used to trigger a second typecheck against the imported
  // Box's cached binding to the first E. The control changes only that line.
  for (std::string_view constructor : {"", "const X = E::Unit;"}) {
    for (bool preload : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "constructor: " << constructor
                                        << ", preload: " << preload);
      absl::flat_hash_map<std::filesystem::path, std::string> files = {
          {"/imported.x", std::string(kImported)},
      };
      ImportData import_data = CreateImportDataForTest(
          std::make_unique<FakeFilesystem>(std::move(files), "/"));
      if (preload) {
        XLS_ASSERT_OK(Typecheck(kImported, "imported", &import_data,
                                /*add_version_attribute=*/false));
      }
      XLS_ASSERT_OK_AND_ASSIGN(
          TypecheckResult result,
          TypecheckV2(absl::Substitute(kProgram, constructor), "main",
                      &import_data));
      XLS_ASSERT_OK_AND_ASSIGN(SumDef * final_sum,
                               result.tm.module->GetMemberOrError<SumDef>("E"));
      XLS_ASSERT_OK_AND_ASSIGN(
          Function * read,
          result.tm.module->GetMemberOrError<Function>("read"));
      std::optional<Type*> member = result.tm.type_info->GetItem(
          ToAstNode(read->body()->statements().back()->wrapped()));
      ASSERT_TRUE(member.has_value());
      const auto* member_type = dynamic_cast<const SumType*>(*member);
      ASSERT_NE(member_type, nullptr);
      XLS_ASSERT_OK_AND_ASSIGN(
          FunctionType * signature,
          result.tm.type_info->GetItemAs<FunctionType>(read));
      const auto* return_type =
          dynamic_cast<const SumType*>(&signature->return_type());
      ASSERT_NE(return_type, nullptr);
      EXPECT_EQ(&member_type->nominal_type(), final_sum);
      EXPECT_EQ(&return_type->nominal_type(), final_sum);
    }
  }
}

TEST(TypecheckV2Test, ImportedGenericStructKeepsFinalStructDeclaration) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
#![feature(generics)]
pub struct Box<T: type> { value: T }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
struct S { value: u32 }
enum E { Unit, Payload(u32) }
const X = E::Unit;
fn read(x: imported::Box<S>) -> S { x.value }
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/imported.x", std::string(kImported)},
  };
  ImportData import_data = CreateImportDataForTest(
      std::make_unique<FakeFilesystem>(std::move(files), "/"));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(kProgram, "main", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(StructDef * final_struct,
                           result.tm.module->GetMemberOrError<StructDef>("S"));
  XLS_ASSERT_OK_AND_ASSIGN(
      Function * read, result.tm.module->GetMemberOrError<Function>("read"));
  XLS_ASSERT_OK_AND_ASSIGN(StructType * member_type,
                           result.tm.type_info->GetItemAs<StructType>(ToAstNode(
                               read->body()->statements().back()->wrapped())));
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * signature,
                           result.tm.type_info->GetItemAs<FunctionType>(read));
  const auto* return_type =
      dynamic_cast<const StructType*>(&signature->return_type());
  ASSERT_NE(return_type, nullptr);
  EXPECT_EQ(&member_type->nominal_type(), final_struct);
  EXPECT_EQ(&return_type->nominal_type(), final_struct);
}

TEST(TypecheckV2Test, SemanticSumNormalizationPreservesUseBindings) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
pub const VALUE = u32:7;
)";
  constexpr std::string_view kProgram = R"(#![feature(use_syntax)]
use imported::VALUE;
enum E { V(u32) }
const X = E::V(VALUE);
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/imported.x", std::string(kImported)},
  };
  ImportData import_data = CreateImportDataForTest(
      std::make_unique<FakeFilesystem>(std::move(files), "/"));
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      IsOkAndHolds(HasTypeInfo(HasNodeWithType("X", "E { V(uN[32]) }"))));
}

TEST(TypecheckV2Test, SemanticSumConstructorFromUseImport) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
pub enum E { V(u8) }
)";
  constexpr std::string_view kProgram = R"(#![feature(use_syntax)]
use imported::E;
const X = E::V(u8:1);
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/imported.x", std::string(kImported)},
  };
  ImportData import_data = CreateImportDataForTest(
      std::make_unique<FakeFilesystem>(std::move(files), "/"));
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      IsOkAndHolds(HasTypeInfo(HasNodeWithType("X", "E { V(uN[8]) }"))));
}

TEST(TypecheckV2Test, SemanticSumNormalizationPreservesGeneratedDomain) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#[fuzz_domain("S_Domain")]
struct S { x: u32 }
enum E { Unit, Payload(u32) }
const X = E::Unit;
fn domain_field(d: S_Domain) -> () { d.x }
)"));
  XLS_ASSERT_OK_AND_ASSIGN(StructDef * original,
                           result.tm.module->GetMemberOrError<StructDef>("S"));
  XLS_ASSERT_OK_AND_ASSIGN(
      StructDef * domain,
      result.tm.module->GetMemberOrError<StructDef>("S_Domain"));
  EXPECT_EQ(domain->name_def()->definer(), original);
  ASSERT_EQ(domain->members().size(), 1);
  EXPECT_EQ(domain->members()[0]->name(), "x");
  EXPECT_EQ(domain->members()[0]->type()->ToString(), "()");
}

TEST(TypecheckV2Test, SemanticSumNormalizationPreparesImportedDomain) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
#[fuzz_domain("S_Domain")]
pub struct S { x: u32 }
enum E { Unit, Payload(u32) }
const X = E::Unit;
)";
  constexpr std::string_view kProgram = R"(
import imported;
fn domain_field(d: imported::S_Domain) -> () { d.x }
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/imported.x", std::string(kImported)},
  };
  ImportData import_data = CreateImportDataForTest(
      std::make_unique<FakeFilesystem>(std::move(files), "/"));
  // Imported modules suppress semantic warnings, but still need preparation.
  EXPECT_THAT(TypecheckV2(kProgram, "main", &import_data),
              IsOkAndHolds(HasTypeInfo(HasNodeWithType("d.x", "()"))));
}

TEST(TypecheckV2Test, SemanticSumNormalizationPreservesCapturedLambda) {
  EXPECT_THAT(R"(
enum E { V(u32) }
fn f(capture: u32) -> E[1] {
  map(u32[1]:[0], |x| -> E { E::V(capture + x) })
}
)",
              TypecheckSucceeds(
                  HasNodeWithType("f", "(uN[32]) -> E { V(uN[32]) }[1]")));
}

TEST(TypecheckV2Test, ImportedTypesCannotBeSemanticSumPayloadValues) {
  constexpr std::string_view kImported = R"(
pub enum Tag: u8 { A = 0 }
)";
  constexpr std::string_view kPrograms[] = {
      R"(
import imported;
enum E { V(imported::Tag) }
fn f() -> E { E::V(imported::Tag) }
)",
      R"(
import imported;
enum E { V { x: imported::Tag } }
fn f() -> E { E::V { x: imported::Tag } }
)",
  };
  for (std::string_view program : kPrograms) {
    SCOPED_TRACE(program);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
    EXPECT_THAT(
        TypecheckV2(program, "main", &import_data),
        StatusIs(absl::StatusCode::kInvalidArgument,
                 HasSubstr("Cannot pass a type as a sum constructor payload")));
  }
}

TEST(TypecheckV2Test, ImportedEnumValuesAreSemanticSumPayloadValues) {
  constexpr std::string_view kImported = R"(
pub enum Tag: u8 { A = 0 }
)";
  constexpr std::string_view kProgram = R"(
import imported;
enum E { Tuple(imported::Tag), Named { x: imported::Tag } }
fn tuple() -> E { E::Tuple(imported::Tag::A) }
fn named() -> E { E::Named { x: imported::Tag::A } }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_EXPECT_OK(TypecheckV2(kProgram, "main", &import_data));
}

TEST(TypecheckV2Test, ImportedCanonicalizedSemanticSumCanBeUsedAsType) {
  constexpr std::string_view kImported = R"(
pub enum Option {
  None,
  Some(u32),
}
pub const SOME: Option = Option::Some(u32:7);
)";
  constexpr std::string_view kProgram = R"(
import imported;

fn identity(x: imported::Option) -> imported::Option {
  x
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(kProgram, "main", &import_data),
              IsOkAndHolds(HasTypeInfo(
                  HasNodeWithType("x", "Option { None | Some(uN[32]) }"))));
}

// Negative test: checks error handling for cross-module sum assignments.
TEST(TypecheckV2Test, ClonedModuleKeepsDistinctSemanticSumIdentity) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule original,
                           ParseAndTypecheck("pub enum S { A(u8), B }",
                                             "same.x", "a", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Module> clone,
                           CloneModuleRemovingMembers(*original.module, {}));
  clone->SetName("b");
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule installed_clone,
      TypecheckModule(std::move(clone), "same.x", &import_data));
  EXPECT_NE(original.module, installed_clone.module);

  // Every owner is still alive. Matching source locations do not make these
  // independently installed declarations interchangeable.
  EXPECT_THAT(
      ParseAndTypecheck("import a; import b; fn f(x: a::S) -> b::S { x }",
                        "main.x", "main", &import_data),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("type mismatch")));
}

// Verifies: normalized imports and aliases retain their owning sum definition.
// Catches: rebinding imported sums by shared source spans.
TEST(TypecheckV2Test, ImportedNormalizedSumKeepsOwningModule) {
  constexpr std::string_view kImported = R"(
pub enum S { A(u8), B }
pub const VALUE: S = S::A(u8:7);
)";
  constexpr std::string_view kConsumer = R"(
import a;
import b;
type Alias = a::S;
fn from_original(x: Alias) -> a::S { x }
fn original_value() -> Alias { a::VALUE }
fn from_clone(x: b::S) -> b::S { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule original,
      ParseAndTypecheck(kImported, "same.x", "a", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Module> clone,
                           CloneModuleRemovingMembers(*original.module, {}));
  clone->SetName("b");
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule installed_clone,
      TypecheckModule(std::move(clone), "same.x", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule consumer,
      ParseAndTypecheck(kConsumer, "consumer.x", "consumer", &import_data));

  Function* from_original =
      consumer.module->GetFunction("from_original").value();
  Function* from_clone = consumer.module->GetFunction("from_clone").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * original_type,
      consumer.type_info->GetItemAs<FunctionType>(from_original));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * clone_type,
      consumer.type_info->GetItemAs<FunctionType>(from_clone));
  EXPECT_EQ(&original_type->params().front()->AsSum().nominal_type(),
            original.module->GetMember<SumDef>("S").value());
  EXPECT_EQ(&clone_type->params().front()->AsSum().nominal_type(),
            installed_clone.module->GetMember<SumDef>("S").value());
  EXPECT_EQ(*original_type->params().front(), original_type->return_type());
  EXPECT_NE(original_type->return_type(), clone_type->return_type());
}

// Exercises normalization before ownership transfers to ImportData.
absl::StatusOr<std::unique_ptr<ModuleInfo>> TypecheckUninstalledNormalizedSum(
    ImportData& import_data) {
  constexpr std::string_view kProgram = R"(
pub enum S { A(u8), B }
pub const VALUE: S = S::A(u8:7);
fn identity(x: S) -> S { x }
)";
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<Module> module,
      ParseModule(kProgram, "pending.x", "pending", import_data.file_table()));
  WarningCollector warnings(import_data.enabled_warnings());
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<ModuleInfo> module_info,
      TypecheckModuleV2(
          std::move(module), "pending.x", &import_data, &warnings,
          std::make_unique<SemanticsAnalysis>(), /*error_handler=*/nullptr,
          std::make_optional(import_data.GetBuiltinTraitDeriver())));
  return module_info;
}

// The canonical declaration is established before typechecking and survives
// installation without a first-pass declaration or remapping.
TEST(TypecheckV2Test, NormalizedSumIdentitySurvivesInstallation) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleInfo> pending,
                           TypecheckUninstalledNormalizedSum(import_data));
  const SumDef* normalized = pending->module().GetMember<SumDef>("S").value();
  Function* identity = pending->module().GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      pending->type_info()->GetItemAs<FunctionType>(identity));
  EXPECT_EQ(&function_type->params().front()->AsSum().nominal_type(),
            normalized);
  XLS_ASSERT_OK_AND_ASSIGN(ImportTokens subject,
                           ImportTokens::FromString("pending"));
  XLS_ASSERT_OK(import_data.Put(subject, std::move(pending)));
  EXPECT_EQ(&function_type->return_type().AsSum().nominal_type(), normalized);
}

// Negative test: checks error handling for duplicate module installation.
TEST(TypecheckV2Test, RejectedInstallationPreservesInstalledSumDefinition) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule existing,
      ParseAndTypecheck("pub enum S { A(u8), B }", "existing.x", "pending",
                        &import_data));
  const SumDef* existing_sum = existing.module->GetMember<SumDef>("S").value();
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleInfo> pending,
                           TypecheckUninstalledNormalizedSum(import_data));
  ASSERT_NE(existing_sum, pending->module().GetMember<SumDef>("S").value());
  XLS_ASSERT_OK_AND_ASSIGN(ImportTokens subject,
                           ImportTokens::FromString("pending"));
  EXPECT_THAT(import_data.Put(subject, std::move(pending)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Module is already loaded")));

  // Rejection retains the installed module and sum definition.
  XLS_ASSERT_OK_AND_ASSIGN(ModuleInfo * installed, import_data.Get(subject));
  EXPECT_EQ(&installed->module(), existing.module);
  EXPECT_EQ(installed->module().GetMember<SumDef>("S").value(), existing_sum);
}

// An uninstalled sum module stays absent from the import map after disposal.
TEST(TypecheckV2Test, DiscardedModuleDoesNotInstallSum) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(ParseAndTypecheck("pub enum S { A(u8), B }", "existing.x",
                                  "existing", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleInfo> pending,
                           TypecheckUninstalledNormalizedSum(import_data));
  const SumDef* normalized = pending->module().GetMember<SumDef>("S").value();
  {
    Function* identity = pending->module().GetFunction("identity").value();
    XLS_ASSERT_OK_AND_ASSIGN(
        FunctionType * function_type,
        pending->type_info()->GetItemAs<FunctionType>(identity));
    EXPECT_EQ(&function_type->params().front()->AsSum().nominal_type(),
              normalized);
    EXPECT_EQ(*function_type->params().front(), function_type->return_type());
    std::unique_ptr<Type> clone = function_type->CloneToUnique();
    EXPECT_EQ(*clone, *function_type);
  }
  XLS_ASSERT_OK_AND_ASSIGN(ImportTokens subject,
                           ImportTokens::FromString("pending"));
  EXPECT_FALSE(import_data.Contains(subject));
  pending.reset();
  EXPECT_FALSE(import_data.Contains(subject));
}

TEST(TypecheckV2Test, SemanticSumExplicitDiscriminantsMustBeDistinct) {
  EXPECT_THAT(
      R"(
enum Message : u3 {
  Idle() = 0,
  Request(u8) = 3,
  Retry(u8) = 3,
}
)",
      TypecheckFails(AllOf(HasSubstr("Semantic sum `Message`"),
                           HasSubstr("duplicate discriminant"))));
}

TEST(TypecheckV2Test, UntaggedSemanticSumInfersMinimumDiscriminantWidth) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Message {
  Idle() = u32:0,
  Ready() = u32:1,
}

fn f(value: Message) -> Message {
  value
}
)"));
  Function* function = result.tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto* sum_type =
      dynamic_cast<const SumType*>(function_type->params().at(0).get());
  ASSERT_NE(sum_type, nullptr);
  EXPECT_THAT(sum_type->tag_bit_count().GetAsInt64(), IsOkAndHolds(1));
  EXPECT_THAT(sum_type->GetDiscriminant(0).GetBitCount(), IsOkAndHolds(1));
  EXPECT_THAT(sum_type->GetDiscriminant(1).GetBitCount(), IsOkAndHolds(1));
}

TEST(TypecheckV2Test, UntaggedSemanticSumUsesUnsignedTagForNonnegativeValues) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Message {
  Idle() = s32:0,
  Ready() = s32:1,
}
fn f(value: Message) -> Message { value }
)"));
  Function* function = result.tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto* sum_type =
      dynamic_cast<const SumType*>(function_type->params().at(0).get());
  ASSERT_NE(sum_type, nullptr);
  EXPECT_THAT(sum_type->tag_bit_count().GetAsInt64(), IsOkAndHolds(1));
  EXPECT_FALSE(sum_type->GetDiscriminant(1).IsSigned());
  EXPECT_THAT(sum_type->GetDiscriminant(1).GetBitCount(), IsOkAndHolds(1));
}

TEST(TypecheckV2Test, UntaggedSemanticSumInfersMinimumSignedDiscriminantWidth) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Message {
  Before() = s32:-1,
  At() = s32:0,
}

fn f(value: Message) -> Message {
  value
}
)"));
  Function* function = result.tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto* sum_type =
      dynamic_cast<const SumType*>(function_type->params().at(0).get());
  ASSERT_NE(sum_type, nullptr);
  EXPECT_THAT(sum_type->tag_bit_count().GetAsInt64(), IsOkAndHolds(1));
  EXPECT_TRUE(sum_type->GetDiscriminant(0).IsSigned());
  EXPECT_THAT(sum_type->GetDiscriminant(0).GetBitCount(), IsOkAndHolds(1));
}

TEST(TypecheckV2Test, LocalSemanticSumConstructorExplicitParametricsRejected) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Option<N: u32> {
  None,
  Some(uN[N]),
}

fn make(x: u8) -> Option<u32:8> {
  Option::Some<u32:8>(x)
}
)",
      TypecheckFails(HasSubstr("Explicit parametrics belong on the sum type, "
                               "not the constructor")));
}

TEST(TypecheckV2Test,
     ImportedSemanticSumConstructorExplicitParametricsRejected) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum Option<N: u32> {
  None,
  Some(uN[N]),
}
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;

fn make(x: u8) -> imported::Option<u32:8> {
  imported::Option::Some<u32:8>(x)
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Explicit parametrics belong on the sum type, not "
                         "the constructor")));
}

TEST(TypecheckV2Test,
     SemanticSumConstructorExplicitParametricsRejectedInGenericBody) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
fn make<N: u32>(x: uN[N]) -> E<N> { E::V<N>(x) }
)",
      TypecheckFails(HasSubstr("Explicit parametrics belong on the sum type, "
                               "not the constructor")));
}

TEST(TypecheckV2Test, NamedSemanticSumRejectsVariantSuffixParametrics) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum E<N: u32 = {u32:8}> { V { x: uN[N] } }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(R"(#![feature(generics)]
import imported;
const X = imported::E::V<u32:16> { x: u8:0 };
)",
                          "main", &import_data),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Explicit parametrics belong on the sum type, "
                                 "not the constructor")));
  EXPECT_THAT(R"(#![feature(generics)]
enum E<N: u32 = {u32:8}> { V { x: uN[N] } }
const X = E<u32:16>::V { x: u8:0 };
)",
              TypecheckFails(HasSubstr("size mismatch")));
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32 = {u32:8}> { V { x: uN[N] } }
const DEFAULT = E::V { x: u8:0 };
const EXPLICIT = E<u32:16>::V { x: u16:0 };
)",
      TypecheckSucceeds(
          AllOf(HasNodeWithType("DEFAULT", "E<u32:8> { V { x: uN[8] } }"),
                HasNodeWithType("EXPLICIT", "E<u32:16> { V { x: uN[16] } }"))));
}

TEST(TypecheckV2Test, MissingSemanticSumConstructorReturnsUserError) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}

const X = Option::Missing(u32:7);
)",
      TypecheckFails(HasSubstr("Sum 'Option' has no constructor 'Missing'.")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructorMissingMemberRejected) {
  EXPECT_THAT(
      R"(
enum MaybePoint {
  None,
  Point { x: u32, y: u32 },
}

const X = MaybePoint::Point { x: u32:1 };
)",
      TypecheckFails(
          HasSubstr("Instance of constructor `Point` is missing member(s): "
                    "`y`")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructorExtraMemberRejected) {
  EXPECT_THAT(
      R"(
enum MaybePoint {
  None,
  Point { x: u32, y: u32 },
}

const X = MaybePoint::Point { x: u32:1, y: u32:2, z: u32:3 };
)",
      TypecheckFails(HasSubstr("Constructor `Point` has no member `z`")));
}

TEST(TypecheckV2Test, SemanticSumTuplePayloadAggregate) {
  EXPECT_THAT(
      R"(
struct Point {
  x: u32,
  y: u32,
}

enum MaybePoint {
  None,
  Some(Point),
}

const X = MaybePoint::None;
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, SemanticSumStructPayloadAggregate) {
  EXPECT_THAT(
      R"(
enum PairBox {
  Pair { xy: (u32, u32) },
}

const X = PairBox::Pair { xy: (u32:1, u32:2) };
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, SemanticSumRejectsTokenPayload) {
  EXPECT_THAT(
      R"(
enum InvalidPayload {
  None,
  Token(token),
}
)",
      TypecheckFails(HasSubstr(
          "Semantic sum constructor `Token` cannot contain a token payload.")));
}

TEST(TypecheckV2Test, SemanticSumRejectsNestedTokenPayload) {
  EXPECT_THAT(
      R"(
struct TokenHolder {
  item: token,
}

enum InvalidPayload {
  None,
  Nested(TokenHolder),
}
)",
      TypecheckFails(HasSubstr("Semantic sum constructor `Nested` cannot "
                               "contain a token payload.")));
}

TEST(TypecheckV2Test, SemanticSumRejectsChannelHandlePayloadBeforeMatching) {
  EXPECT_THAT(R"(
enum E { Carry(chan<u8> in) }
fn f(x: E) -> bool {
  match x { E::Carry(_) => true }
}
)",
              TypecheckFails(HasSubstr(
                  "Semantic sum constructor `Carry` cannot contain a channel "
                  "handle payload.")));
}

TEST(TypecheckV2Test, SemanticSumRejectsNestedChannelHandlePayload) {
  EXPECT_THAT(R"(
struct Handles { outputs: chan<u8>[2] out }
enum E { Carry((u8, Handles)) }
fn f(x: E) -> bool {
  match x { E::Carry(_) => true }
}
)",
              TypecheckFails(HasSubstr(
                  "Semantic sum constructor `Carry` cannot contain a channel "
                  "handle payload.")));
}

TEST(TypecheckV2Test, ImplicitSemanticSumAcceptsTagTypeAnnotationInPhase2) {
  EXPECT_THAT(
      R"(
enum MaybeU32 : u3 {
  None,
  Some(u32),
}
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test,
     ImplicitSemanticSumRejectsTooNarrowTagTypeAnnotationInPhase2) {
  EXPECT_THAT(
      R"(
enum TrafficLight : u1 {
  Red(),
  Yellow(),
  Green(),
}
)",
      TypecheckFails(HasSubstr(
          "Semantic sum `TrafficLight` needs at least 2 tag bits for 3 "
          "implicit constructors, but tag type `u1` has only 1 bits.")));
}

TEST(TypecheckV2Test,
     ImplicitSemanticSumAcceptsSufficientSignedTagTypeAnnotationInPhase2) {
  EXPECT_THAT(
      R"(
enum Flag : s2 {
  Off,
  On(),
}
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test,
     ImplicitSemanticSumRejectsSignedTagThatCannotRepresentItsOrdinals) {
  EXPECT_THAT(
      R"(
enum Flag : s1 {
  Off,
  On(),
}
)",
      TypecheckFails(HasSubstr(
          "Semantic sum `Flag` needs at least 2 tag bits for 2 implicit "
          "constructors, but tag type `s1` has only 1 bits.")));
}

TEST(TypecheckV2Test, SemanticSumEmptyPayloadLeafAllowedInPhase1) {
  EXPECT_THAT(
      R"(
enum Never {}

enum S {
  Unit,
  Impossible(Never),
}

fn f(x: S) -> u32 {
  match x {
    S::Unit => u32:0,
  }
}
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, InvalidPatternBindsRawRepresentationBits) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u9 {
  match x {
    Option::Some(_) => u9:0,
    _ => u9:0,
    invalid!(raw) => raw,
  }
}
)",
              TypecheckSucceeds(::testing::A<std::string>()));
}

TEST(TypecheckV2Test, InvalidPatternBindsGenericSumRawRepresentationBits) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum E<N: u32>: u2 { A(uN[N]) = 0, B = 1 }
fn f(x: E<u32:8>) -> u10 {
  match x {
    E<u32:8>::A(_) => u10:0,
    E<u32:8>::B => u10:0,
    invalid!(raw) => raw,
  }
}
)",
              TypecheckSucceeds(HasNodeWithType("raw", "uN[10]")));
}

TEST(TypecheckV2Test, InvalidPatternRequiresSumScrutinee) {
  EXPECT_THAT(R"(
fn f(x: u8) -> u8 {
  match x {
    _ => x,
    invalid! => u8:0,
  }
}
)",
              TypecheckFails(
                  HasSubstr("`invalid!` is only valid when matching on a sum "
                            "type.")));
}

TEST(TypecheckV2Test, InvalidPatternMustBeFinalArm) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    invalid! => u8:0,
    _ => u8:1,
  }
}
)",
              TypecheckFails(
                  HasSubstr("`invalid!` must be the final arm in a match.")));
}

TEST(TypecheckV2Test, WildcardMayOnlyBeFollowedByInvalidPattern) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    _ => u8:0,
    Option::Some(v) => v,
    invalid! => u8:1,
  }
}
)",
              TypecheckFails(HasSubstr(
                  "A wildcard arm may only be followed by a final `invalid!` "
                  "arm.")));
}

TEST(TypecheckV2Test, WildcardMayBeFollowedByFinalInvalidPattern) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    _ => u8:0,
    invalid! => u8:1,
  }
}
)",
              TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, WildcardWithoutInvalidPatternCannotPrecedeSumArm) {
  for (const std::string pattern : {"_", "_ | Option::None"}) {
    SCOPED_TRACE(pattern);
    EXPECT_THAT(
        R"(
enum Option { None, Some(u8) }
fn f(x: Option) -> u8 {
  match x {
    )" + pattern +
            R"( => u8:0,
    Option::Some(v) => v,
  }
}
)",
        TypecheckFails(HasSubstr(
            "A wildcard arm may only be followed by a final `invalid!` arm.")));
  }
}

TEST(TypecheckV2Test, WildcardAlternativeMayBeFollowedByFinalInvalidPattern) {
  EXPECT_THAT(R"(
enum Option { None, Some(u8) }
fn f(x: Option) -> u8 {
  match x {
    _ | Option::None => u8:0,
    invalid! => u8:1,
  }
}
)",
              TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, DeeplyNestedSumCatchAll) {
  // Each level shares its inner type between two alternatives. A catch-all
  // needs no payload coverage dimensions, with or without a final invalid!.
  std::string program = "enum S0 { Leaf(u1) }\n";
  for (int depth = 1; depth <= 12; ++depth) {
    const std::string inner = "S" + std::to_string(depth - 1);
    program += "enum S" + std::to_string(depth) + " { L(" + inner + "), R(" +
               inner + ") }\n";
  }
  program += R"(
fn wildcard(x: S12) -> bool { match x { _ => true } }
fn explicit_invalid(x: S12) -> bool {
  match x { _ => true, invalid! => false }
}
fn binding(x: S12) -> bool {
  match x { _whole => true, invalid! => false }
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(program));
  EXPECT_TRUE(result.tm.warnings.warnings().empty());
}

TEST(TypecheckV2Test, EmptySumCatchAllPreservesExhaustiveWarning) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Never {}
enum Empty { Left(Never), Right(Never) }
fn wildcard(x: Empty) -> bool { match x { _ => true } }
fn explicit_invalid(x: Empty) -> bool {
  match x { _ => true, invalid! => false }
}
)"));
  ASSERT_EQ(result.tm.warnings.warnings().size(), 2);
  for (const auto& warning : result.tm.warnings.warnings()) {
    EXPECT_EQ(warning.message,
              "Match is already exhaustive before this pattern");
  }
}

TEST(TypecheckV2Test, SumCatchAllPreservesFallbackAndCoverageRejections) {
  const std::string declaration = "enum Choice { Left(u1), Right(u1) }\n";
  EXPECT_THAT(declaration + R"(
fn f(x: Choice) -> bool { match x { _whole => true } }
)",
              TypecheckFails(HasSubstr("A sum match without `invalid!`")));
  EXPECT_THAT(declaration + R"(
fn f(x: Choice) -> bool { match x { invalid! => false } }
)",
              TypecheckFails(HasSubstr("Match patterns are not exhaustive")));
  EXPECT_THAT(declaration + R"(
fn f(x: Choice) -> bool { match x { Choice::Left(_) => true } }
)",
              TypecheckFails(HasSubstr("Match patterns are not exhaustive")));
}

TEST(TypecheckV2Test,
     SumMatchWithoutInvalidPatternRejectsRefutableFinalConstructorPayload) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    Option::None => u8:0,
    Option::Some(u8:7) => u8:7,
  }
}
)",
              TypecheckFails(HasSubstr(
                  "A sum match without `invalid!` must end with `_` or one "
                  "constructor pattern whose payload subpatterns are "
                  "irrefutable.")));
}

TEST(TypecheckV2Test,
     SumMatchWithoutInvalidPatternAcceptsIrrefutableFinalConstructorPayload) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    Option::None => u8:0,
    Option::Some(v) => v,
  }
}
)",
              TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test,
     SumMatchWithoutInvalidPatternAcceptsPayloadlessFinalConstructor) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    Option::Some(v) => v,
    Option::None => u8:0,
  }
}
)",
              TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, SumMatchRejectsQualifiedValueConstantAsFinalFallback) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(R"(
pub enum E: u2 { A, B(bool) }
pub const A = E::B(false);
)",
                            "other", &import_data));
  EXPECT_THAT(TypecheckV2(R"(
import other;
fn f(x: other::E) -> u8 {
  match x {
    other::E::A => u8:0,
    other::E::B(true) => u8:1,
    other::A => u8:2,
  }
}
)",
                          "main", &import_data),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("A sum match without `invalid!` must end "
                                 "with `_` or one constructor pattern")));
}

TEST(TypecheckV2Test, SumMatchQualifiedValueConstantWithInvalidArm) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(R"(
pub enum E: u2 { A(), B(bool) }
pub const LAST = E::B(false);
)",
                            "other", &import_data));
  XLS_EXPECT_OK(TypecheckV2(R"(
import other;
fn f(x: other::E) -> u8 {
  match x {
    other::E::A() => u8:0,
    other::E::B(true) => u8:1,
    other::LAST => u8:2,
    invalid! => u8:3,
  }
}
)",
                            "main", &import_data));
}

TEST(TypecheckV2Test, SumMatchQualifiedUnitConstructorAsFinalFallback) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(R"(
pub enum E: u2 { A, B(bool) }
pub type Alias = E;
)",
                            "other", &import_data));
  XLS_EXPECT_OK(TypecheckV2(R"(
import other;
fn f(x: other::E) -> u8 {
  match x {
    other::E::B(_) => u8:1,
    other::Alias::A => u8:0,
  }
}
)",
                            "main", &import_data));
}

TEST(TypecheckV2Test, InvalidPatternMustBeTopLevel) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u8),
}
fn f(x: Option) -> u8 {
  match x {
    Option::Some(invalid!) => u8:0,
    _ => u8:1,
  }
}
)",
              TypecheckFails(HasSubstr(
                  "`invalid!` is only allowed as a top-level match arm "
                  "pattern.")));
}

TEST(TypecheckV2Test, MatchWithSemanticSumConstructors) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}

fn unwrap_or_zero(x: MaybeU32) -> u32 {
  match x {
    MaybeU32::Some(v) => v,
    MaybeU32::None => u32:0,
  }
}

const X = unwrap_or_zero(MaybeU32::Some(u32:7));
)",
      TypecheckSucceeds(AllOf(HasNodeWithType("v", "uN[32]"),
                              HasNodeWithType("X", "uN[32]"))));
}

TEST(TypecheckV2Test, SemanticSumOperationsPreserveImplicitTokenRequirements) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum E { A(u8), B(u8) }
fn fallback(x: E) -> u8 {
  match x { E::A(v) => v, E::B(v) => v }
}
fn explicit_invalid(x: E) -> u8 {
  match x { E::A(v) => v, E::B(v) => v, invalid! => u8:0 }
}
fn compare(x: E, y: E) -> bool { x == y || x != y }
fn failing(x: E) -> u8 {
  match x { E::A(v) => v, E::B(v) => fail!("B", v) }
}
)"));
  for (std::string_view name : {"fallback", "explicit_invalid", "compare"}) {
    SCOPED_TRACE(name);
    std::optional<Function*> function = result.tm.module->GetFunction(name);
    ASSERT_TRUE(function.has_value());
    EXPECT_EQ(result.tm.type_info->GetRequiresImplicitToken(**function), false);
  }
  std::optional<Function*> failing = result.tm.module->GetFunction("failing");
  ASSERT_TRUE(failing.has_value());
  EXPECT_EQ(result.tm.type_info->GetRequiresImplicitToken(**failing), true);
}

TEST(TypecheckV2Test, SemanticSumStructPatternsRejectInvalidMemberNames) {
  constexpr std::string_view kProgram = R"(
enum E { Pair { x: u8, y: u8 } }
fn f(value: E) -> u8 {
  match value { E::Pair { $0 } => u8:0 }
}
)";
  constexpr std::pair<std::string_view, std::string_view> kCases[] = {
      {"z, y", "Constructor `E::Pair` has no member `z`."},
      {"x, x: other, y",
       "Duplicate payload pattern for `x` in constructor `E::Pair`."},
      {"x", "Constructor pattern `E::Pair` is missing member(s): `y`"},
  };
  for (const auto& [fields, error] : kCases) {
    SCOPED_TRACE(fields);
    EXPECT_THAT(absl::Substitute(kProgram, fields),
                TypecheckFails(HasSubstr(error)));
  }
}

TEST(TypecheckV2Test, EmptyTupleConstructorRejectsStructPatternSyntax) {
  EXPECT_THAT(
      R"(
enum Choice {
  Empty(),
}

fn f(x: Choice) -> u32 {
  match x {
    Choice::Empty {} => u32:0,
  }
}
)",
      TypecheckFails(HasSubstr("does not support named payload patterns")));
}

TEST(TypecheckV2Test, EmptyStructConstructorRejectsTuplePatternSyntax) {
  EXPECT_THAT(
      R"(
enum Choice {
  Empty {},
}

fn f(x: Choice) -> u32 {
  match x {
    Choice::Empty() => u32:0,
  }
}
)",
      TypecheckFails(
          HasSubstr("does not support positional payload patterns")));
}

TEST(TypecheckV2Test,
     ParametricSemanticTupleConstructorUsesContextualPayloadType) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum OptionN<N: u32> {
  None,
  Some(uN[N]),
}

fn make(x: u16) -> OptionN<u32:8> {
  OptionN::Some(x)
}
)",
      TypecheckFails(AllOf(HasSubstr("size mismatch"), HasSubstr("u16"),
                           HasSubstr("uN[8]"))));
}

TEST(TypecheckV2Test, ParametricSemanticSumIdentityIncludesUnusedBinding) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Phantom<N: u32> {
  Only(),
}

fn convert(value: Phantom<u32:1>) -> Phantom<u32:2> {
  value
}
)",
      TypecheckFails(
          HasSubstr("Value mismatch for parametric `N` of sum `Phantom`")));
}

TEST(TypecheckV2Test, ConcreteSemanticSumIdentityIncludesUnusedBinding) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Phantom<N: u32> {
  Only(),
}

fn first(value: Phantom<u32:1>) -> Phantom<u32:1> {
  value
}

fn second(value: Phantom<u32:2>) -> Phantom<u32:2> {
  value
}
)"));
  Function* first = result.tm.module->GetFunction("first").value();
  Function* second = result.tm.module->GetFunction("second").value();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * first_type,
                           result.tm.type_info->GetItemAs<FunctionType>(first));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * second_type,
      result.tm.type_info->GetItemAs<FunctionType>(second));

  ASSERT_EQ(first_type->params().size(), 1);
  ASSERT_EQ(second_type->params().size(), 1);
  EXPECT_NE(*first_type->params()[0], *second_type->params()[0]);
  EXPECT_EQ(first_type->params()[0]->ToString(), "Phantom<u32:1> { Only() }");
  EXPECT_EQ(second_type->params()[0]->ToString(), "Phantom<u32:2> { Only() }");
  EXPECT_EQ(*first_type->params()[0],
            *first_type->params()[0]->CloneToUnique());
  EXPECT_NE(*first_type->params()[0]->CloneToUnique(),
            *second_type->params()[0]->CloneToUnique());
}

TEST(TypecheckV2Test, ConcreteSemanticSumIdentityIncludesUnusedTypeBinding) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Phantom<T: type> {
  Only(),
}

type ByteAlias = u8;

fn first(value: Phantom<u8>) -> Phantom<u8> {
  value
}

fn second(value: Phantom<u16>) -> Phantom<u16> {
  value
}

fn alias(value: Phantom<ByteAlias>) -> Phantom<ByteAlias> {
  value
}
)"));
  Function* first = result.tm.module->GetFunction("first").value();
  Function* second = result.tm.module->GetFunction("second").value();
  Function* alias = result.tm.module->GetFunction("alias").value();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(alias, nullptr);
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * first_type,
                           result.tm.type_info->GetItemAs<FunctionType>(first));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * second_type,
      result.tm.type_info->GetItemAs<FunctionType>(second));
  XLS_ASSERT_OK_AND_ASSIGN(FunctionType * alias_type,
                           result.tm.type_info->GetItemAs<FunctionType>(alias));

  EXPECT_NE(*first_type->params()[0], *second_type->params()[0]);
  EXPECT_EQ(*first_type->params()[0], *alias_type->params()[0]);
  EXPECT_EQ(first_type->params()[0]->ToString(), "Phantom<uN[8]> { Only() }");
  EXPECT_EQ(second_type->params()[0]->ToString(), "Phantom<uN[16]> { Only() }");
  EXPECT_EQ(first_type->params()[0]->ToString(),
            alias_type->params()[0]->ToString());
  EXPECT_EQ(*first_type->params()[0],
            *first_type->params()[0]->CloneToUnique());
}

TEST(TypecheckV2Test, TaggedSemanticSumWithUnusedTypeBinding) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Phantom<T: type>: u2 { Only() }
fn identity(value: Phantom<u8>) -> Phantom<u8> { value }
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  EXPECT_THAT(function_type->params()[0]->GetTotalBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(2)));
}

TEST(TypecheckV2Test, TaggedSemanticSumWithArrayTypeBinding) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Wrap<T: type, N: u32>: u2 { Item(T[N]) }
fn identity(value: Wrap<u8, u32:2>) -> Wrap<u8, u32:2> { value }
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  EXPECT_THAT(function_type->params()[0]->GetTotalBitCount(),
              IsOkAndHolds(TypeDim::CreateU32(18)));
}

// Each concrete instance owns its tag layout, including its discriminants.
TEST(TypecheckV2Test, TaggedSemanticSumResolvesParametricTagWidths) {
  for (const std::string_view program : {
           R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A(), B }
fn identity(narrow: E<u32:2>, wide: E<u32:4>) -> (E<u32:2>, E<u32:4>) {
  (narrow, wide)
}
)",
           R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A() = 0, B = 1 }
fn identity(narrow: E<u32:2>, wide: E<u32:4>) -> (E<u32:2>, E<u32:4>) {
  (narrow, wide)
}
)",
           R"(
#![feature(generics)]
enum E<T: type>: T { A() = 0, B = 1 }
fn identity(narrow: E<u2>, wide: E<u4>) -> (E<u2>, E<u4>) {
  (narrow, wide)
}
)"}) {
    SCOPED_TRACE(program);
    XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(program));
    Function* function = result.tm.module->GetFunction("identity").value();
    XLS_ASSERT_OK_AND_ASSIGN(
        FunctionType * function_type,
        result.tm.type_info->GetItemAs<FunctionType>(function));
    const int widths[] = {2, 4};
    for (int i = 0; i < 2; ++i) {
      const SumType& sum = function_type->params()[i]->AsSum();
      EXPECT_THAT(sum.GetTotalBitCount(),
                  IsOkAndHolds(TypeDim::CreateU32(widths[i])));
      EXPECT_THAT(sum.tag_bit_count().GetAsInt64(), IsOkAndHolds(widths[i]));
      EXPECT_EQ(sum.GetDiscriminant(0), InterpValue::MakeUBits(widths[i], 0));
      EXPECT_EQ(sum.GetDiscriminant(1), InterpValue::MakeUBits(widths[i], 1));
    }
  }
}

TEST(TypecheckV2Test, TaggedSemanticSumResolvesSignedParametricDiscriminants) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32>: sN[N] { Before() = -1, At = 0 }
fn identity(narrow: E<u32:1>, wide: E<u32:3>) -> (E<u32:1>, E<u32:3>) {
  (narrow, wide)
}
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const int widths[] = {1, 3};
  for (int i = 0; i < 2; ++i) {
    const SumType& sum = function_type->params()[i]->AsSum();
    EXPECT_THAT(sum.GetTotalBitCount(),
                IsOkAndHolds(TypeDim::CreateU32(widths[i])));
    EXPECT_EQ(sum.GetDiscriminant(0), InterpValue::MakeSBits(widths[i], -1));
    EXPECT_EQ(sum.GetDiscriminant(1), InterpValue::MakeSBits(widths[i], 0));
  }
}

TEST(TypecheckV2Test, TaggedSemanticSumTypesCompoundParametricDiscriminants) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A() = 1 + 1, B = 3 }
fn make() -> E<u32:2> { E<u32:2>::A() }
fn wide() -> E<u32:4> { E<u32:4>::B }
)"));
  Function* function = result.tm.module->GetFunction("make").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const SumType& sum = function_type->return_type().AsSum();
  EXPECT_EQ(sum.GetDiscriminant(0), InterpValue::MakeUBits(2, 2));
  EXPECT_EQ(sum.GetDiscriminant(1), InterpValue::MakeUBits(2, 3));
}

// Negative test: concrete tag widths must be checked before constructing a sum.
TEST(TypecheckV2Test, TaggedSemanticSumRejectsInsufficientParametricTagWidth) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A(), B, C }
fn identity(value: E<u32:1>) -> E<u32:1> { value }
)",
              TypecheckFails(HasSubstr("needs at least 2 tag bits")));
  EXPECT_THAT(R"(
#![feature(generics)]
enum E<N: u32>: sN[N] { A(), B }
fn identity(value: E<u32:1>) -> E<u32:1> { value }
)",
              TypecheckFails(HasSubstr("needs at least 2 tag bits")));
}

// Negative test: overflow and duplicate tags remain invalid after
// instantiation.
TEST(TypecheckV2Test, TaggedSemanticSumRejectsInvalidParametricDiscriminants) {
  EXPECT_THAT(TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A() = 0, B = 4 }
fn identity(value: E<u32:2>) -> E<u32:2> { value }
)")
                  .status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("size mismatch: u3 vs. uN[2]")));
  EXPECT_THAT(R"(
#![feature(generics)]
enum E<N: u32>: uN[N] { A() = 1 + 1, B = 2 }
fn identity(value: E<u32:2>) -> E<u32:2> { value }
)",
              TypecheckFails(HasSubstr("duplicate discriminant")));
}

TEST(TypecheckV2Test, ImportedTaggedSemanticSumUsesCallerParametrics) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(R"(
#![feature(generics)]
pub enum E<N: u32>: uN[N] { A(), B }
)",
                            "imported", &import_data));
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
import imported;
fn make<N: u32>() -> imported::E<N> { imported::E<N>::A() }
const NARROW = make<u32:2>();
const WIDE = make<u32:4>();
)",
                            "main", &import_data)
                    .status());
}

TEST(TypecheckV2Test, TaggedSemanticSumTypesParametricDiscriminantCalls) {
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
fn one<M: u32>() -> uN[M] { uN[M]:1 }
enum E<N: u32>: uN[N] { A() = 0, B = one<N>() }
fn narrow() -> E<u32:2> { E<u32:2>::A() }
fn wide() -> E<u32:4> { E<u32:4>::B }
)")
                    .status());
}

TEST(TypecheckV2Test, ImportedSemanticSumTypesParametricMapDiscriminant) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult imported,
                           TypecheckV2(R"(
#![feature(generics)]
fn id(x: u32) -> u32 { x }
pub enum E<N: u32>: u32 { A() = map([N], id)[0] }
)",
                                       "defs", &import_data));
  ASSERT_TRUE(imported.tm.module->fs_path().has_value());
  EXPECT_EQ(imported.tm.module->fs_path()->generic_string(), "defs.x");
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
#![feature(generics)]
import defs;
fn identity(low: defs::E<u32:1>, high: defs::E<u32:7>)
    -> (defs::E<u32:1>, defs::E<u32:7>) {
  (low, high)
}
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  EXPECT_EQ(function_type->params()[0]->AsSum().GetDiscriminant(0),
            InterpValue::MakeU32(1));
  EXPECT_EQ(function_type->params()[1]->AsSum().GetDiscriminant(0),
            InterpValue::MakeU32(7));
}

TEST(TypecheckV2Test, TaggedSemanticSumResolvesParametricTagWidthCalls) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
fn width<M: u32>() -> u32 { M }
enum E<N: u32>: uN[width<N>()] { A(), B }
fn identity(narrow: E<u32:2>, wide: E<u32:4>) -> (E<u32:2>, E<u32:4>) {
  (narrow, wide)
}
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  EXPECT_THAT(function_type->params()[0]->AsSum().tag_bit_count().GetAsInt64(),
              IsOkAndHolds(2));
  EXPECT_THAT(function_type->params()[1]->AsSum().tag_bit_count().GetAsInt64(),
              IsOkAndHolds(4));
}

TEST(TypecheckV2Test, TaggedSemanticSumKeepsInstanceDiscriminantsDistinct) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<K: u32>: u32 { A() = K, B = K + u32:1 }
fn identity(low: E<u32:2>, high: E<u32:7>) -> (E<u32:2>, E<u32:7>) {
  (low, high)
}
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const int values[] = {2, 7};
  for (int i = 0; i < 2; ++i) {
    const SumType& sum = function_type->params()[i]->AsSum();
    EXPECT_EQ(sum.GetDiscriminant(0), InterpValue::MakeUBits(32, values[i]));
    EXPECT_EQ(sum.GetDiscriminant(1),
              InterpValue::MakeUBits(32, values[i] + 1));
  }
}

TEST(TypecheckV2Test, TaggedSemanticSumResolvesCallerDependentTypeArguments) {
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
enum E<T: type>: T { A() = 0, B = 1 }
fn make<M: u32>() -> E<uN[M]> { E<uN[M]>::A() }
const NARROW = make<u32:2>();
const WIDE = make<u32:4>();
)")
                    .status());
}

TEST(TypecheckV2Test, SemanticSumInfersTagsFromParametricDiscriminants) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<K: u32> { A() = K, B = K + u32:1 }
fn identity(low: E<u32:2>, high: E<u32:7>) -> (E<u32:2>, E<u32:7>) {
  (low, high)
}
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const int widths[] = {2, 4};
  const int values[] = {2, 7};
  for (int i = 0; i < 2; ++i) {
    const SumType& sum = function_type->params()[i]->AsSum();
    EXPECT_THAT(sum.tag_bit_count().GetAsInt64(), IsOkAndHolds(widths[i]));
    EXPECT_EQ(sum.GetDiscriminant(0),
              InterpValue::MakeUBits(widths[i], values[i]));
    EXPECT_EQ(sum.GetDiscriminant(1),
              InterpValue::MakeUBits(widths[i], values[i] + 1));
  }
}

TEST(TypecheckV2Test, GenericSemanticSumWithBareTypePayload) {
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn identity(value: Option<u8>) -> Option<u8> { value }
)"));
}

TEST(TypecheckV2Test, TaggedGenericSemanticSumWithBareTypePayload) {
  XLS_EXPECT_OK(TypecheckV2(R"(
#![feature(generics)]
enum Option<T: type>: u2 { None, Some(T) }
fn identity(value: Option<u8>) -> Option<u8> { value }
)"));
}

TEST(TypecheckV2Test, GenericSemanticSumBarePayloadConstructorsAndPatterns) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn make(value: u8) -> Option<u8> { Option::Some(value) }
fn unwrap(value: Option<u8>) -> u8 {
  match value {
    Option::None => u8:0,
    Option::Some(v) => v,
  }
}
const_assert!(unwrap(make(u8:7)) == u8:7);
)",
              TypecheckSucceeds(HasNodeWithType("v", "uN[8]")));
}

TEST(TypecheckV2Test, GenericSemanticSumInferredTupleConstructor) {
  EXPECT_THAT(
      R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn make(value: u8) -> Option<u8> { Option::Some(value) }
)",
      TypecheckSucceeds(HasNodeWithType(
          "Option::Some(value)", "Option<uN[8]> { None | Some(uN[8]) }")));
}

TEST(TypecheckV2Test, GenericSemanticSumInferredConstructorPatterns) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn unwrap(value: Option<u8>) -> u8 {
  match value {
    Option::None => u8:0,
    Option::Some(v) => v,
  }
}
const_assert!(unwrap(Option<u8>::Some(u8:7)) == u8:7);
)",
              TypecheckSucceeds(HasNodeWithType("v", "uN[8]")));
}

TEST(TypecheckV2Test, GenericSemanticSumInferredUnitConstructor) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn empty() -> Option<u8> { Option::None }
)",
              TypecheckSucceeds(HasNodeWithType(
                  "Option::None", "Option<uN[8]> { None | Some(uN[8]) }")));
}

TEST(TypecheckV2Test, GenericSemanticSumExplicitConstructorPatternsAndUnit) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn empty() -> Option<u8> { Option<u8>::None }
fn unwrap(value: Option<u8>) -> u8 {
  match value {
    Option<u8>::None => u8:0,
    Option<u8>::Some(v) => v,
  }
}
const_assert!(unwrap(empty()) == u8:0);
const_assert!(unwrap(Option<u8>::Some(u8:7)) == u8:7);
)",
              TypecheckSucceeds(HasNodeWithType("v", "uN[8]")));
}

TEST(TypecheckV2Test, GenericSemanticSumPatternRejectsWrongExplicitType) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
fn has_value(value: Option<u8>) -> bool {
  match value {
    Option<u16>::Some(_) => true,
    _ => false,
  }
}
)",
              TypecheckFails(HasSubstr(
                  "Value mismatch for parametric `T` of sum `Option`")));
}

TEST(TypecheckV2Test, GenericSemanticSumUnitRejectsMissingContext) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Option<T: type> { None, Some(T) }
const X = Option::None;
)",
              TypecheckFails(HasSubstr("must have all parametrics specified")));
}

TEST(TypecheckV2Test, GenericSemanticSumWithBareStructPayload) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Box<T: type> { Value { value: T } }
fn identity(value: Box<u8>) -> Box<u8> { value }
)",
              TypecheckSucceeds(HasNodeWithType(
                  "value", "Box<uN[8]> { Value { value: uN[8] } }")));
}

TEST(TypecheckV2Test, GenericSemanticSumWithTypeOnlyAggregatePayload) {
  EXPECT_THAT(R"(
#![feature(generics)]
enum Box<T: type> { Value((T, T[2])) }
fn identity(value: Box<u8>) -> Box<u8> { value }
)",
              TypecheckSucceeds(HasNodeWithType(
                  "value", "Box<uN[8]> { Value((uN[8], uN[8][2])) }")));
}

TEST(TypecheckV2Test,
     GenericSemanticSumBarePayloadPreservesAliasAndImportIdentity) {
  constexpr std::string_view kImported = R"(
#![feature(generics)]
pub enum Option<T: type> { None, Some(T) }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "left", &import_data));
  XLS_ASSERT_OK(TypecheckV2(kImported, "right", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
#![feature(generics)]
import left;
import right;
type Byte = u8;
fn f(a: left::Option<u8>, alias: left::Option<Byte>,
     wider: left::Option<u16>, other: right::Option<u8>) -> left::Option<Byte> { a }
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto& params = function_type->params();
  EXPECT_EQ(*params[0], *params[1]);
  EXPECT_EQ(*params[0], function_type->return_type());
  EXPECT_NE(*params[0], *params[2]);
  EXPECT_NE(*params[0], *params[3]);
  EXPECT_EQ(params[0]->AsSum().variants()[1].GetMemberType(0),
            *BitsType::MakeU8());
}

// Verifies: nested and repeated sums share completed immutable type data.
// Catches: recursive duplication of a type argument and its payload use.
TEST(TypecheckV2Test, NestedSemanticSumReusesCompletedTypes) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Wrap<T: type, N: u32> {
  Item(T[N]),
}

type W1 = Wrap<u1, u32:1>;
type W2 = Wrap<W1, u32:1>;

fn identity(value: W2) -> Wrap<Wrap<u1, u32:1>, u32:1> { value }
)"));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto* sum =
      dynamic_cast<const SumType*>(function_type->params().at(0).get());
  const auto* result_sum =
      dynamic_cast<const SumType*>(&function_type->return_type());
  ASSERT_NE(sum, nullptr);
  ASSERT_NE(result_sum, nullptr);

  const auto* type_argument = dynamic_cast<const SumType*>(
      std::get<std::unique_ptr<const Type>>(sum->parametric_arguments().at(0))
          .get());
  const auto* payload_array =
      dynamic_cast<const ArrayType*>(&sum->variants().at(0).GetMemberType(0));
  ASSERT_NE(type_argument, nullptr);
  ASSERT_NE(payload_array, nullptr);
  const auto* payload_sum =
      dynamic_cast<const SumType*>(&payload_array->element_type());
  ASSERT_NE(payload_sum, nullptr);

  // W2 is one bit, but recursively copying both W1 descriptions makes the
  // compiler's retained type tree grow exponentially with this nesting.
  EXPECT_THAT(sum->GetTotalBitCount(), IsOkAndHolds(TypeDim::CreateU32(1)));
  EXPECT_NE(type_argument, payload_sum);
  EXPECT_EQ(&type_argument->variants(), &payload_sum->variants());

  // The alias and explicit instantiation resolve to the same completed data,
  // even though each use still owns its outer Type wrapper.
  EXPECT_NE(sum, result_sum);
  EXPECT_EQ(&sum->variants(), &result_sum->variants());
  EXPECT_EQ(*sum, *result_sum);
  EXPECT_EQ(sum->parametric_arguments_hash(),
            SumType::HashParametricArguments(sum->parametric_arguments()));
  EXPECT_EQ(type_argument->parametric_arguments_hash(),
            payload_sum->parametric_arguments_hash());
}

// Verifies: nested arguments and unused values distinguish sums; aliases reuse.
// Catches: treating equal total width as type identity in the sum cache.
TEST(TypecheckV2Test, SemanticSumCachePreservesNestedArgumentIdentity) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum Phantom<T: type, N: u32> { Only(), }
type PairAlias = (u1, u7);
type First = Phantom<PairAlias, u32:1>;
type Second = Phantom<(u2, u6), u32:1>;
type Third = Phantom<PairAlias, u32:2>;

fn f(a: First, b: Second, c: Third,
     alias: Phantom<(u1, u7), u32:1>) -> First { alias }
)"));
  Function* function = result.tm.module->GetFunction("f").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto& first = function_type->params()[0]->AsSum();
  const auto& second = function_type->params()[1]->AsSum();
  const auto& third = function_type->params()[2]->AsSum();
  const auto& alias = function_type->params()[3]->AsSum();
  EXPECT_NE(first, second);
  EXPECT_NE(first, third);
  EXPECT_EQ(first, alias);
  EXPECT_EQ(&first.variants(), &alias.variants());
  EXPECT_EQ(first.parametric_arguments_hash(),
            alias.parametric_arguments_hash());
}

// Verifies: same-spelling imported sums retain distinct nominal identities.
// Catches: reuse keyed by a sum name instead of its actual declaration.
TEST(TypecheckV2Test, SemanticSumReusePreservesImportedDefinitionIdentity) {
  constexpr std::string_view kImported = R"(
pub enum Wrap {
  Item(u1),
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "left", &import_data));
  XLS_ASSERT_OK(TypecheckV2(kImported, "right", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
import left;
import right;

fn pair(a: left::Wrap, b: right::Wrap) -> (left::Wrap, right::Wrap) { (a, b) }
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("pair").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const auto* left =
      dynamic_cast<const SumType*>(function_type->params().at(0).get());
  const auto* right =
      dynamic_cast<const SumType*>(function_type->params().at(1).get());
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->ToString(), right->ToString());
  EXPECT_NE(&left->nominal_type(), &right->nominal_type());
  EXPECT_NE(*left, *right);
  EXPECT_NE(&left->variants(), &right->variants());
}

TEST(TypecheckV2Test,
     ParametricSemanticStructConstructorUsesContextualPayloadType) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum BoxN<N: u32> {
  None,
  Pair { value: uN[N] },
}

fn make(x: u16) -> BoxN<u32:8> {
  BoxN::Pair { value: x }
}
)",
      TypecheckFails(AllOf(HasSubstr("size mismatch"), HasSubstr("u16"),
                           HasSubstr("uN[8]"))));
}

TEST(TypecheckV2Test, ExplicitSemanticSumParametricsMustAgreeWithContext) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
fn f(x: u16) -> E<u32:16> { E<u32:8>::V(x) }
)",
      TypecheckFails(HasSubstr("Value mismatch for parametric `N`")));
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
fn f(x: u16) -> E<u32:16> { E<u32:16>::V(x) }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, ParametricSemanticTupleConstructorInfersValueParametric) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum OptionN<N: u32> {
  None,
  Some(uN[N]),
}

const X = OptionN::Some(u7:7);
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, ParametricSemanticSumAliasInfersValueParametric) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum OptionN<N: u32> {
  None,
  Some(uN[N]),
}
type AbstractOption = OptionN;

const X = AbstractOption::Some(u7:7);
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, ParametricSemanticStructConstructorInfersParametric) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<N: u32> {
  Empty,
  Value { item: uN[N] },
}

const X = Box::Value { item: u16:7 };
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, ImportedParametricSemanticConstructorInfersParametric) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum Option<N: u32> {
  None,
  Some(uN[N]),
}
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;

const X = imported::Option::Some(u8:7);
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(kProgram, "main", &import_data),
              IsOkAndHolds(::testing::_));
}

TEST(TypecheckV2Test, ParametricSemanticSumPatternsBindPayloadTypes) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum OptionN<N: u32> {
  None,
  Some(uN[N]),
  Pair { value: uN[N] },
}

fn unwrap_or_zero(x: OptionN<u32:8>) -> u8 {
  match x {
    OptionN<u32:8>::Some(v) => v,
    OptionN<u32:8>::Pair { value } => value,
    OptionN<u32:8>::None => u8:0,
  }
}

const X = unwrap_or_zero(OptionN<u32:8>::Some(u8:7));
const INPUT_Y: OptionN<u32:8> = OptionN::Pair { value: u8:9 };
const Y = unwrap_or_zero(INPUT_Y);
)",
      TypecheckSucceeds(AllOf(
          HasNodeWithType("v", "uN[8]"), HasNodeWithType("value", "uN[8]"),
          HasNodeWithType("X", "uN[8]"), HasNodeWithType("Y", "uN[8]"))));
}

TEST(TypecheckV2Test, NonUnitSemanticSumConstructorCannotBeUsedAsValue) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}

fn f() -> () {
  let make = MaybeU32::Some;
  ()
}
)",
      TypecheckFails(AllOf(HasSubstr("MaybeU32::Some"),
                           HasSubstr("cannot be used as a value"))));
}

TEST(TypecheckV2Test, MatchWithSemanticSumConstructorsNonExhaustive) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
  Pair { lhs: u32, rhs: u32 },
}

fn unwrap_or_zero(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    Option::None => u32:0,
  }
}
)",
      TypecheckFails(
          AllOf(HasSubstr("Match patterns are not exhaustive"),
                HasSubstr("`Option::Pair { lhs: u32:0, rhs: u32:0 }` is not "
                          "covered"))));
}

TEST(TypecheckV2Test, NonExhaustiveSemanticSumReportsMissingUnitConstructor) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u32),
}

fn f(value: Option) -> u32 {
  match value {
    Option::Some(_) => u32:0,
  }
}
)",
              TypecheckFails(HasSubstr("`Option::None` is not covered")));
}

TEST(TypecheckV2Test,
     NonExhaustiveSemanticSumReportsEmptyTupleConstructorShape) {
  EXPECT_THAT(R"(
enum Shape {
  Unit,
  EmptyTuple(),
}

fn f(value: Shape) -> u32 {
  match value {
    Shape::Unit => u32:0,
  }
}
)",
              TypecheckFails(HasSubstr("`Shape::EmptyTuple()` is not "
                                       "covered")));
}

TEST(TypecheckV2Test,
     NonExhaustiveSemanticSumReportsEmptyStructConstructorShape) {
  EXPECT_THAT(R"(
enum Shape {
  Unit,
  EmptyStruct {},
}

fn f(value: Shape) -> u32 {
  match value {
    Shape::Unit => u32:0,
  }
}
)",
              TypecheckFails(HasSubstr("`Shape::EmptyStruct { }` is not "
                                       "covered")));
}

TEST(TypecheckV2Test, NonExhaustiveSemanticSumReportsNamedEnumTuplePayload) {
  EXPECT_THAT(
      R"(
enum E: u2 { A = 0, B = 1, C = 2 }

enum Option {
  None,
  Some(E),
}

fn f(value: Option) -> u32 {
  match value {
    Option::None => u32:0,
    Option::Some(E::A) => u32:1,
    Option::Some(E::C) => u32:2,
  }
}
)",
      TypecheckFails(HasSubstr("`Option::Some(E::B)` is not covered")));
}

TEST(TypecheckV2Test, NonExhaustiveSemanticSumReportsNamedEnumStructPayload) {
  EXPECT_THAT(
      R"(
enum E: u2 { A = 0, B = 1, C = 2 }

enum Message {
  Empty,
  Data { flag: bool, state: E },
}

fn f(value: Message) -> u32 {
  match value {
    Message::Empty => u32:0,
    Message::Data { flag: _, state: E::A } => u32:1,
    Message::Data { flag: _, state: E::C } => u32:2,
  }
}
)",
      TypecheckFails(HasSubstr(
          "`Message::Data { flag: u1:0, state: E::B }` is not covered")));
}

TEST(TypecheckV2Test, NonExhaustiveTupleReportsNestedSemanticSumConstructor) {
  EXPECT_THAT(
      R"(
enum E: u2 { A = 0, B = 1, C = 2 }

enum Option {
  Before(u8),
  Some(E),
  After(u4),
}

fn f(value: (Option, bool)) -> u32 {
  match value {
    (Option::Before(_), _) => u32:0,
    (Option::Some(E::A), _) => u32:1,
    (Option::Some(E::C), _) => u32:2,
    (Option::After(_), _) => u32:3,
  }
}
)",
      TypecheckFails(HasSubstr("`(Option::Some(E::B), u1:0)` is not covered")));
}

TEST(TypecheckV2Test, MatchWithSemanticSumConstructorsWildcardCompletes) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
  Pair { lhs: u32, rhs: u32 },
}

fn unwrap_or_zero(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    _ => u32:0,
  }
}

const X = unwrap_or_zero(Option::Pair { lhs: u32:3, rhs: u32:4 });
)",
      TypecheckSucceeds(AllOf(HasNodeWithType("v", "uN[32]"),
                              HasNodeWithType("X", "uN[32]"))));
}

TEST(TypecheckV2Test, MatchOrPatternRejectsBindingInLaterAlternative) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::None | Option::Some(v) => v,
    _ => u32:0,
  }
}
)",
      TypecheckFails(AllOf(HasSubstr("Cannot bind names in a match arm with "
                                     "multiple patterns"),
                           HasSubstr("bound: v"))));
}

TEST(TypecheckV2Test, MatchOrPatternRejectsBindingInFirstAlternative) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::Some(v) | Option::None => v,
    _ => u32:0,
  }
}
)",
      TypecheckFails(AllOf(HasSubstr("Cannot bind names in a match arm with "
                                     "multiple patterns"),
                           HasSubstr("bound: v"))));
}

TEST(TypecheckV2Test, WildcardAfterSumConstructorCannotPrecedeAnotherArm) {
  EXPECT_THAT(R"(
enum Option {
  None,
  Some(u32),
  Pair { lhs: u32, rhs: u32 },
}

fn unwrap_or_zero(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    _ => u32:0,
    Option::None => u32:1,
  }
}
)",
              TypecheckFails(HasSubstr(
                  "A wildcard arm may only be followed by a final `invalid!` "
                  "arm.")));
}

TEST(TypecheckV2Test, ZeroMacroImplicitSemanticSumUsesFirstVariant) {
  EXPECT_THAT(
      R"(
enum MaybeU32 {
  None,
  Some(u32),
}
const Y = zero!<MaybeU32>();
)",
      TypecheckSucceeds(
          HasNodeWithType("Y", "MaybeU32 { None | Some(uN[32]) }")));
}

TEST(TypecheckV2Test, ZeroMacroExplicitSemanticSumUsesZeroDiscriminant) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Message : u3 {
  Request(u8) = 3,
  Idle() = 0,
}
const Y = zero!<Message>();
)"));
  EXPECT_THAT(result, HasTypeInfo(HasNodeWithType(
                          "Y", "Message { Request(uN[8]) | Idle() }")));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  EXPECT_EQ(value, internal::CreateEncodedSumTuple(InterpValue::MakeUBits(3, 0),
                                                   InterpValue::MakeU8(0)));
}

TEST(TypecheckV2Test, AllOnesMacroSemanticSumReportsUnsupportedType) {
  EXPECT_THAT(
      R"(
enum Message { Idle, Data(u8) }
const Y = all_ones!<Message>();
)",
      TypecheckFails(HasSubstr(
          "Cannot use `all_ones!<Message>()` with sum type `Message`")));
}

TEST(TypecheckV2Test, ZeroMacroTupleContainingSemanticSumHasConstexprValue) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Option {
  None,
  Some(u32),
}
const Y = zero!<(Option,)>();
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  const InterpValue none = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(1, 0), InterpValue::MakeU32(0));
  EXPECT_EQ(value, InterpValue::MakeTuple({none}));
}

TEST(TypecheckV2Test, ZeroMacroArrayContainingSemanticSumHasConstexprValue) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Option {
  None,
  Some(u32),
}
const Y = zero!<Option[1]>();
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  const InterpValue none = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(1, 0), InterpValue::MakeU32(0));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue expected,
                           InterpValue::MakeArray({none}));
  EXPECT_EQ(value, expected);
}

TEST(TypecheckV2Test, ZeroMacroStructContainingSemanticSumHasConstexprValue) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum Option {
  None,
  Some(u32),
}
struct Wrapper {
  value: Option,
}
const Y = zero!<Wrapper>();
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  const InterpValue none = internal::CreateEncodedSumTuple(
      InterpValue::MakeUBits(1, 0), InterpValue::MakeU32(0));
  EXPECT_EQ(value, InterpValue::MakeTuple({none}));
}

TEST(TypecheckV2Test, ZeroMacroExplicitSemanticSumWithoutZeroFails) {
  EXPECT_THAT(
      R"(
enum Message : u3 {
  Request(u8) = 3,
  Response(u8) = 7,
}
const Y = zero!<Message>();
)",
      TypecheckFails(
          HasSubstr("Sum type 'Message' does not have a known zero value.")));
}

TEST(TypecheckV2Test, ZeroMacroGenericSemanticSumKeepsEachInstancesVariant) {
  constexpr std::string_view kPrograms[] = {
      R"(#![feature(generics)]
enum E<N: u32>: u32 { A(u8) = N, B(u8) = N ^ u32:1 }
const Y = zero!<(E<u32:0>, E<u32:1>)>();
)",
      R"(#![feature(generics)]
enum E<N: u32>: u32 { A(u8) = N, B(u8) = N ^ u32:1 }
const Y = zero!<(E<u32:1>, E<u32:0>)>();
)",
  };
  for (int first_tag = 0; first_tag < 2; ++first_tag) {
    SCOPED_TRACE(kPrograms[first_tag]);
    XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                             TypecheckV2(kPrograms[first_tag]));
    XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                             result.tm.module->GetConstantDef("Y"));
    XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                             result.tm.type_info->GetConstExpr(constant));
    // Instantiations choose different constructors, but their semantic wire
    // tags and packed payload slots must both be zero in either order.
    const InterpValue zero = internal::CreateEncodedSumTuple(
        InterpValue::MakeU32(0), InterpValue::MakeU8(0));
    EXPECT_EQ(value, InterpValue::MakeTuple({zero, zero}));
    XLS_ASSERT_OK_AND_ASSIGN(
        Type * type, result.tm.type_info->GetItemOrError(constant->name_def()));
    ASSERT_TRUE(type->IsTuple());
    const TupleType& tuple_type = type->AsTuple();
    EXPECT_EQ(tuple_type.GetMemberType(0).AsSum().GetDiscriminant(0),
              InterpValue::MakeU32(first_tag));
    EXPECT_EQ(tuple_type.GetMemberType(1).AsSum().GetDiscriminant(0),
              InterpValue::MakeU32(1 - first_tag));
  }
}

TEST(TypecheckV2Test, ZeroMacroGenericSemanticSumInConstexprFunction) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32>: u32 { A(u8) = N, B(u8) = N ^ u32:1 }
fn make() -> (E<u32:0>, E<u32:1>) {
  zero!<(E<u32:0>, E<u32:1>)>()
}
const Y = make();
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  const InterpValue zero = internal::CreateEncodedSumTuple(
      InterpValue::MakeU32(0), InterpValue::MakeU8(0));
  EXPECT_EQ(value, InterpValue::MakeTuple({zero, zero}));
}

TEST(TypecheckV2Test, ZeroMacroImportedGenericSumInStructAndArray) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum E<N: u32>: u32 { A(u8) = N, B(u8) = N ^ u32:1 }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
type Alias = imported::E<u32:1>;
struct Wrapper { first: imported::E<u32:0>, rest: Alias[2] }
const Y = zero!<Wrapper>();
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(kProgram, "main", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                           result.tm.module->GetConstantDef("Y"));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           result.tm.type_info->GetConstExpr(constant));
  const InterpValue zero = internal::CreateEncodedSumTuple(
      InterpValue::MakeU32(0), InterpValue::MakeU8(0));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue array,
                           InterpValue::MakeArray({zero, zero}));
  EXPECT_EQ(value, InterpValue::MakeTuple({zero, array}));
}

TEST(TypecheckV2Test, ZeroMacroGenericSemanticSumWithoutZeroFails) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32>: u32 { A(u8) = N, B(u8) = N ^ u32:1 }
const Y = zero!<(E<u32:2>,)>();
)",
      TypecheckFails(
          HasSubstr("Sum type 'E' does not have a known zero value.")));
}

TEST(TypecheckV2Test,
     ZeroMacroGenericSemanticSumRequiresZeroConstructiblePayload) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Never {}
enum E<N: u32>: u32 { A(Never) = N }
const Y = zero!<(E<u32:0>,)>();
)",
      TypecheckFails(
          HasSubstr("Sum type 'Never' does not have a known zero value.")));
}

TEST(TypecheckV2Test, ZeroMacroEmptySemanticSumFails) {
  EXPECT_THAT(
      R"(
enum Never {}
const Y = zero!<Never>();
)",
      TypecheckFails(
          HasSubstr("Sum type 'Never' does not have a known zero value.")));
}

TEST(TypecheckV2Test, ZeroMacroAnnotatedEmptySemanticSumFails) {
  EXPECT_THAT(
      R"(
enum Never : u3 {}
const Y = zero!<Never>();
)",
      TypecheckFails(
          HasSubstr("Sum type 'Never' does not have a known zero value.")));
}

TEST(TypecheckV2Test, ZeroMacroImportedSemanticSumUsesFirstVariant) {
  constexpr std::string_view kImported = R"(
pub enum ImportedMaybe {
  None,
  Some(u32),
}
)";
  constexpr std::string_view kProgram = R"(
import imported;
const Y = zero!<imported::ImportedMaybe>();
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_EXPECT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(kProgram, "main", &import_data),
              IsOkAndHolds(HasTypeInfo(HasNodeWithType(
                  "Y", "ImportedMaybe { None | Some(uN[32]) }"))));
}

TEST(TypecheckV2Test,
     SemanticSumConstructorsRejectTypeParametricsWithoutBindings) {
  constexpr std::string_view kPrograms[] = {
      R"(#![feature(generics)]
enum E { V(u8) }
const X = E<u32:1>::V(u8:0);
)",
      R"(#![feature(generics)]
enum E { Unit, V(u8) }
const X = E<u32:1>::Unit;
)",
      R"(#![feature(generics)]
enum E { V { x: u8 } }
const X = E<u32:1>::V { x: u8:0 };
)",
  };
  for (std::string_view program : kPrograms) {
    SCOPED_TRACE(program);
    EXPECT_THAT(program, TypecheckFails(HasSubstr(
                             "Too many parametric values supplied; limit: 0 "
                             "given: 1")));
  }
}

TEST(TypecheckV2Test,
     ParametricSemanticTupleConstructorRejectsExtraTypeParametric) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
const X = E<u32:8, u32:16>::V(u8:0);
)",
      TypecheckFails(
          HasSubstr("Too many parametric values supplied; limit: 1 given: 2")));
}

TEST(TypecheckV2Test, SemanticTupleConstructorRejectsWrongValueParametricType) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(u8) }
const X = E<u8:8>::V(u8:0);
)",
      TypecheckFails(HasSubstr("size mismatch")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructorDuplicateMemberRejected) {
  EXPECT_THAT(
      R"(
enum E { V { x: u8 } }
const X = E::V { x: u8:0, x: u8:1 };
)",
      TypecheckFails(
          HasSubstr("Duplicate value seen for `x` in constructor `V`.")));
}

TEST(TypecheckV2Test, SemanticSumStructConstructorBindsReorderedMembersByName) {
  EXPECT_THAT(
      R"(
enum E { V { x: u8, y: u16 } }
const X = E::V { y: u16:2, x: u8:1 };
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E { V { x: uN[8], y: uN[16] } }")));
}

TEST(TypecheckV2Test, GenericSemanticSumDeclarationNeedsNoInstantiation) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, GenericSemanticSumExplicitAndInferredTypes) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
const EXPLICIT = Box<u8>::Value(u8:1);
const INFERRED = Box::Value(u8:2);
)",
      TypecheckSucceeds(
          AllOf(HasNodeWithType("EXPLICIT", "Box<uN[8]> { Value(uN[8]) }"),
                HasNodeWithType("INFERRED", "Box<uN[8]> { Value(uN[8]) }"))));
}

TEST(TypecheckV2Test, GenericSemanticSumRejectsMismatchedPayload) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
fn f(x: u16) -> Box<u8> { Box::Value(x) }
)",
      TypecheckFails(HasSubstr("size mismatch")));
}

TEST(TypecheckV2Test, GenericSemanticSumValidatesInstantiatedPayloadType) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
fn f(x: Box<u8[1]>) -> Box<u8[1]> { x }
)",
      TypecheckSucceeds(
          HasNodeWithType("x", "Box<uN[8][1]> { Value(uN[8][1]) }")));
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
fn f(x: Box<token>) -> Box<token> { x }
)",
      TypecheckFails(HasSubstr(
          "Semantic sum constructor `Value` cannot contain a token payload.")));
}

TEST(TypecheckV2Test, SemanticSumReferenceResolvesValueDefault) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32 = {u32:8}> { V(uN[N]) }
fn f(x: E) -> E { x }
const X = E::V(u8:1);
)",
      TypecheckSucceeds(HasNodeWithType("X", "E<u32:8> { V(uN[8]) }")));
}

TEST(TypecheckV2Test, SemanticSumReferenceResolvesTypeDefaultAndOverride) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type = u8> { Value(T) }
fn f(x: Box) -> Box { x }
const DEFAULT = Box::Value(u8:1);
const OVERRIDE = Box<u16>::Value(u16:2);
)",
      TypecheckSucceeds(
          AllOf(HasNodeWithType("DEFAULT", "Box<uN[8]> { Value(uN[8]) }"),
                HasNodeWithType("OVERRIDE", "Box<uN[16]> { Value(uN[16]) }"))));
}

TEST(TypecheckV2Test, SemanticSumValueDefaultSubstitutesLiteralType) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type = u8, V: T = {T:0}> { A(u8) }
fn f(x: E) -> E { x }
fn g(x: E<u16>) -> E<u16> { x }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, SemanticSumRecordParametricAcceptsNestedValue) {
  // A record parameter is admitted through its nominal annotation. Its enum,
  // nested arrays/tuples and empty array must survive normalization intact.
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(#![feature(generics)]
enum Flag: s3 { Neg = -1, Two = 2 }
struct Record { number: Flag, fields: (u3[2], (u2, u4[2])), empty: u8[0] }
const RECORD = Record {
  number: Flag::Neg,
  fields: (u3[2]:[1, 6], (u2:2, u4[2]:[3, 12])),
  empty: u8[0]:[],
};

enum Phantom<V: Record> { Only() }
fn accept(_x: Phantom<RECORD>) -> u1 { u1:0 }
)"));
  XLS_ASSERT_OK_AND_ASSIGN(
      Function * function,
      result.tm.module->GetMemberOrError<Function>("accept"));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  ASSERT_EQ(type->params().size(), 1);
  ASSERT_TRUE(type->params()[0]->IsSum());
  EXPECT_EQ(type->params()[0]->AsSum().nominal_type().identifier(), "Phantom");
}

TEST(TypecheckV2Test, ImportedSemanticSumRecordValuesRemainDistinctInDefaults) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(R"(#![feature(generics)]
pub struct Record { width: u32 }
pub const LOW = Record { width: u32:2 };
pub const HIGH = Record { width: u32:5 };
pub enum E<L: Record, R: Record = {Record { width: L.width + u32:1 }}>: u32 {
  Value(u8) = L.width + R.width,
}
)",
                            "defs", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(#![feature(generics)]
import defs;
fn values(explicit_values: defs::E<defs::LOW, defs::HIGH>,
          defaulted_value: defs::E<defs::HIGH>) {
  ()
}
)",
                                       "main", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(
      Function * function,
      result.tm.module->GetMemberOrError<Function>("values"));
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  ASSERT_EQ(type->params().size(), 2);
  ASSERT_TRUE(type->params()[0]->IsSum());
  ASSERT_TRUE(type->params()[1]->IsSum());
  const SumDef& definition = type->params()[0]->AsSum().nominal_type();
  EXPECT_EQ(&type->params()[1]->AsSum().nominal_type(), &definition);
  ASSERT_EQ(definition.variants().size(), 1);
  ASSERT_TRUE(definition.variants()[0]->discriminant().has_value());
  const Expr* source_discriminant = *definition.variants()[0]->discriminant();

  // Observe computed discriminant roots, as in the adjacent imported-tag
  // tests. Both records must retain their own values in the same expression:
  // explicit values compute 2 + 5; the dependent default computes 5 + 6.
  std::vector<InterpValue> evaluated_discriminants;
  for (const auto& [node, node_type] : result.tm.type_info->dict()) {
    if (node != source_discriminant && node->owner() == result.tm.module &&
        node->kind() == source_discriminant->kind() &&
        node->GetSpan() == source_discriminant->GetSpan() &&
        node->parent() == source_discriminant->parent()) {
      XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                               result.tm.type_info->GetConstExpr(node));
      evaluated_discriminants.push_back(value);
    }
  }
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(7)));
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(11)));
  EXPECT_THAT(evaluated_discriminants,
              ::testing::Each(::testing::AnyOf(InterpValue::MakeU32(7),
                                               InterpValue::MakeU32(11))));
}

TEST(TypecheckV2Test, SemanticSumInfersBeforeStructuredValueDefault) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(#![feature(generics)]
struct Record { width: u32 }
enum E<N: u32, V: Record = {Record { width: N + u32:1 }}>: u32 {
  Value(uN[N]) = V.width,
}
const VALUE = E::Value(u5:1);
)"));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * value,
                           result.tm.module->GetConstantDef("VALUE"));
  auto type = result.tm.type_info->GetItem(value->name_def());
  ASSERT_TRUE(type.has_value());
  ASSERT_TRUE((*type)->IsSum());
  const SumTypeVariant& variant = (*type)->AsSum().variants()[0];
  EXPECT_EQ(variant.GetMemberType(0).ToString(), "uN[5]");
  const SumDef& definition = (*type)->AsSum().nominal_type();
  ASSERT_EQ(definition.variants().size(), 1);
  ASSERT_TRUE(definition.variants()[0]->discriminant().has_value());
  const Expr* source_discriminant = *definition.variants()[0]->discriminant();

  // N must be inferred as 5 before constructing the default Record. Its
  // retained value is observed through the computed tag, independently of
  // the payload width that supplied the scalar inference evidence.
  std::vector<InterpValue> evaluated_discriminants;
  for (const auto& [node, node_type] : result.tm.type_info->dict()) {
    if (node != source_discriminant && node->owner() == result.tm.module &&
        node->kind() == source_discriminant->kind() &&
        node->GetSpan() == source_discriminant->GetSpan() &&
        node->parent() == source_discriminant->parent()) {
      XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                               result.tm.type_info->GetConstExpr(node));
      evaluated_discriminants.push_back(value);
    }
  }
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(6)));
  EXPECT_THAT(evaluated_discriminants,
              ::testing::Each(InterpValue::MakeU32(6)));
}

TEST(TypecheckV2Test, SemanticSumValueDefaultSubstitutesLiteralWidth) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32 = {u32:8}, V: uN[N] = {uN[N]:0}> { A(u8) }
fn f(x: E) -> E { x }
fn g(x: E<u32:16>) -> E<u32:16> { x }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, SemanticSumUntypedDefaultsKeepBindingWidth) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32 = {8}, V: uN[N] = {0}> { A(uN[N]) }
const NARROW = E::A(u8:0);
const WIDE = E<u32:16>::A(u16:0);
)",
      TypecheckSucceeds(
          AllOf(HasNodeWithType("NARROW", "E<u32:8, u8:0> { A(uN[8]) }"),
                HasNodeWithType("WIDE", "E<u32:16, u16:0> { A(uN[16]) }"))));
}

TEST(TypecheckV2Test, SemanticSumDefaultPreservesConstexprRolloverWarning) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
#![feature(generics)]
enum E<N: u32 = {u32:0xffff_ffff + u32:1}> { A(u8) }
fn f(x: E) -> E { x }
)"));
  EXPECT_THAT(result.tm.warnings.warnings(),
              Contains(AllOf(
                  Field(&WarningCollector::Entry::kind,
                        WarningKind::kConstexprEvalRollover),
                  Field(&WarningCollector::Entry::message,
                        HasSubstr("constexpr evaluation detected rollover")))));
}

TEST(TypecheckV2Test, SemanticSumTypeDefaultRejectsMismatchedPayload) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type = u8> { Value(T) }
const X = Box::Value(u16:1);
)",
      TypecheckFails(HasSubstr("size mismatch")));
}

TEST(TypecheckV2Test, SemanticSumInfersBeforeDependentValueDefault) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32, M: u32 = {N + u32:1}> { V(uN[N], uN[M]) }
const X = E::V(u8:1, u9:2);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<u32:8, u32:9> { V(uN[8], uN[9]) }")));
}

TEST(TypecheckV2Test, SemanticSumInfersBeforeValueDependentTypeDefault) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32, T: type = uN[N]> { V(uN[N], T) }
const X = E::V(u8:1, u8:2);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<u32:8, uN[8]> { V(uN[8], uN[8]) }")));
}

TEST(TypecheckV2Test, SemanticSumInfersBeforeTypeDependentTypeDefault) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type, U: type = T> { V(T, U) }
const X = E::V(u8:1, u8:2);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<uN[8], uN[8]> { V(uN[8], uN[8]) }")));
}

TEST(TypecheckV2Test, SemanticSumSubstitutesNestedTypeArgumentsInDefault) {
  EXPECT_THAT(
      R"(#![feature(generics)]
struct W<T: type> { x: T }
enum E<T: type, U: type = W<T>> { V(T) }
const X = E::V(u8:1);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<uN[8], W { x: uN[8] }> { V(uN[8]) }")));
}

TEST(TypecheckV2Test, SemanticSumKeepsConcreteValueBindingTypeInExpression) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type, V: T> { Value(uN[V + u32:1]) }
const X = E<u32, u32:7>::Value(u8:1);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<uN[32], u32:7> { Value(uN[8]) }")));
}

TEST(TypecheckV2Test, SemanticSumPartiallyExplicitTupleParametrics) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum TuplePair<N: u32, M: u32> { V(uN[N], uN[M]) }
const TUPLE = TuplePair<u32:8>::V(u8:1, u16:2);
)",
      TypecheckSucceeds(HasNodeWithType(
          "TUPLE", "TuplePair<u32:8, u32:16> { V(uN[8], uN[16]) }")));
}

TEST(TypecheckV2Test, SemanticSumPartiallyExplicitNamedParametrics) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum NamedPair<N: u32, M: u32> { V { x: uN[N], y: uN[M] } }
const NAMED = NamedPair<u32:8>::V { y: u16:2, x: u8:1 };
)",
      TypecheckSucceeds(HasNodeWithType(
          "NAMED", "NamedPair<u32:8, u32:16> { V { x: uN[8], y: uN[16] } }")));
}

TEST(TypecheckV2Test, SemanticSumAliasArgumentKeepsPayloadInference) {
  EXPECT_THAT(
      R"(#![feature(generics)]
type U = u32;
enum E<A: u32, B: u32> { V(uN[A], uN[B]) }
const X = E<U:8>::V(u8:1, u16:2);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<u32:8, u32:16> { V(uN[8], uN[16]) }")));
}

TEST(TypecheckV2Test, ImportedSemanticSumPartiallyExplicitNamedParametrics) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum E<N: u32, M: u32> { V { x: uN[N], y: uN[M] } }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
const X = imported::E<u32:8>::V { x: u8:1, y: u16:2 };
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(kProgram, "main", &import_data),
              IsOkAndHolds(HasTypeInfo(HasNodeWithType(
                  "X", "E<u32:8, u32:16> { V { x: uN[8], y: uN[16] } }"))));
}

TEST(TypecheckV2Test, SemanticSumConstructorInGenericFunction) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
fn wrap<N: u32>(x: uN[N]) -> E<N> { E::V(x) }
fn f() -> (E<u32:8>, E<u32:16>) {
  let a = wrap(u8:1);
  let b = wrap(u16:2);
  (a, b)
}
)",
      TypecheckSucceeds(
          AllOf(HasNodeWithType("a", "E<u32:8> { V(uN[8]) }"),
                HasNodeWithType("b", "E<u32:16> { V(uN[16]) }"))));
}

TEST(TypecheckV2Test, SemanticSumConstructorWithImportedTypeArgument) {
  constexpr std::string_view kImported = R"(
pub enum Kind: u8 { A = 0 }
)";
  constexpr std::string_view kPrograms[] = {
      R"(#![feature(generics)]
import imported;
enum E<T: type> { A(T) }
const X = E<imported::Kind>::A(imported::Kind::A);
)",
      R"(#![feature(generics)]
import imported;
type K = imported::Kind;
enum E<T: type> { A(T) }
const X = E<K>::A(imported::Kind::A);
)",
      R"(#![feature(generics)]
import imported;
enum E<T: type, N: u32> { A(T, uN[N]) }
const X = E<imported::Kind>::A(imported::Kind::A, u8:0);
)",
  };
  for (std::string_view program : kPrograms) {
    SCOPED_TRACE(program);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
    XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                             TypecheckV2(program, "main", &import_data));
    XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                             result.tm.module->GetConstantDef("X"));
    EXPECT_EQ(constant->value()->kind(), AstNodeKind::kSumInstance);
  }
}

TEST(TypecheckV2Test, SemanticSumValueArgumentUsesEarlierExplicitBinding) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32, V: uN[N]> { A(u8) }
const X = E<u32:8, u8:1>::A(u8:0);
)",
      TypecheckSucceeds(HasNodeWithType("X", "E<u32:8, u8:1> { A(uN[8]) }")));
}

TEST(TypecheckV2Test,
     PartialSemanticSumValueArgumentUsesEarlierExplicitBinding) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32, V: uN[N], M: u32> { A(uN[M]) }
const X = E<u32:8, u8:1>::A(u16:0);
)",
      TypecheckSucceeds(
          HasNodeWithType("X", "E<u32:8, u8:1, u32:16> { A(uN[16]) }")));
}

TEST(TypecheckV2Test, SemanticSumValueArgumentRejectsWrongWidth) {
  constexpr std::string_view kPrograms[] = {
      R"(#![feature(generics)]
enum E<N: u32, V: uN[N]> { A(u8) }
const X = E<u32:8, u16:1>::A(u8:0);
)",
      R"(#![feature(generics)]
enum E<N: u32, V: uN[N], M: u32> { A(uN[M]) }
const X = E<u32:8, u16:1>::A(u16:0);
)",
  };
  for (std::string_view program : kPrograms) {
    SCOPED_TRACE(program);
    EXPECT_THAT(program, TypecheckFails(HasSubstr("size mismatch")));
  }
}

TEST(TypecheckV2Test, GenericSemanticSumReportsMissingPayloadArgument) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { V(uN[N]) }
const X = E::V();
)",
      TypecheckFails(HasSubstr("Expected 1 argument(s) but got 0.")));
}

TEST(TypecheckV2Test, GenericSemanticSumChecksDeclaredTagWidth) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32>: u1 { V(uN[N]) = 2 }
const X = E<u32:8>::V(u8:0);
)",
      TypecheckFails(HasSubstr("size mismatch: u2 vs. u1")));
}

TEST(TypecheckV2Test, GenericSemanticSumChecksTagWidthPerInstantiation) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32>: uN[N] { A(u8) = 0, B(u8) = 2 }
const WIDE = E<u32:2>::A(u8:0);
const NARROW = E<u32:1>::A(u8:0);
)",
      TypecheckFails(HasSubstr("size mismatch: u2 vs. uN[1]")));
}

TEST(TypecheckV2Test, GenericSemanticSumChecksDuplicateTagsPerInstantiation) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32>: u32 { A(u8) = 0, B(u8) = N }
const DISTINCT = E<u32:1>::A(u8:0);
const DUPLICATE = E<u32:0>::A(u8:0);
)",
      TypecheckFails(HasSubstr("duplicate discriminant")));
}

TEST(TypecheckV2Test, GenericSemanticSumSubstitutesPrimitiveTypeMembers) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type>: T { A(u8) = T::ZERO, B(u8) = T::MAX }
fn f(x: E<u8>) -> E<u8> { x }
fn g(x: E<uN[16]>) -> E<uN[16]> { x }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, GenericSemanticSumPreservesNominalTypeMembers) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Tag: u8 { FIRST = 0, LAST = 1 }
enum E<T: type>: u8 { A(u8) = T::FIRST as u8, B(u8) = T::LAST as u8 }
fn f(x: E<Tag>) -> E<Tag> { x }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test, ImportedSemanticSumSubstitutesTypesInDiscriminants) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum E<T: type>: T { A(u8) = T:0, B(u8) = T:1 }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
const NARROW = imported::E<u8>::A(u8:0);
const WIDE = imported::E<u16>::B(u8:1);
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      IsOkAndHolds(HasTypeInfo(AllOf(
          HasNodeWithType("NARROW", "E<uN[8]> { A(uN[8]) | B(uN[8]) }"),
          HasNodeWithType("WIDE", "E<uN[16]> { A(uN[8]) | B(uN[8]) }")))));
}

TEST(TypecheckV2Test, ImportedSemanticSumTypesParametricMapDiscriminantValues) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult imported,
                           TypecheckV2(R"(
#![feature(generics)]
fn id(x: u32) -> u32 { x }
pub enum E<N: u32>: u32 { A() = map([N], id)[0] }
)",
                                       "defs", &import_data));
  ASSERT_TRUE(imported.tm.module->fs_path().has_value());
  EXPECT_EQ(imported.tm.module->fs_path()->generic_string(), "defs.x");
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
#![feature(generics)]
import defs;
fn identity(low: defs::E<u32:1>, high: defs::E<u32:7>)
    -> (defs::E<u32:1>, defs::E<u32:7>) {
  (low, high)
}
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const SumDef& definition = function_type->params()[0]->AsSum().nominal_type();
  EXPECT_EQ(&function_type->params()[1]->AsSum().nominal_type(), &definition);
  ASSERT_EQ(definition.variants().size(), 1);
  ASSERT_TRUE(definition.variants()[0]->discriminant().has_value());
  const Expr* source_discriminant = *definition.variants()[0]->discriminant();
  ASSERT_EQ(source_discriminant->kind(), AstNodeKind::kIndex);

  // Inspect the computed roots cloned from this declaration. The type-argument
  // literals cannot satisfy these checks: they have a different kind and span.
  std::vector<InterpValue> evaluated_discriminants;
  for (const auto& [node, type] : result.tm.type_info->dict()) {
    if (node->owner() == result.tm.module &&
        node->kind() == source_discriminant->kind() &&
        node->GetSpan() == source_discriminant->GetSpan() &&
        node->parent() == source_discriminant->parent()) {
      XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                               result.tm.type_info->GetConstExpr(node));
      evaluated_discriminants.push_back(value);
    }
  }
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(1)));
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(7)));
  EXPECT_THAT(evaluated_discriminants,
              ::testing::Each(::testing::AnyOf(InterpValue::MakeU32(1),
                                               InterpValue::MakeU32(7))));
}

TEST(TypecheckV2Test,
     ImportedSemanticSumTypesParametricInvocationDiscriminantValues) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult imported,
                           TypecheckV2(R"(
#![feature(generics)]
fn id<M: u32>(x: uN[M]) -> uN[M] { x }
pub enum E<N: u32>: u32 { A() = id(N) }
)",
                                       "defs", &import_data));
  ASSERT_TRUE(imported.tm.module->fs_path().has_value());
  EXPECT_EQ(imported.tm.module->fs_path()->generic_string(), "defs.x");
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
#![feature(generics)]
import defs;
fn identity(low: defs::E<u32:1>, high: defs::E<u32:7>)
    -> (defs::E<u32:1>, defs::E<u32:7>) {
  (low, high)
}
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const SumDef& definition = function_type->params()[0]->AsSum().nominal_type();
  EXPECT_EQ(&function_type->params()[1]->AsSum().nominal_type(), &definition);
  ASSERT_EQ(definition.variants().size(), 1);
  ASSERT_TRUE(definition.variants()[0]->discriminant().has_value());
  const Expr* source_discriminant = *definition.variants()[0]->discriminant();
  ASSERT_EQ(source_discriminant->kind(), AstNodeKind::kInvocation);

  // Both imported instantiations must evaluate their own discriminant. Match
  // the cloned invocation roots rather than the literal type arguments.
  std::vector<InterpValue> evaluated_discriminants;
  for (const auto& [node, type] : result.tm.type_info->dict()) {
    if (node->owner() == result.tm.module &&
        node->kind() == source_discriminant->kind() &&
        node->GetSpan() == source_discriminant->GetSpan() &&
        node->parent() == source_discriminant->parent()) {
      XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                               result.tm.type_info->GetConstExpr(node));
      evaluated_discriminants.push_back(value);
    }
  }
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(1)));
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(7)));
  EXPECT_THAT(evaluated_discriminants,
              ::testing::Each(::testing::AnyOf(InterpValue::MakeU32(1),
                                               InterpValue::MakeU32(7))));
}

TEST(TypecheckV2Test,
     ImportedSemanticSumTypesParametricTuplePatternDiscriminantValues) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult imported,
                           TypecheckV2(R"(
#![feature(generics)]
pub enum E<N: u32>: u32 {
  A() = { let (x, y) = (N, u32:0); x + y }
}
)",
                                       "defs", &import_data));
  ASSERT_TRUE(imported.tm.module->fs_path().has_value());
  EXPECT_EQ(imported.tm.module->fs_path()->generic_string(), "defs.x");
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(R"(
#![feature(generics)]
import defs;
fn identity(low: defs::E<u32:1>, high: defs::E<u32:7>)
    -> (defs::E<u32:1>, defs::E<u32:7>) {
  (low, high)
}
)",
                                       "main", &import_data));
  Function* function = result.tm.module->GetFunction("identity").value();
  XLS_ASSERT_OK_AND_ASSIGN(
      FunctionType * function_type,
      result.tm.type_info->GetItemAs<FunctionType>(function));
  const SumDef& definition = function_type->params()[0]->AsSum().nominal_type();
  EXPECT_EQ(&function_type->params()[1]->AsSum().nominal_type(), &definition);
  ASSERT_EQ(definition.variants().size(), 1);
  ASSERT_TRUE(definition.variants()[0]->discriminant().has_value());
  const Expr* source_discriminant = *definition.variants()[0]->discriminant();
  ASSERT_EQ(source_discriminant->kind(), AstNodeKind::kStatementBlock);
  // Both imported instantiations must evaluate their own discriminant. Match
  // the cloned block roots rather than the literal type arguments or tuple
  // items.
  std::vector<InterpValue> evaluated_discriminants;
  for (const auto& [node, type] : result.tm.type_info->dict()) {
    if (node->owner() == result.tm.module &&
        node->kind() == source_discriminant->kind() &&
        node->GetSpan() == source_discriminant->GetSpan() &&
        node->parent() == source_discriminant->parent()) {
      XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                               result.tm.type_info->GetConstExpr(node));
      evaluated_discriminants.push_back(value);
    }
  }
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(1)));
  EXPECT_THAT(evaluated_discriminants, Contains(InterpValue::MakeU32(7)));
  EXPECT_THAT(evaluated_discriminants,
              ::testing::Each(::testing::AnyOf(InterpValue::MakeU32(1),
                                               InterpValue::MakeU32(7))));
}

TEST(TypecheckV2Test, SemanticSumReferenceRequiresUnboundParametrics) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
fn f(x: Box) -> Box { x }
)",
      TypecheckFails(HasSubstr("must have all parametrics specified")));
}

TEST(TypecheckV2Test, SemanticSumUnifiesEquivalentTypeArgumentSpellings) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
fn f(x: Box<bits[8]>) -> Box<u8> { x }
)",
      TypecheckSucceeds(::testing::_));
}

TEST(TypecheckV2Test,
     SemanticSumSharedPayloadInferenceIgnoresConstructorOrder) {
  constexpr std::string_view kType = "E<u32:8> { None | Some(uN[8]) }";
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<N: u32> { None, Some(uN[N]) }
fn f(b: bool) {
  let first = if b { E::Some(u8:0) } else { E::None };
  let last = if b { E::None } else { E::Some(u8:0) };
  let array_first = [E::Some(u8:0), E::None];
  let array_last = [E::None, E::Some(u8:0)];
}
)",
      TypecheckSucceeds(AllOf(
          HasNodeWithType("first", kType), HasNodeWithType("last", kType),
          HasNodeWithType("array_first", "E<u32:8> { None | Some(uN[8]) }[2]"),
          HasNodeWithType("array_last",
                          "E<u32:8> { None | Some(uN[8]) }[2]"))));
}

TEST(TypecheckV2Test, SemanticSumSharedPayloadsJointlyInferParameters) {
  constexpr std::string_view kType =
      "E<u32:8, u32:16> { Tuple(uN[8]) | Named { value: uN[16] } }";
  EXPECT_THAT(R"(#![feature(generics)]
enum E<N: u32, M: u32> { Tuple(uN[N]), Named { value: uN[M] } }
fn f(b: bool) {
  let tuple_first = if b { E::Tuple(u8:0) } else { E::Named { value: u16:0 } };
  let named_first = if b { E::Named { value: u16:0 } } else { E::Tuple(u8:0) };
}
)",
              TypecheckSucceeds(AllOf(HasNodeWithType("tuple_first", kType),
                                      HasNodeWithType("named_first", kType))));
}

TEST(TypecheckV2Test, SemanticSumSharedPayloadConstraintsStillRejectConflicts) {
  for (std::string_view expression : {
           "if b { E::Some(u8:0) } else { E::Some(u16:0) }",
           "if b { E::Some(u16:0) } else { E::Some(u8:0) }",
           "if b { E::Some(u8:0) } else { if b { E::None } else { "
           "E::Some(u16:0) } }",
           "if b { if b { E::None } else { E::Some(u16:0) } } else { "
           "E::Some(u8:0) }",
       }) {
    SCOPED_TRACE(expression);
    EXPECT_THAT(absl::Substitute(R"(#![feature(generics)]
enum E<N: u32> { None, Some(uN[N]) }
fn f(b: bool) { let _ = $0; }
)",
                                 expression),
                TypecheckFails(::testing::AnyOf(
                    HasSubstr("size mismatch"),
                    HasSubstr("Value mismatch for parametric"))));
  }
  EXPECT_THAT(R"(#![feature(generics)]
enum E<N: u32> { None, Some(uN[N]) }
fn f(b: bool) { let _ = if b { E::None } else { E::None }; }
)",
              TypecheckFails(HasSubstr("must have all parametrics specified")));
}

TEST(TypecheckV2Test, SemanticSumPayloadsRequireValuesThroughExpressions) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub type Word = u32;
pub type Flag = bool;
pub const VALUE = u32:1;
pub const FLAG = true;
pub struct S { x: u32 }
)";
  // Each control changes only the type operand to a real value. These
  // expressions must not erase the distinction before payload validation.
  constexpr std::pair<std::string_view, std::string_view> kExpressions[] = {
      {"{ imported::Word }", "{ imported::VALUE }"},
      {"{ let x = imported::Word; x }", "{ let x = imported::VALUE; x }"},
      {"{ const X = imported::Word; X }", "{ const X = imported::VALUE; X }"},
      {"match true { _ => imported::Word }",
       "match true { _ => imported::VALUE }"},
      {"(imported::Word, u32:0).0", "(imported::VALUE, u32:0).0"},
      {"!imported::Word", "!imported::VALUE"},
      {"imported::Word + u32:1", "imported::VALUE + u32:1"},
      {"u32:1 + imported::Word", "u32:1 + imported::VALUE"},
      {"if imported::Flag { u32:1 } else { u32:0 }",
       "if imported::FLAG { u32:1 } else { u32:0 }"},
      {"match imported::Word { _ => u32:1 }",
       "match imported::VALUE { _ => u32:1 }"},
      {"for (_, x): (u32, u32) in u32:0..u32:1 { x }(imported::Word)",
       "for (_, x): (u32, u32) in u32:0..u32:1 { x }(imported::VALUE)"},
      {"imported::S.x", "(imported::S { x: u32:1 }).x"},
      {"(imported::S { x: imported::Word }).x",
       "(imported::S { x: imported::VALUE }).x"},
      {"(imported::S { x: imported::Word, ..imported::S { x: u32:0 } }).x",
       "(imported::S { x: imported::VALUE, ..imported::S { x: u32:0 } }).x"},
      {"(imported::S { ..imported::S }).x",
       "(imported::S { ..imported::S { x: u32:1 } }).x"},
  };
  for (const auto& [type_expression, value_expression] : kExpressions) {
    for (bool use_value : {false, true}) {
      std::string_view expression =
          use_value ? value_expression : type_expression;
      SCOPED_TRACE(expression);
      ImportData import_data = CreateImportDataForTest();
      XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
      const std::string program = absl::Substitute(R"(#![feature(generics)]
import imported;
enum E { V(u32) }
fn f() -> E { E::V($0) }
)",
                                                   expression);
      if (use_value) {
        EXPECT_THAT(TypecheckV2(program, "main", &import_data),
                    IsOkAndHolds(HasTypeInfo(
                        HasNodeWithType("f", "() -> E { V(uN[32]) }"))));
      } else {
        EXPECT_THAT(TypecheckV2(program, "main", &import_data),
                    StatusIs(absl::StatusCode::kInvalidArgument,
                             HasSubstr("Cannot use a type as a value.")));
      }
    }
  }
}

TEST(TypecheckV2Test, SemanticSumWrappedEnumPayloadsKeepTypeAndValueDistinct) {
  constexpr std::string_view kImported = "pub enum Tag: u8 { A = 0 }";
  for (std::string_view expression :
       {"E::Tuple({ { imported::Tag } })",
        "E::Named { value: { imported::Tag } }"}) {
    SCOPED_TRACE(expression);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
    EXPECT_THAT(TypecheckV2(absl::Substitute(R"(
import imported;
enum E { Tuple(imported::Tag), Named { value: imported::Tag } }
fn f() -> E { $0 }
)",
                                             expression),
                            "main", &import_data),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("Cannot use a type as a value.")));
  }
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(TypecheckV2(R"(#![feature(generics)]
import imported;
enum E { Tuple(imported::Tag), Named { value: imported::Tag } }
fn identity<T: type>(x: T) -> T { x }
fn tuple() -> E { E::Tuple({ { imported::Tag::A } }) }
fn named() -> E { E::Named { value: { imported::Tag::A } } }
fn argument() -> E { E::Tuple(identity<imported::Tag>(imported::Tag::A)) }
)",
                          "main", &import_data),
              IsOkAndHolds(::testing::_));
}

TEST(TypecheckV2Test, SemanticSumRejectsDifferentSameNamedTypeArguments) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<A: type> { Unit, Value(u8) }
fn f(condition: bool) {
  let _ = if condition {
    type T = u8;
    E<T>::Unit
  } else {
    type T = u16;
    E<T>::Unit
  };
}
)",
      TypecheckFails(HasSubstr("Value mismatch for parametric")));
}

TEST(TypecheckV2Test, ImportedSemanticSumResolvesDependentDefaults) {
  constexpr std::string_view kImported = R"(#![feature(generics)]
pub enum Box<N: u32 = {u32:8}, T: type = uN[N], V: T = {T:0}> { Value(T) }
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
fn f(x: imported::Box) -> imported::Box { x }
const DEFAULT = imported::Box::Value(u8:1);
const OVERRIDE = imported::Box<u32:16>::Value(u16:2);
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      IsOkAndHolds(HasTypeInfo(AllOf(
          HasNodeWithType("DEFAULT",
                          "Box<u32:8, uN[8], u8:0> { Value(uN[8]) }"),
          HasNodeWithType("OVERRIDE",
                          "Box<u32:16, uN[16], u16:0> { Value(uN[16]) }")))));
}

TEST(TypecheckV2Test, SemanticSumConstructorPreservesShadowedTypeAlias) {
  EXPECT_THAT(
      R"(
type T = u8;
enum E { V(u16) }
fn f() -> E {
  type T = u16;
  E::V(T:0)
}
)",
      TypecheckSucceeds(HasNodeWithType("E::V(T:0)", "E { V(uN[16]) }")));
}

TEST(TypecheckV2Test, SemanticSumNestedPayloadConstructorsCanonicalize) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
enum E { V(u32) }
fn f() -> E {
  E::V({ let inner = E::V(u32:0); u32:1 })
}
)"));
  int instances = 0;
  for (const AstNode* node : FlattenToSet(result.tm.module)) {
    if (node->kind() == AstNodeKind::kSumInstance) {
      ++instances;
      const auto* instance = absl::down_cast<const SumInstance*>(node);
      EXPECT_EQ(instance->constructor_ref()->parent(), instance);
      EXPECT_EQ(instance->tuple_payload_args().front()->parent(), instance);
    } else {
      EXPECT_NE(node->kind(), AstNodeKind::kInvocation);
    }
  }
  EXPECT_EQ(instances, 2);
}

TEST(TypecheckV2Test, ImportedSemanticSumShapesPreserveParens) {
  constexpr std::string_view kImported = R"(
pub enum E {
  Unit,
  Tuple(u32),
  Struct { value: u32 },
  EmptyTuple(),
  EmptyStruct {},
}
)";
  constexpr std::string_view kProgram = R"(
import imported;
const UNIT = (imported::E::Unit);
const TUPLE = (imported::E::Tuple(u32:1));
const STRUCT = (imported::E::Struct { value: u32:2 });
const EMPTY_TUPLE = (imported::E::EmptyTuple());
const EMPTY_STRUCT = (imported::E::EmptyStruct {});
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(kProgram, "main", &import_data));
  struct Expected {
    std::string_view name;
    SumInstance::PayloadShape shape;
    std::string_view text;
  };
  const Expected expected[] = {
      {"UNIT", SumInstance::PayloadShape::kUnit, "(imported::E::Unit)"},
      {"TUPLE", SumInstance::PayloadShape::kTuple,
       "(imported::E::Tuple(u32:1))"},
      {"STRUCT", SumInstance::PayloadShape::kStruct,
       "(imported::E::Struct { value: u32:2 })"},
      {"EMPTY_TUPLE", SumInstance::PayloadShape::kTuple,
       "(imported::E::EmptyTuple())"},
      {"EMPTY_STRUCT", SumInstance::PayloadShape::kStruct,
       "(imported::E::EmptyStruct {})"},
  };
  for (const auto& [name, shape, text] : expected) {
    XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * constant,
                             result.tm.module->GetConstantDef(name));
    ASSERT_EQ(constant->value()->kind(), AstNodeKind::kSumInstance);
    const auto* instance =
        absl::down_cast<const SumInstance*>(constant->value());
    EXPECT_EQ(instance->payload_shape(), shape);
    EXPECT_TRUE(instance->in_parens());
    EXPECT_EQ(instance->ToString(), text);
    EXPECT_EQ(instance->owner(), result.tm.module);
    EXPECT_EQ(instance->constructor_ref()->owner(), result.tm.module);
    EXPECT_EQ(instance->constructor_ref()->parent(), instance);
    ASSERT_TRUE(std::holds_alternative<ColonRef*>(
        instance->constructor_ref()->subject()));
    EXPECT_EQ(
        std::get<ColonRef*>(instance->constructor_ref()->subject())->ToString(),
        "imported::E");
  }
}

TEST(TypecheckV2Test, SemanticSumRejectsWrongZeroPayloadShape) {
  EXPECT_THAT(R"(
enum E { Unit, EmptyTuple(), EmptyStruct {} }
fn f() -> E { E::Unit() }
)",
              TypecheckFails(HasSubstr("is not callable here")));
  EXPECT_THAT(R"(
enum E { Unit, EmptyTuple(), EmptyStruct {} }
fn f() -> E { E::EmptyTuple {} }
)",
              TypecheckFails(HasSubstr("Attempted to instantiate non-struct")));
}

TEST(TypecheckV2Test, SemanticSumPrimitiveMemberPayloadTypeAndControl) {
  // This declaration-only case used to abort during annotation substitution;
  // replacing just its dependent width with u8 is the accepting control.
  for (std::string_view payload : {"uN[T::ZERO + u32:8]", "u8"}) {
    SCOPED_TRACE(payload);
    EXPECT_THAT(absl::Substitute(R"(#![feature(generics)]
enum E<T: type> { V($0) }
fn f(x: E<u32>) -> E<u32> { x }
)",
                                 payload),
                TypecheckSucceeds(
                    HasNodeWithType("f", "(@1=E<uN[32]> { V(uN[8]) }) -> @1")));
  }
}

TEST(TypecheckV2Test, SemanticSumPrimitiveMemberConstructorPayloads) {
  constexpr std::string_view k8 =
      "E<uN[32], u32:8> { Tuple(uN[8], uN[8]) | Named { width: uN[8], value: "
      "uN[8] } }";
  constexpr std::string_view k16 =
      "E<uN[32], u32:16> { Tuple(uN[16], uN[16]) | Named { width: uN[16], "
      "value: uN[16] } }";
  // The first payload supplies N in the partial cases. T is always explicit
  // or supplied by context, rather than inferred from its member's value.
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type, N: u32> {
  Tuple(uN[N], uN[T::ZERO + N]),
  Named { width: uN[N], value: uN[T::ZERO + N] },
}
const EXPLICIT_TUPLE = E<u32, u32:8>::Tuple(u8:0, u8:1);
const PARTIAL_TUPLE = E<u32>::Tuple(u8:0, u8:2);
const CONTEXTUAL_TUPLE: E<u32, u32:16> = E::Tuple(u16:0, u16:3);
const EXPLICIT_NAMED = E<u32, u32:8>::Named { width: u8:0, value: u8:1 };
const PARTIAL_NAMED = E<u32>::Named { width: u8:0, value: u8:2 };
const CONTEXTUAL_NAMED: E<u32, u32:16> = E::Named { width: u16:0, value: u16:3 };
)",
      TypecheckSucceeds(AllOf(HasNodeWithType("EXPLICIT_TUPLE", k8),
                              HasNodeWithType("PARTIAL_TUPLE", k8),
                              HasNodeWithType("CONTEXTUAL_TUPLE", k16),
                              HasNodeWithType("EXPLICIT_NAMED", k8),
                              HasNodeWithType("PARTIAL_NAMED", k8),
                              HasNodeWithType("CONTEXTUAL_NAMED", k16))));
}

TEST(TypecheckV2Test, ImportedSemanticSumPrimitiveMemberPayloadAliases) {
  constexpr std::string_view kImported = R"(#![feature(type_inference_v2)]
#![feature(generics)]
pub type Word = u32;
pub enum E<T: type> {
  Tuple(uN[T::ZERO + u32:8]),
  Named { value: uN[T::ZERO + u32:8] },
}
)";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import imported;
type Alias = imported::E<imported::Word>;
type OtherWord = uN[32];
type OtherAlias = imported::E<OtherWord>;
const TUPLE: Alias = Alias::Tuple(u8:1);
const NAMED: Alias = Alias::Named { value: u8:2 };
const EQUIVALENT: OtherAlias = Alias::Tuple(u8:3);
)";
  absl::flat_hash_map<std::filesystem::path, std::string> files = {
      {"/imported.x", std::string(kImported)},
  };
  ImportData import_data = CreateImportDataForTest(
      std::make_unique<FakeFilesystem>(std::move(files), "/"));
  constexpr std::string_view kType =
      "E<uN[32]> { Tuple(uN[8]) | Named { value: uN[8] } }";
  EXPECT_THAT(
      TypecheckV2(kProgram, "main", &import_data),
      IsOkAndHolds(HasTypeInfo(AllOf(HasNodeWithType("TUPLE", kType),
                                     HasNodeWithType("NAMED", kType),
                                     HasNodeWithType("EQUIVALENT", kType)))));
}

TEST(TypecheckV2Test, SemanticSumDimensionedPrimitiveMemberPayloads) {
  // MAX depends on the actual primitive width: uN[3] yields 7 and uN[4]
  // yields 15. Cast before addition so the arithmetic itself remains u32.
  EXPECT_THAT(R"(#![feature(generics)]
enum E<T: type> { V(uN[(T::MAX as u32) + u32:1]) }
fn f(x: E<uN[3]>) -> E<uN[3]> { x }
const EXPLICIT = E<uN[3]>::V(u8:0);
const CONTEXTUAL: E<uN[4]> = E::V(u16:0);
)",
              TypecheckSucceeds(AllOf(
                  HasNodeWithType("f", "(@1=E<uN[3]> { V(uN[8]) }) -> @1"),
                  HasNodeWithType("EXPLICIT", "E<uN[3]> { V(uN[8]) }"),
                  HasNodeWithType("CONTEXTUAL", "E<uN[4]> { V(uN[16]) }"))));
}

TEST(TypecheckV2Test, SemanticSumPrimitiveMemberKeepsCallerWidth) {
  EXPECT_THAT(
      R"(#![feature(generics)]
enum E<T: type> { V(uN[(T::MAX as u32) + u32:1]) }
fn make<N: u32>(x: uN[u32:1 << N]) -> E<uN[N]> { E<uN[N]>::V(x) }
fn narrow(x: u8) -> E<uN[3]> { make<u32:3>(x) }
fn wide(x: u16) -> E<uN[4]> { make<u32:4>(x) }
)",
      TypecheckSucceeds(AllOf(
          HasNodeWithType("narrow", "(uN[8]) -> E<uN[3]> { V(uN[8]) }"),
          HasNodeWithType("wide", "(uN[16]) -> E<uN[4]> { V(uN[16]) }"))));
}

TEST(TypecheckV2Test, SemanticSumPrimitiveMemberRejectsUnknownMember) {
  EXPECT_THAT(R"(#![feature(generics)]
enum E<T: type> { V(uN[T::MISSING + u32:8]) }
fn f(x: E<u32>) -> E<u32> { x }
)",
              TypecheckFails(HasSubstr("does not have attribute 'MISSING'")));
}

TEST(TypecheckV2Test, SemanticSumPrimitiveMemberRejectsNonBitsType) {
  EXPECT_THAT(R"(#![feature(generics)]
enum E<T: type> { V(uN[(T::ZERO as u32) + u32:8]) }
fn f(x: E<(u8, u8)>) -> E<(u8, u8)> { x }
)",
              TypecheckFails(HasSubstr("has no member `ZERO`")));
}

}  // namespace
}  // namespace xls::dslx
