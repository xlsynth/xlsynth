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

#include <string_view>

#include "absl/base/casts.h"
#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/typecheck_test_utils.h"
#include "xls/dslx/type_system_v2/matchers.h"
#include "xls/dslx/type_system_v2/type_system_test_utils.h"

namespace xls::dslx {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::testing::AllOf;
using ::testing::HasSubstr;

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

TEST(TypecheckV2Test, ImportedSemanticSumConstructorCanonicalizes) {
  constexpr std::string_view kImported = R"(
pub enum Option {
  None,
  Some(u32),
}
)";
  constexpr std::string_view kProgram = R"(
import imported;

const X = imported::Option::Some(u32:7);
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(TypecheckV2(kImported, "imported", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result,
                           TypecheckV2(kProgram, "main", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(ConstantDef * x,
                           result.tm.module->GetConstantDef("X"));
  EXPECT_EQ(x->value()->kind(), AstNodeKind::kSumInstance);
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

TEST(TypecheckV2Test, SemanticSumTuplePayloadAggregateRejectedInPhase1) {
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
      TypecheckFails(AllOf(
          HasSubstr("Phase 1 semantic sum payload members must be bits-like, "
                    "enum typed, or empty semantic sums"),
          HasSubstr("constructor `Some`"), HasSubstr("Point"))));
}

TEST(TypecheckV2Test, SemanticSumStructPayloadAggregateRejectedInPhase1) {
  EXPECT_THAT(
      R"(
enum PairBox {
  Pair { xy: (u32, u32) },
}

const X = PairBox::Pair { xy: (u32:1, u32:2) };
)",
      TypecheckFails(AllOf(
          HasSubstr("Phase 1 semantic sum payload members must be bits-like, "
                    "enum typed, or empty semantic sums"),
          HasSubstr("constructor `Pair`"), HasSubstr("(uN[32], uN[32])"))));
}

TEST(TypecheckV2Test, ImplicitSemanticSumRejectsTagTypeAnnotationInPhase1) {
  EXPECT_THAT(
      R"(
enum MaybeU32 : u3 {
  None,
  Some(u32),
}
)",
      TypecheckFails(HasSubstr(
          "Semantic sum `MaybeU32` with a tag type annotation requires "
          "explicit discriminants on every variant in Phase 1.")));
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
                           HasSubstr("uN[u32:8]"))));
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
                           HasSubstr("cannot be used as a value in Phase 1"))));
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
      TypecheckFails(HasSubstr("Match patterns are not exhaustive")));
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
      TypecheckSucceeds(
          AllOf(HasNodeWithType("v", "uN[32]"),
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

TEST(TypecheckV2Test, MatchWithSemanticSumConstructorsAlreadyExhaustive) {
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckResult result, TypecheckV2(R"(
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
)"));
  ASSERT_THAT(result.tm.warnings.warnings().size(), 1);
  EXPECT_EQ(result.tm.warnings.warnings()[0].message,
            "Match is already exhaustive before this pattern");
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
  EXPECT_THAT(
      R"(
enum Message : u3 {
  Request(u8) = 3,
  Idle() = 0,
}
const Y = zero!<Message>();
)",
      TypecheckSucceeds(
          HasNodeWithType("Y", "Message { Request(uN[8]) | Idle() }")));
}

TEST(TypecheckV2Test, ZeroMacroTupleContainingSemanticSumFailsInPhase1) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}
const Y = zero!<(Option,)>();
)",
      TypecheckFails(
          HasSubstr("aggregate type containing a semantic sum in Phase 1")));
}

TEST(TypecheckV2Test, ZeroMacroArrayContainingSemanticSumFailsInPhase1) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}
const Y = zero!<Option[1]>();
)",
      TypecheckFails(
          HasSubstr("aggregate type containing a semantic sum in Phase 1")));
}

TEST(TypecheckV2Test, ZeroMacroStructContainingSemanticSumFailsInPhase1) {
  EXPECT_THAT(
      R"(
enum Option {
  None,
  Some(u32),
}
struct Wrapper {
  value: Option,
}
const Y = zero!<Wrapper>();
)",
      TypecheckFails(
          HasSubstr("aggregate type containing a semantic sum in Phase 1")));
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

}  // namespace
}  // namespace xls::dslx
