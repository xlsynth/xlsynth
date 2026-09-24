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

#include "xls/dslx/type_system/type_info_to_proto.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/scoped_mock_log.h"
#include "absl/strings/str_format.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "re2/re2.h"
#include "xls/common/golden_files.h"
#include "xls/common/logging/scoped_vlog_level.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/type_info.pb.h"

namespace xls::dslx {
namespace {

constexpr int kLegacyNameDefTreeAstNodeKindProtoValue = 21;

std::string TestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

class TypeInfoToProtoWithBothTypecheckVersionsTest : public ::testing::Test {
 public:
  void DoRun(std::string_view program, TypeInfoProto* proto_out = nullptr,
             ImportData* import_data = nullptr) {
    std::optional<ImportData> local_import_data;
    if (import_data == nullptr) {
      local_import_data.emplace(CreateImportDataForTest());
      import_data = &local_import_data.value();
    }
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "fake.x", "fake", import_data, nullptr));

    XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                             TypeInfoToProto(*tm.type_info, tm.module));
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string nodes_text,
        ToHumanString(tip, *import_data, import_data->file_table()));

    std::string test_name(TestName());
    // Remove parametric test suite suffix.
    RE2::GlobalReplace(&test_name, R"(/\d+)", "");

    std::filesystem::path golden_file_path = absl::StrFormat(
        "xls/dslx/type_system/testdata/type_info_to_proto_test_%s.txt",
        test_name);
    ExpectEqualToGoldenFile(golden_file_path, nodes_text);

    if (proto_out != nullptr) {
      *proto_out = tip;
    }
  }
};

const AstNodeTypeInfoProto* FindSumTypeInfoNode(const TypeInfoProto& tip,
                                                std::string_view identifier,
                                                ImportData& import_data) {
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    const SumTypeProto& sum_type = node.type().sum_type();
    if (!sum_type.has_sum_def_span()) {
      continue;
    }
    auto sum_def = import_data.FindSumDef(
        FromProto(sum_type.sum_def_span(), import_data.file_table()));
    if (sum_def.ok() && (*sum_def)->identifier() == identifier) {
      return &node;
    }
  }
  return nullptr;
}

const AstNodeTypeInfoProto* FindParameterNode(const TypeInfoProto& tip) {
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    if (node.kind() == AST_NODE_KIND_PARAM) {
      return &node;
    }
  }
  return nullptr;
}

struct SerializedSumCounts {
  int64_t definitions = 0;
  int64_t references = 0;
  int64_t bits_types = 0;
};

void CountSerializedSums(const TypeProto& type, SerializedSumCounts& counts) {
  if (type.has_sum_type()) {
    ++counts.definitions;
    for (const SumTypeParametricProto& argument :
         type.sum_type().parametric_arguments()) {
      if (argument.has_type()) {
        CountSerializedSums(argument.type(), counts);
      }
    }
    for (const SumTypeVariantProto& variant : type.sum_type().variants()) {
      for (const TypeProto& member : variant.payload_members()) {
        CountSerializedSums(member, counts);
      }
    }
  } else if (type.has_sum_type_reference()) {
    ++counts.references;
  } else if (type.has_bits_type()) {
    ++counts.bits_types;
  } else if (type.has_tuple_type()) {
    for (const TypeProto& member : type.tuple_type().members()) {
      CountSerializedSums(member, counts);
    }
  } else if (type.has_array_type()) {
    CountSerializedSums(type.array_type().element_type(), counts);
  } else if (type.has_struct_type()) {
    for (const TypeProto& member : type.struct_type().members()) {
      CountSerializedSums(member, counts);
    }
  } else if (type.has_meta_type()) {
    CountSerializedSums(type.meta_type().wrapped(), counts);
  } else if (type.has_fn_type()) {
    for (const TypeProto& param : type.fn_type().params()) {
      CountSerializedSums(param, counts);
    }
    CountSerializedSums(type.fn_type().return_type(), counts);
  }
}

std::string NestedSumProgram(int64_t depth, bool binary, bool outer_sum) {
  std::string program = "#![feature(generics)]\nenum Wrap<T: type> { ";
  program += binary ? "Left(T), Right(T) }\n" : "Value(T) }\n";
  program += "type T0 = u8;\n";
  for (int64_t i = 1; i <= depth; ++i) {
    program += absl::StrFormat("type T%d = Wrap<T%d>;\n", i, i - 1);
  }
  if (outer_sum) {
    program += absl::StrFormat("enum Outer { Value(T%d) }\n", depth);
    program += "fn identity(x: Outer) -> Outer { x }\n";
  } else {
    program +=
        absl::StrFormat("fn identity(x: T%d) -> T%d { x }\n", depth, depth);
  }
  return program;
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, IdentityFunction) {
  std::string program = R"(fn id(x: u32) -> u32 { x })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ParametricIdentityFunction) {
  std::string program = R"(
fn pid<N: u32>(x: bits[N]) -> bits[N] { x }
fn id(x: u32) -> u32 { pid<u32:32>(x) }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, UnitFunction) {
  std::string program = R"(fn f() -> () { () })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, ArrayFunction) {
  std::string program = R"(fn f() -> u8[2] { u8[2]:[u8:1, u8:2] })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, TokenFunction) {
  std::string program = R"(fn f(x: token) -> token { x })";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       MakeStructInstanceFunction) {
  std::string program = R"(
struct S { x: u32 }
fn f() -> S { S { x: u32:42 } }
)";
  TypeInfoProto tip;
  DoRun(program, &tip);
  EXPECT_THAT(
      tip.ShortDebugString(),
      ::testing::ContainsRegex(
          R"(struct_def \{ span \{ .*? \} identifier: "S" member_names: "x" is_public: false \})"));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, MakeEnumFunction) {
  std::string program = R"(
enum E : u32 { A = 42 }
fn f() -> E { E::A }
)";
  ImportData import_data = CreateImportDataForTest();
  TypeInfoProto proto;
  DoRun(program, &proto, &import_data);

  int enum_index = -1;
  for (int i = 0; i < proto.nodes_size(); ++i) {
    const AstNodeTypeInfoProto& node = proto.nodes(i);
    if (node.type().has_enum_type()) {
      const EnumTypeProto& enum_type = node.type().enum_type();
      EXPECT_EQ(enum_type.members_size(), 0);
      enum_index = i;
    }
  }
  ASSERT_GE(enum_index, 0);

  XLS_ASSERT_OK(ToHumanString(proto, import_data, import_data.file_table()));

  TypeInfoProto populated = proto;
  InterpValueProto* member = populated.mutable_nodes(enum_index)
                                 ->mutable_type()
                                 ->mutable_enum_type()
                                 ->add_members();
  member->mutable_bits()->set_bit_count(32);
  member->mutable_bits()->set_is_signed(false);
  member->mutable_bits()->set_data(std::string("\0\0\0*", 4));
  XLS_ASSERT_OK(
      ToHumanString(populated, import_data, import_data.file_table()));

  TypeInfoProto extra_member = populated;
  EnumTypeProto* extra_enum = extra_member.mutable_nodes(enum_index)
                                  ->mutable_type()
                                  ->mutable_enum_type();
  *extra_enum->add_members() = extra_enum->members(0);
  EXPECT_THAT(
      ToHumanString(extra_member, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Enum member count mismatch")));

  TypeInfoProto wrong_signedness = populated;
  wrong_signedness.mutable_nodes(enum_index)
      ->mutable_type()
      ->mutable_enum_type()
      ->mutable_members(0)
      ->mutable_bits()
      ->set_is_signed(true);
  EXPECT_THAT(
      ToHumanString(wrong_signedness, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Enum member type mismatch")));

  TypeInfoProto wrong_value = populated;
  wrong_value.mutable_nodes(enum_index)
      ->mutable_type()
      ->mutable_enum_type()
      ->mutable_members(0)
      ->mutable_bits()
      ->set_data(std::string("\0\0\0+", 4));
  EXPECT_THAT(ToHumanString(wrong_value, import_data, import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("Enum member value mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, MakeSumFunction) {
  std::string program = R"(
enum Option {
  None,
  Some(u32),
}
fn f() -> Option { Option::None }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsReorderedSumVariantsInToHumanString) {
  std::string program = R"(
enum Option {
  None,
  Some(u32),
}
fn f() -> Option { Option::None }
)";

  ImportData import_data = CreateImportDataForTest();
  TypeInfoProto tip;
  DoRun(program, &tip, &import_data);

  int mutated_nodes = 0;
  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def_span()) {
      continue;
    }
    ASSERT_EQ(sum_type->variants_size(), 2);
    sum_type->mutable_variants()->SwapElements(0, 1);
    ++mutated_nodes;
  }
  ASSERT_GT(mutated_nodes, 0);

  EXPECT_THAT(ToHumanString(tip, import_data, import_data.file_table()),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumEmptyPayloadShapes) {
  std::string program = R"(
enum E {
  None,
  EmptyTuple(),
  EmptyStruct {},
  Some(u32),
  Point { x: u32 },
}

fn f(x: bool) -> E {
  if x { E::EmptyTuple() } else { E::EmptyStruct {} }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));

  const AstNodeTypeInfoProto* sum_node =
      FindSumTypeInfoNode(tip, "E", import_data);
  ASSERT_NE(sum_node, nullptr);
  const SumTypeProto& sum_type = sum_node->type().sum_type();
  ASSERT_TRUE(sum_type.has_sum_def_span());
  ASSERT_EQ(sum_type.variants_size(), 5);
  EXPECT_EQ(sum_type.variants(1).payload_members_size(), 0);
  EXPECT_EQ(sum_type.variants(2).payload_members_size(), 0);
  XLS_ASSERT_OK_AND_ASSIGN(
      const SumDef* sum_def,
      import_data.FindSumDef(
          FromProto(sum_type.sum_def_span(), import_data.file_table())));
  ASSERT_EQ(sum_def->variants().size(), 5);
  EXPECT_EQ(sum_def->variants().at(1)->identifier(), "EmptyTuple");
  EXPECT_TRUE(sum_def->variants().at(1)->is_tuple());
  EXPECT_EQ(sum_def->variants().at(2)->identifier(), "EmptyStruct");
  EXPECT_TRUE(sum_def->variants().at(2)->is_struct());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(*sum_node, import_data, import_data.file_table()));
  EXPECT_THAT(human, ::testing::EndsWith(
                         " :: E { None | EmptyTuple() | EmptyStruct {} | "
                         "Some(uN[32]) | Point { x: uN[32] } }"));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       FormatsAndValidatesSumsNestedInOtherTypes) {
  constexpr std::string_view kProgram = R"(
enum E { None, Some(u8) }
struct Box { item: E }
fn f(x: (E[2],)) -> Box { Box { item: E::None } }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* function_node = nullptr;
  const AstNodeTypeInfoProto* meta_node = nullptr;
  for (const AstNodeTypeInfoProto& node : proto.nodes()) {
    if (node.kind() == AST_NODE_KIND_FUNCTION) {
      function_node = &node;
    } else if (node.type().has_meta_type() &&
               node.type().meta_type().wrapped().has_sum_type()) {
      meta_node = &node;
    }
  }
  ASSERT_NE(function_node, nullptr);
  ASSERT_NE(meta_node, nullptr);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string function_text,
      ToHumanString(*function_node, import_data, import_data.file_table()));
  EXPECT_THAT(function_text,
              ::testing::EndsWith(" :: ((@1=E { None | Some(uN[8]) }[2])) -> "
                                  "Box { item: @1 }"));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string meta_text,
      ToHumanString(*meta_node, import_data, import_data.file_table()));
  EXPECT_THAT(meta_text,
              ::testing::EndsWith(" :: typeof(E { None | Some(uN[8]) })"));

  AstNodeTypeInfoProto missing_payload = *function_node;
  missing_payload.mutable_type()
      ->mutable_fn_type()
      ->mutable_params(0)
      ->mutable_tuple_type()
      ->mutable_members(0)
      ->mutable_array_type()
      ->mutable_element_type()
      ->mutable_sum_type()
      ->mutable_variants(1)
      ->clear_payload_members();
  EXPECT_THAT(
      ToHumanString(missing_payload, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant payload member count mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumPreservesConcreteTagLayout) {
  std::string program = R"(
enum Message: u3 {
  Idle = 0,
  Request(u8) = 3,
  Response(u32) = 7,
}

fn f(x: Message) -> Message { x }
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));

  const AstNodeTypeInfoProto* sum_node =
      FindSumTypeInfoNode(tip, "Message", import_data);
  ASSERT_NE(sum_node, nullptr);
  const SumTypeProto& sum_type = sum_node->type().sum_type();
  ASSERT_TRUE(sum_type.has_tag_bit_count());
  ASSERT_TRUE(sum_type.tag_bit_count().has_interp_value());
  EXPECT_EQ(sum_type.tag_bit_count().interp_value().bits().bit_count(), 32);
  EXPECT_EQ(sum_type.tag_bit_count().interp_value().bits().data(),
            std::string("\000\000\000\003", 4));
  ASSERT_EQ(sum_type.variants_size(), 3);
  ASSERT_TRUE(sum_type.variants(0).has_discriminant());
  EXPECT_EQ(sum_type.variants(0).discriminant().bits().bit_count(), 3);
  EXPECT_EQ(sum_type.variants(0).discriminant().bits().data(),
            std::string("\000", 1));
  ASSERT_TRUE(sum_type.variants(1).has_discriminant());
  EXPECT_EQ(sum_type.variants(1).discriminant().bits().bit_count(), 3);
  EXPECT_EQ(sum_type.variants(1).discriminant().bits().data(),
            std::string("\003", 1));
  ASSERT_TRUE(sum_type.variants(2).has_discriminant());
  EXPECT_EQ(sum_type.variants(2).discriminant().bits().bit_count(), 3);
  EXPECT_EQ(sum_type.variants(2).discriminant().bits().data(),
            std::string("\007", 1));
  XLS_EXPECT_OK(
      ToHumanString(*sum_node, import_data, import_data.file_table()));

  auto mutate_message_types = [&](TypeInfoProto& proto, auto mutation) {
    for (AstNodeTypeInfoProto& node : *proto.mutable_nodes()) {
      if (node.has_type() && node.type().has_sum_type()) {
        mutation(*node.mutable_type()->mutable_sum_type());
      }
    }
  };

  TypeInfoProto phase_one_layout = tip;
  mutate_message_types(phase_one_layout, [](SumTypeProto& sum) {
    sum.clear_tag_bit_count();
    for (SumTypeVariantProto& variant : *sum.mutable_variants()) {
      variant.clear_discriminant();
    }
  });
  XLS_EXPECT_OK(
      ToHumanString(phase_one_layout, import_data, import_data.file_table()));

  TypeInfoProto mixed_legacy_layout = tip;
  mutate_message_types(mixed_legacy_layout, [](SumTypeProto& sum) {
    sum.clear_tag_bit_count();
    sum.mutable_variants(0)->clear_discriminant();
  });
  EXPECT_THAT(
      ToHumanString(mixed_legacy_layout, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Missing sum tag bit count")));

  TypeInfoProto missing_discriminant = tip;
  mutate_message_types(missing_discriminant, [](SumTypeProto& sum) {
    sum.mutable_variants(1)->clear_discriminant();
  });
  EXPECT_THAT(
      ToHumanString(missing_discriminant, import_data,
                    import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("missing its bits-valued discriminant")));

  TypeInfoProto wrong_discriminant_width = tip;
  mutate_message_types(wrong_discriminant_width, [](SumTypeProto& sum) {
    sum.mutable_variants(1)
        ->mutable_discriminant()
        ->mutable_bits()
        ->set_bit_count(2);
  });
  EXPECT_THAT(ToHumanString(wrong_discriminant_width, import_data,
                            import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("discriminant width mismatch")));

  TypeInfoProto wrong_discriminant_signedness = tip;
  mutate_message_types(wrong_discriminant_signedness, [](SumTypeProto& sum) {
    sum.mutable_variants(1)
        ->mutable_discriminant()
        ->mutable_bits()
        ->set_is_signed(true);
  });
  EXPECT_THAT(ToHumanString(wrong_discriminant_signedness, import_data,
                            import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("discriminant signedness mismatch")));

  TypeInfoProto duplicate_discriminant = tip;
  mutate_message_types(duplicate_discriminant, [](SumTypeProto& sum) {
    *sum.mutable_variants(1)->mutable_discriminant() =
        sum.variants(0).discriminant();
  });
  EXPECT_THAT(
      ToHumanString(duplicate_discriminant, import_data,
                    import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("duplicate discriminant")));

  TypeInfoProto wrong_discriminant_value = tip;
  mutate_message_types(wrong_discriminant_value, [](SumTypeProto& sum) {
    sum.mutable_variants(1)->mutable_discriminant()->mutable_bits()->set_data(
        std::string("\006", 1));
  });
  EXPECT_THAT(ToHumanString(wrong_discriminant_value, import_data,
                            import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("discriminant value mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RoundTripsSignedSemanticSumDiscriminants) {
  constexpr std::string_view kProgram = R"(
enum SignedOption: s3 {
  Empty = 0,
  Negative(u8) = -1,
}
fn f(x: SignedOption) -> SignedOption { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* node =
      FindSumTypeInfoNode(proto, "SignedOption", import_data);
  ASSERT_NE(node, nullptr);

  const SumTypeProto& sum = node->type().sum_type();
  ASSERT_TRUE(sum.tag_bit_count().interp_value().has_bits());
  EXPECT_EQ(sum.tag_bit_count().interp_value().bits().bit_count(), 32);
  EXPECT_EQ(sum.tag_bit_count().interp_value().bits().data(),
            std::string("\0\0\0\x03", 4));
  ASSERT_EQ(sum.variants_size(), 2);
  ASSERT_TRUE(sum.variants(0).discriminant().has_bits());
  const BitsValueProto& zero = sum.variants(0).discriminant().bits();
  EXPECT_EQ(zero.bit_count(), 3);
  EXPECT_TRUE(zero.is_signed());
  EXPECT_EQ(zero.data(), std::string(1, '\0'));
  ASSERT_TRUE(sum.variants(1).discriminant().has_bits());
  const BitsValueProto& negative = sum.variants(1).discriminant().bits();
  EXPECT_EQ(negative.bit_count(), 3);
  EXPECT_TRUE(negative.is_signed());
  EXPECT_EQ(negative.data(), std::string(1, '\x07'));

  AstNodeTypeInfoProto from_wire;
  ASSERT_TRUE(from_wire.ParseFromString(node->SerializeAsString()));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(from_wire, import_data, import_data.file_table()));
  EXPECT_THAT(human, ::testing::EndsWith(
                         " :: SignedOption { Empty | Negative(uN[8]) }"));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumPreservesUnusedNominalParametrics) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Phantom<N: u32> {
  Only(),
}

enum PhantomType<T: type> {
  Only(),
}

fn first(value: Phantom<u32:1>) -> Phantom<u32:1> {
  value
}

fn second(value: Phantom<u32:2>) -> Phantom<u32:2> {
  value
}

fn typed(value: PhantomType<u8>) -> PhantomType<u8> {
  value
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));

  bool found_one = false;
  bool found_two = false;
  bool found_type = false;
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    const SumTypeProto& sum = node.type().sum_type();
    ASSERT_EQ(sum.parametric_arguments_size(), 1);
    const SumTypeParametricProto& argument = sum.parametric_arguments(0);
    if (argument.has_value()) {
      const BitsValueProto& value = argument.value().bits();
      ASSERT_EQ(value.bit_count(), 32);
      found_one |= value.data() == std::string("\000\000\000\001", 4);
      found_two |= value.data() == std::string("\000\000\000\002", 4);
    } else {
      ASSERT_TRUE(argument.has_type());
      ASSERT_TRUE(argument.type().has_bits_type());
      found_type = true;
    }
  }
  EXPECT_TRUE(found_one);
  EXPECT_TRUE(found_two);
  EXPECT_TRUE(found_type);
  XLS_EXPECT_OK(ToHumanString(tip, import_data, import_data.file_table()));

  const AstNodeTypeInfoProto* value_node =
      FindSumTypeInfoNode(tip, "Phantom", import_data);
  const AstNodeTypeInfoProto* type_node =
      FindSumTypeInfoNode(tip, "PhantomType", import_data);
  ASSERT_NE(value_node, nullptr);
  ASSERT_NE(type_node, nullptr);

  AstNodeTypeInfoProto missing_argument = *value_node;
  missing_argument.mutable_type()
      ->mutable_sum_type()
      ->clear_parametric_arguments();
  EXPECT_THAT(
      ToHumanString(missing_argument, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("parametric argument count mismatch")));

  AstNodeTypeInfoProto type_in_value_argument = *value_node;
  *type_in_value_argument.mutable_type()
       ->mutable_sum_type()
       ->mutable_parametric_arguments(0) =
      type_node->type().sum_type().parametric_arguments(0);
  EXPECT_THAT(ToHumanString(type_in_value_argument, import_data,
                            import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("argument 0 must contain a value")));

  AstNodeTypeInfoProto value_in_type_argument = *type_node;
  *value_in_type_argument.mutable_type()
       ->mutable_sum_type()
       ->mutable_parametric_arguments(0) =
      value_node->type().sum_type().parametric_arguments(0);
  EXPECT_THAT(ToHumanString(value_in_type_argument, import_data,
                            import_data.file_table()),
              absl_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  ::testing::HasSubstr("argument 0 must contain a type")));

  AstNodeTypeInfoProto wrong_width = *value_node;
  BitsValueProto* wrong_width_bits = wrong_width.mutable_type()
                                         ->mutable_sum_type()
                                         ->mutable_parametric_arguments(0)
                                         ->mutable_value()
                                         ->mutable_bits();
  wrong_width_bits->set_bit_count(16);
  wrong_width_bits->set_data(std::string("\000\001", 2));
  EXPECT_THAT(
      ToHumanString(wrong_width, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("argument 0 type mismatch")));

  AstNodeTypeInfoProto wrong_signedness = *value_node;
  wrong_signedness.mutable_type()
      ->mutable_sum_type()
      ->mutable_parametric_arguments(0)
      ->mutable_value()
      ->mutable_bits()
      ->set_is_signed(true);
  EXPECT_THAT(
      ToHumanString(wrong_signedness, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("argument 0 type mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumNominalValueParametricsKeepDeclaredTypes) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
type Count = u16;
enum Signed<N: s8> { Only() }
enum Aliased<N: Count> { Only() }
enum Flag<N: bool> { Only() }

fn signed_value(x: Signed<s8:-1>) -> Signed<s8:-1> { x }
fn aliased_value(x: Aliased<u16:3>) -> Aliased<u16:3> { x }
fn bool_value(x: Flag<true>) -> Flag<true> { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));
  XLS_EXPECT_OK(ToHumanString(tip, import_data, import_data.file_table()));
}

// Verifies: imported phantom sum arguments can be humanized.
// Catches: missing declaration types when only import consumers instantiate.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ImportedPhantomSumValueArgumentRoundTrips) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule lib,
                           ParseAndTypecheck(R"(#![feature(generics)]
pub enum Marker<N: u32> { Only() }
)",
                                             "lib.x", "lib", &import_data));
  (void)lib;
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(R"(#![feature(generics)]
import lib;
fn f(x: lib::Marker<u32:7>) -> lib::Marker<u32:7> { x }
)",
                        "consumer.x", "consumer", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* param = FindParameterNode(tip);
  ASSERT_NE(param, nullptr);
  ASSERT_TRUE(param->type().has_sum_type());
  EXPECT_EQ(param->type().sum_type().parametric_arguments_size(), 1);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string text,
      ToHumanString(tip, import_data, import_data.file_table()));
  EXPECT_THAT(text, ::testing::HasSubstr("Marker"));
}

// Verifies: imported enum, struct and sum nominal values round-trip.
// Catches: missing declaration types during packed argument decoding.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ImportedSumNonBitsArgumentsRoundTrip) {
  struct TestCase {
    std::string_view binding_type;
    std::string_view argument;
  };
  const TestCase kCases[] = {
      {"Flag", "lib::Flag::B"},
      {"Record", "lib::RECORD"},
      {"Choice", "lib::CHOICE"},
  };
  for (const auto& test_case : kCases) {
    SCOPED_TRACE(test_case.binding_type);
    const std::string imported = absl::StrFormat(R"(#![feature(generics)]
pub enum Flag: u2 { A = 0, B = 2 }
pub struct Record { flag: Flag, bytes: u8[2] }
pub enum Choice { None, Some(Record) }
pub const RECORD = Record { flag: Flag::B, bytes: u8[2]:[3, 7] };
pub const CHOICE = Choice::Some(RECORD);
pub enum Phantom<V: %s> { Only() }
)",
                                                 test_case.binding_type);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule lib,
        ParseAndTypecheck(imported, "lib.x", "lib", &import_data));
    (void)lib;
    const std::string program = absl::StrFormat(
        "#![feature(generics)]\nimport lib;\n"
        "fn accept(_x: lib::Phantom<%s>) -> u1 { u1:0 }\n",
        test_case.argument);
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "consumer.x", "consumer", &import_data));
    XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                             tm.module->GetMemberOrError<Function>("accept"));
    XLS_ASSERT_OK_AND_ASSIGN(Type * source_type, tm.type_info->GetItemOrError(
                                                     function->params()[0]));
    XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                             TypeInfoToProto(*tm.type_info, tm.module));
    TypeInfoProto parsed;
    ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
    const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
    ASSERT_NE(parameter, nullptr);
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string standalone,
        ToHumanString(*parameter, import_data, import_data.file_table()));
    EXPECT_THAT(standalone,
                ::testing::EndsWith(" :: " + source_type->ToString()));
    EXPECT_THAT(ToHumanString(parsed, import_data, import_data.file_table()),
                absl_testing::IsOkAndHolds(::testing::HasSubstr(standalone)));
  }
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumNominalNonBitsArgumentsRoundTrip) {
  constexpr std::string_view kTypes = R"(#![feature(generics)]
enum Flag: s3 { Neg = -1, Two = 2 }
struct Record { number: Flag, fields: (u3[2], (u2, u4[2])), empty: u8[0] }
const RECORD = Record {
  number: Flag::Neg,
  fields: (u3[2]:[1, 6], (u2:2, u4[2]:[3, 12])),
  empty: u8[0]:[],
};
enum Choice: u2 { None = 0, Some(Record) = 2 }
enum OuterChoice: u3 { None = 0, Value(Choice, u2[2]) = 5 }
enum Empty { Only() }
struct Zero { value: uN[0], empty: u8[0], unit: (), constructor: Empty }
const ZERO = Zero { value: uN[0]:0, empty: u8[0]:[], unit: (),
                    constructor: Empty::Only() };
)";
  struct TestCase {
    std::string_view binding_type;
    std::string_view argument;
    int64_t packed_bit_count;
    std::string packed_bytes;
  };
  // Independent packing oracle: tuple/struct members are MSB-first, while
  // [1, 6] packs as 6 ++ 1 and [3, 12] packs as 12 ++ 3. Signed Flag::Neg
  // contributes 111. Sum images prepend their declared tag to the payload.
  const TestCase kCases[] = {
      {"Flag", "Flag::Neg", 3, std::string("\x07", 1)},
      {"Record", "RECORD", 19, std::string("\x07\xc6\xc3", 3)},
      {"Choice", "{Choice::Some(RECORD)}", 21, std::string("\x17\xc6\xc3", 3)},
      {"OuterChoice",
       "{OuterChoice::Value(Choice::Some(RECORD), u2[2]:[1, 2])}", 28,
       std::string("\x0b\x7c\x6c\x39", 4)},
      {"Empty", "{Empty::Only()}", 0, ""},
      {"Zero", "ZERO", 0, ""},
  };
  for (const TestCase& test_case : kCases) {
    SCOPED_TRACE(test_case.binding_type);
    // An ordinary result keeps this at the admitted parameter-serialization
    // boundary, without re-instantiating the nominal argument in a return type.
    const std::string program = absl::StrFormat(
        "%s\nenum Phantom<V: %s> { Only() }\n"
        "fn accept(_x: Phantom<%s>) -> u1 { u1:0 }\n",
        kTypes, test_case.binding_type, test_case.argument);
    ImportData import_data = CreateImportDataForTest();
    XLS_ASSERT_OK_AND_ASSIGN(
        TypecheckedModule tm,
        ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
    XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                             tm.module->GetMemberOrError<Function>("accept"));
    XLS_ASSERT_OK_AND_ASSIGN(Type * source_type, tm.type_info->GetItemOrError(
                                                     function->params()[0]));
    XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                             TypeInfoToProto(*tm.type_info, tm.module));
    TypeInfoProto parsed;
    ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
    const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
    ASSERT_NE(parameter, nullptr);
    ASSERT_TRUE(parameter->type().has_sum_type());
    ASSERT_EQ(parameter->type().sum_type().parametric_arguments_size(), 1);
    const SumTypeParametricProto& argument =
        parameter->type().sum_type().parametric_arguments(0);
    ASSERT_TRUE(argument.has_packed_value());
    EXPECT_FALSE(argument.packed_value().is_signed());
    EXPECT_EQ(argument.packed_value().bit_count(), test_case.packed_bit_count);
    EXPECT_EQ(argument.packed_value().data(), test_case.packed_bytes);
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string standalone,
        ToHumanString(*parameter, import_data, import_data.file_table()));
    EXPECT_THAT(standalone,
                ::testing::EndsWith(" :: " + source_type->ToString()));
    EXPECT_THAT(ToHumanString(parsed, import_data, import_data.file_table()),
                absl_testing::IsOkAndHolds(::testing::HasSubstr(standalone)));
  }
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumNominalPackedArgumentsRejectMalformedValues) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Flag: s3 { Neg = -1, Two = 2 }
enum Inner: u2 { None = 0, Some(Flag) = 2 }
enum Outer: u3 { Empty = 0, Value(Inner) = 5 }
enum Phantom<V: Outer> { Only() }
fn identity(x: Phantom<{Outer::Value(Inner::Some(Flag::Neg))}>) -> u1 { u1:0 }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
  ASSERT_NE(parameter, nullptr);
  const BitsValueProto& packed =
      parameter->type().sum_type().parametric_arguments(0).packed_value();
  EXPECT_EQ(packed.bit_count(), 8);
  EXPECT_EQ(packed.data(), std::string("\xb7", 1));  // 101 ++ 10 ++ 111.
  XLS_ASSERT_OK(
      ToHumanString(*parameter, import_data, import_data.file_table()));
  auto expect_invalid = [&](const BitsValueProto& invalid,
                            std::string_view message) {
    AstNodeTypeInfoProto node = *parameter;
    *node.mutable_type()
         ->mutable_sum_type()
         ->mutable_parametric_arguments(0)
         ->mutable_packed_value() = invalid;
    EXPECT_THAT(
        ToHumanString(node, import_data, import_data.file_table()),
        absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                               ::testing::HasSubstr(std::string(message))));
  };
  BitsValueProto wrong_width = packed;
  wrong_width.set_bit_count(7);
  expect_invalid(wrong_width, "expected 8 bits; got 7");
  BitsValueProto wrong_signedness = packed;
  wrong_signedness.set_is_signed(true);
  expect_invalid(wrong_signedness, "requires an unsigned bit count");
  BitsValueProto missing_bytes = packed;
  missing_bytes.clear_data();
  expect_invalid(missing_bytes, "data does not match its bit count");
  BitsValueProto undeclared_nested_tag = packed;
  undeclared_nested_tag.set_data(std::string("\xaf", 1));  // 101 ++ 01 ++ 111.
  expect_invalid(undeclared_nested_tag, "No variant with tag bits");
  BitsValueProto undeclared_enum = packed;
  undeclared_enum.set_data(std::string("\xb3", 1));  // 101 ++ 10 ++ 011.
  expect_invalid(undeclared_enum, "declared member");
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsReorderedSumVariantsInProtoImport) {
  std::string program = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: bool) -> Option {
  if x { Option::None } else { Option::Some(u32:42) }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));

  const AstNodeTypeInfoProto* sum_node =
      FindSumTypeInfoNode(tip, "Option", import_data);
  ASSERT_NE(sum_node, nullptr);

  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def_span()) {
      continue;
    }
    sum_type->mutable_variants()->SwapElements(0, 1);
  }

  EXPECT_THAT(
      ToHumanString(*sum_node, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant payload member count mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumSchemaStoresOnlyConcreteTypeFacts) {
  EXPECT_EQ(SumTypeProto::descriptor()->field_count(), 5);
  EXPECT_EQ(SumTypeVariantProto::descriptor()->field_count(), 2);
  EXPECT_EQ(SumTypeProto::kSumDefSpanFieldNumber, 1);
  EXPECT_EQ(SumTypeProto::kVariantsFieldNumber, 2);
  EXPECT_EQ(SumTypeProto::kTagBitCountFieldNumber, 3);
  EXPECT_EQ(SumTypeProto::kParametricArgumentsFieldNumber, 4);
  EXPECT_EQ(SumTypeProto::kDefinitionIdFieldNumber, 5);
  EXPECT_EQ(SumTypeVariantProto::kPayloadMembersFieldNumber, 1);
  EXPECT_EQ(SumTypeVariantProto::kDiscriminantFieldNumber, 2);
  EXPECT_EQ(TypeProto::kSumTypeFieldNumber, 13);
  EXPECT_EQ(EnumTypeProto::kMembersFieldNumber, 4);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsSumLayoutFromExplicitTagMetadata) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("enum E { A, B(u8) } fn f() -> E { E::A }", "fake.x",
                        "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* node =
      FindSumTypeInfoNode(proto, "E", import_data);
  ASSERT_NE(node, nullptr);
  const SumTypeProto& old = node->type().sum_type();
  ASSERT_EQ(old.variants_size(), 2);
  XLS_ASSERT_OK(ToHumanString(*node, import_data, import_data.file_table()));

  AstNodeTypeInfoProto with_layout = *node;
  SumTypeProto* sum = with_layout.mutable_type()->mutable_sum_type();
  BitsValueProto* width =
      sum->mutable_tag_bit_count()->mutable_interp_value()->mutable_bits();
  width->set_bit_count(32);
  width->set_is_signed(false);
  width->set_data(std::string("\0\0\0\1", 4));
  for (int variant = 0; variant < 2; ++variant) {
    BitsValueProto* discriminant =
        sum->mutable_variants(variant)->mutable_discriminant()->mutable_bits();
    discriminant->set_bit_count(1);
    discriminant->set_is_signed(false);
    discriminant->set_data(std::string(1, static_cast<char>(variant)));
  }
  XLS_ASSERT_OK(
      ToHumanString(with_layout, import_data, import_data.file_table()));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsRepeatedLegacyExpandedSumsWithoutLayoutMetadata) {
  constexpr std::string_view kProgram = R"(
enum E { A, B(u8) }
fn f(pair: (E, E)) -> (E, E) { pair }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           tm.module->GetMemberOrError<Function>("f"));
  ASSERT_EQ(tm.module->GetSumDefs().size(), 1);

  AstNodeTypeInfoProto legacy;
  legacy.set_kind(AST_NODE_KIND_PARAM);
  *legacy.mutable_span() =
      ToProto(function->params().front()->span(), import_data.file_table());
  TupleTypeProto* tuple = legacy.mutable_type()->mutable_tuple_type();
  for (int i = 0; i < 2; ++i) {
    SumTypeProto* sum = tuple->add_members()->mutable_sum_type();
    *sum->mutable_sum_def_span() = ToProto(
        tm.module->GetSumDefs().front()->span(), import_data.file_table());
    sum->add_variants();
    BitsTypeProto* payload =
        sum->add_variants()->add_payload_members()->mutable_bits_type();
    payload->set_is_signed(false);
    BitsValueProto* payload_width =
        payload->mutable_dim()->mutable_interp_value()->mutable_bits();
    payload_width->set_bit_count(32);
    payload_width->set_is_signed(false);
    payload_width->set_data(std::string("\0\0\0\x08", 4));
  }

  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(legacy, import_data, import_data.file_table()));
  EXPECT_THAT(human, ::testing::EndsWith(
                         " :: (E { A | B(uN[8]) }, E { A | B(uN[8]) })"));
}

constexpr std::string_view kPackedNominalArgumentProgram =
    R"(#![feature(generics)]
struct Record { marker: u8, fields: u8[2], empty: u8[0] }
const RECORD = Record {
  marker: u8:0x5b, fields: u8[2]:[0x12, 0xa4], empty: u8[0]:[],
};
enum Phantom<V: Record> { Only() }
fn f(x: Phantom<RECORD>) -> Phantom<RECORD> { x }
)";

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsPackedNominalArgumentUsingItsDeclaredType) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kPackedNominalArgumentProgram,
                                             "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* node =
      FindSumTypeInfoNode(proto, "Phantom", import_data);
  ASSERT_NE(node, nullptr);

  AstNodeTypeInfoProto with_argument = *node;
  with_argument.mutable_type()
      ->mutable_sum_type()
      ->clear_parametric_arguments();
  BitsValueProto* packed = with_argument.mutable_type()
                               ->mutable_sum_type()
                               ->add_parametric_arguments()
                               ->mutable_packed_value();
  packed->set_bit_count(24);
  packed->set_is_signed(false);
  packed->set_data(std::string("\x5b\xa4\x12", 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(with_argument, import_data, import_data.file_table()));
  EXPECT_THAT(human, ::testing::EndsWith(" :: Phantom<(u8:91, [u8:18, "
                                         "u8:164], [])> { Only() }"));

  packed->set_is_signed(true);
  EXPECT_THAT(
      ToHumanString(with_argument, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("unsigned bit count")));
  packed->set_is_signed(false);
  packed->set_data(std::string("\xa4\x12", 2));
  EXPECT_THAT(
      ToHumanString(with_argument, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("data does not match")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       WritesPackedNominalArgumentFromSemanticSumType) {
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck(kPackedNominalArgumentProgram,
                                             "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(Function * function,
                           tm.module->GetMemberOrError<Function>("f"));
  const Param* param = function->params().front();
  XLS_ASSERT_OK_AND_ASSIGN(Type * parsed_type,
                           tm.type_info->GetItemOrError(param));
  ASSERT_TRUE(parsed_type->IsSum());
  const SumDef& sum_def = parsed_type->AsSum().nominal_type();

  XLS_ASSERT_OK_AND_ASSIGN(InterpValue fields,
                           InterpValue::MakeArray({InterpValue::MakeU8(0x12),
                                                   InterpValue::MakeU8(0xa4)}));
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue empty, InterpValue::MakeArray({}));
  std::vector<SumType::ParametricArgument> arguments;
  arguments.emplace_back(InterpValue::MakeTuple(
      {InterpValue::MakeU8(0x5b), std::move(fields), std::move(empty)}));
  std::vector<SumTypeVariant> variants;
  variants.push_back(
      SumTypeVariant::MakeTuple(*sum_def.variants().front(), {}));
  SumType semantic_sum(sum_def, std::move(variants), std::nullopt, {},
                       std::move(arguments));
  tm.type_info->SetItem(param, semantic_sum);
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const SumTypeProto* written = nullptr;
  for (const AstNodeTypeInfoProto& node : proto.nodes()) {
    if (node.kind() == AST_NODE_KIND_PARAM && node.type().has_sum_type()) {
      written = &node.type().sum_type();
      break;
    }
  }
  ASSERT_NE(written, nullptr);
  ASSERT_EQ(written->parametric_arguments_size(), 1);
  ASSERT_TRUE(written->parametric_arguments(0).has_packed_value());
  const BitsValueProto& packed =
      written->parametric_arguments(0).packed_value();
  EXPECT_EQ(packed.bit_count(), 24);
  EXPECT_FALSE(packed.is_signed());
  EXPECT_EQ(packed.data(), std::string("\x5b\xa4\x12", 3));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsDuplicateAndMissingSumVariantsInProtoImport) {
  std::string program = R"(
enum Option {
  None,
  Some(u32),
}

fn f(x: bool) -> Option {
  if x { Option::None } else { Option::Some(u32:42) }
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));

  const AstNodeTypeInfoProto* sum_node =
      FindSumTypeInfoNode(tip, "Option", import_data);
  ASSERT_NE(sum_node, nullptr);

  for (AstNodeTypeInfoProto& node : *tip.mutable_nodes()) {
    if (!node.has_type() || !node.type().has_sum_type()) {
      continue;
    }
    SumTypeProto* sum_type = node.mutable_type()->mutable_sum_type();
    if (!sum_type->has_sum_def_span()) {
      continue;
    }
    *sum_type->mutable_variants(1) = sum_type->variants(0);
  }

  EXPECT_THAT(
      ToHumanString(*sum_node, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant payload member count mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SemanticSumSchemaKeepsLegacyFieldNumbers) {
  EXPECT_EQ(SumTypeProto::descriptor()->field_count(), 5);
  EXPECT_EQ(SumTypeVariantProto::descriptor()->field_count(), 2);
  EXPECT_EQ(SumTypeParametricProto::descriptor()->field_count(), 3);
  EXPECT_EQ(SumTypeProto::kSumDefSpanFieldNumber, 1);
  EXPECT_EQ(SumTypeProto::kVariantsFieldNumber, 2);
  EXPECT_EQ(SumTypeProto::kTagBitCountFieldNumber, 3);
  EXPECT_EQ(SumTypeProto::kParametricArgumentsFieldNumber, 4);
  EXPECT_EQ(SumTypeProto::kDefinitionIdFieldNumber, 5);
  EXPECT_EQ(SumTypeVariantProto::kPayloadMembersFieldNumber, 1);
  EXPECT_EQ(SumTypeVariantProto::kDiscriminantFieldNumber, 2);
  EXPECT_EQ(SumTypeParametricProto::kValueFieldNumber, 1);
  EXPECT_EQ(SumTypeParametricProto::kTypeFieldNumber, 2);
  EXPECT_EQ(SumTypeParametricProto::kPackedValueFieldNumber, 3);
  EXPECT_EQ(TypeProto::kSumTypeFieldNumber, 13);
  EXPECT_EQ(TypeProto::kSumTypeReferenceFieldNumber, 14);
  EXPECT_EQ(EnumTypeProto::kMembersFieldNumber, 4);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       PhantomTypeArgumentsStillSerializeCompleteStructMembers) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
struct Pair<T: type> { a: T, b: T }
enum Marker<T: type> { Only(u1) }
fn identity(value: Marker<Pair<Pair<u8>>>) -> Marker<Pair<Pair<u8>>> { value }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(proto);
  ASSERT_NE(parameter, nullptr);
  ASSERT_TRUE(parameter->type().has_sum_type());
  ASSERT_EQ(parameter->type().sum_type().parametric_arguments_size(), 1);
  const TypeProto& argument =
      parameter->type().sum_type().parametric_arguments(0).type();
  ASSERT_TRUE(argument.has_struct_type());
  ASSERT_EQ(argument.struct_type().members_size(), 2);
  for (const TypeProto& inner : argument.struct_type().members()) {
    ASSERT_TRUE(inner.has_struct_type());
    ASSERT_EQ(inner.struct_type().members_size(), 2);
    for (const TypeProto& leaf : inner.struct_type().members()) {
      EXPECT_TRUE(leaf.has_bits_type());
    }
  }
  XLS_ASSERT_OK(
      ToHumanString(*parameter, import_data, import_data.file_table()));

  AstNodeTypeInfoProto truncated = *parameter;
  truncated.mutable_type()
      ->mutable_sum_type()
      ->mutable_parametric_arguments(0)
      ->mutable_type()
      ->mutable_struct_type()
      ->mutable_members(0)
      ->mutable_struct_type()
      ->mutable_members()
      ->RemoveLast();
  EXPECT_FALSE(
      ToHumanString(truncated, import_data, import_data.file_table()).ok());
}

// These are legal source types with 8 (unary) or 8+depth (binary) packed bits.
// The non-generic outer case also invokes source-backed payload comparisons in
// the reader, which must preserve sharing as well as decoding each ID once.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SharedSumDescriptionsHaveBoundedBinaryAndHumanizedOutput) {
  for (bool binary : {false, true}) {
    for (bool outer_sum : {false, true}) {
      for (int64_t depth : {4, 8}) {
        SCOPED_TRACE(absl::StrFormat("binary=%d outer=%d depth=%d", binary,
                                     outer_sum, depth));
        ImportData import_data = CreateImportDataForTest();
        XLS_ASSERT_OK_AND_ASSIGN(
            TypecheckedModule tm,
            ParseAndTypecheck(NestedSumProgram(depth, binary, outer_sum),
                              "fake.x", "fake", &import_data, nullptr));
        XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                                 TypeInfoToProto(*tm.type_info, tm.module));
        const std::string wire = proto.SerializeAsString();
        // Per-node scopes may repeat a description in different AST records;
        // they must not expand the shared graph within any one record.
        EXPECT_LT(wire.size(), 20000 * depth);
        TypeInfoProto parsed;
        ASSERT_TRUE(parsed.ParseFromString(wire));
        const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
        ASSERT_NE(parameter, nullptr);
        EXPECT_LT(parameter->type().ByteSizeLong(), 256 * (depth + 1));
        SerializedSumCounts counts;
        CountSerializedSums(parameter->type(), counts);
        EXPECT_EQ(counts.definitions, depth + (outer_sum ? 1 : 0));
        EXPECT_EQ(counts.references, (binary ? 2 : 1) * (depth - 1));
        EXPECT_EQ(counts.bits_types, binary ? 3 : 2);

        std::optional<Function*> function = tm.module->GetFunction("identity");
        ASSERT_TRUE(function.has_value());
        XLS_ASSERT_OK_AND_ASSIGN(Type * type, tm.type_info->GetItemOrError(
                                                  (*function)->params()[0]));
        EXPECT_THAT(type->GetTotalBitCount(),
                    absl_testing::IsOkAndHolds(
                        TypeDim::CreateU32(8 + (binary ? depth : 0))));
        const std::string type_text = type->ToString();
        EXPECT_LT(type_text.size(), 128 * (depth + 1));
        std::string standalone;
        int64_t source_leaf_comparisons = 0;
        {
          // Use the existing per-comparison trace to count reader work without
          // timing thresholds or a production instrumentation API. Capturing
          // only readback excludes typechecking and serialization comparisons.
          ScopedSetVlogLevel vlog_level("type", 10);
          absl::ScopedMockLog comparison_log;
          EXPECT_CALL(comparison_log,
                      Log(::testing::_, ::testing::_, ::testing::_))
              .Times(::testing::AnyNumber());
          EXPECT_CALL(comparison_log,
                      Log(::testing::_, ::testing::_,
                          ::testing::StartsWith("BitsType::operator==;")))
              .Times(::testing::AnyNumber())
              .WillRepeatedly(::testing::InvokeWithoutArgs(
                  [&source_leaf_comparisons]() { ++source_leaf_comparisons; }));
          comparison_log.StartCapturingLogs();
          XLS_ASSERT_OK_AND_ASSIGN(
              standalone,
              ToHumanString(*parameter, import_data, import_data.file_table()));
        }
        // Outer validates the generic child against its source type. With
        // sharing, only the innermost argument and one/two payloads reach u8.
        // Without sum-pair reuse, these counts become 2^depth / 3^depth even
        // though the humanized output is unchanged. Positive counts also make
        // a missing or disabled observation point fail visibly.
        EXPECT_EQ(source_leaf_comparisons, outer_sum ? (binary ? 3 : 2) : 0);
        EXPECT_THAT(standalone, ::testing::EndsWith(" :: " + type_text));
        XLS_ASSERT_OK_AND_ASSIGN(
            std::string whole,
            ToHumanString(parsed, import_data, import_data.file_table()));
        EXPECT_THAT(whole, ::testing::HasSubstr(standalone));
        XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto repeated,
                                 TypeInfoToProto(*tm.type_info, tm.module));
        EXPECT_EQ(repeated.SerializeAsString(), wire);
      }
    }
  }
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SumReferencesPreservePhantomValueAndTypeArguments) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Phantom<N: u32> { Only() }
enum PhantomType<T: type> { Only() }
type Inputs = (Phantom<u32:1>, Phantom<u32:2>, Phantom<u32:1>,
               PhantomType<u8>, PhantomType<u16>, PhantomType<u8>);
fn identity(x: Inputs) -> Inputs { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
  ASSERT_NE(parameter, nullptr);
  const TupleTypeProto& tuple = parameter->type().tuple_type();
  ASSERT_EQ(tuple.members_size(), 6);
  ASSERT_TRUE(tuple.members(0).has_sum_type());
  ASSERT_TRUE(tuple.members(1).has_sum_type());
  EXPECT_NE(tuple.members(0).sum_type().definition_id(),
            tuple.members(1).sum_type().definition_id());
  EXPECT_EQ(tuple.members(2).sum_type_reference(),
            tuple.members(0).sum_type().definition_id());
  EXPECT_EQ(tuple.members(5).sum_type_reference(),
            tuple.members(3).sum_type().definition_id());
  EXPECT_NE(tuple.members(3).sum_type().definition_id(),
            tuple.members(4).sum_type().definition_id());
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(*parameter, import_data, import_data.file_table()));
  EXPECT_THAT(human, ::testing::HasSubstr("@1=Phantom<u32:1>"));
  EXPECT_THAT(human, ::testing::HasSubstr("Phantom<u32:2>"));
  EXPECT_THAT(human, ::testing::HasSubstr("@2=PhantomType<uN[8]>"));
  EXPECT_THAT(human, ::testing::HasSubstr("PhantomType<uN[16]>"));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsInvalidSumReferencesWithinEachNodeScope) {
  constexpr std::string_view kProgram = R"(
enum S { Value(u8) }
fn identity(x: (S, S)) -> (S, S) { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(proto);
  ASSERT_NE(parameter, nullptr);
  ASSERT_EQ(parameter->type().tuple_type().members_size(), 2);
  ASSERT_TRUE(parameter->type().tuple_type().members(0).has_sum_type());
  ASSERT_TRUE(
      parameter->type().tuple_type().members(1).has_sum_type_reference());
  const uint64_t id =
      parameter->type().tuple_type().members(0).sum_type().definition_id();
  EXPECT_EQ(id, 1);
  auto expect_invalid = [&](const AstNodeTypeInfoProto& node,
                            std::string_view message) {
    EXPECT_THAT(
        ToHumanString(node, import_data, import_data.file_table()),
        absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                               ::testing::HasSubstr(std::string(message))));
  };

  AstNodeTypeInfoProto unknown = *parameter;
  unknown.mutable_type()
      ->mutable_tuple_type()
      ->mutable_members(1)
      ->set_sum_type_reference(id + 1);
  expect_invalid(unknown, "Unresolved sum reference");
  AstNodeTypeInfoProto zero = *parameter;
  zero.mutable_type()
      ->mutable_tuple_type()
      ->mutable_members(0)
      ->mutable_sum_type()
      ->set_definition_id(0);
  expect_invalid(zero, "Sum definition ID must be positive");
  AstNodeTypeInfoProto zero_reference = *parameter;
  zero_reference.mutable_type()
      ->mutable_tuple_type()
      ->mutable_members(1)
      ->set_sum_type_reference(0);
  expect_invalid(zero_reference, "Unresolved sum reference");
  AstNodeTypeInfoProto duplicate = *parameter;
  *duplicate.mutable_type()->mutable_tuple_type()->mutable_members(1) =
      parameter->type().tuple_type().members(0);
  expect_invalid(duplicate, "Duplicate sum definition ID");
  AstNodeTypeInfoProto forward = *parameter;
  forward.mutable_type()->mutable_tuple_type()->mutable_members()->SwapElements(
      0, 1);
  expect_invalid(forward, "Unresolved sum reference");
  AstNodeTypeInfoProto cycle = *parameter;
  cycle.mutable_type()
      ->mutable_tuple_type()
      ->mutable_members(0)
      ->mutable_sum_type()
      ->mutable_variants(0)
      ->mutable_payload_members(0)
      ->set_sum_type_reference(id);
  expect_invalid(cycle, "incomplete definition (cycle)");
  AstNodeTypeInfoProto cross_node = *parameter;
  *cross_node.mutable_type() = parameter->type().tuple_type().members(1);
  expect_invalid(cross_node, "Unresolved sum reference");
  TypeInfoProto two_nodes;
  *two_nodes.add_nodes() = *parameter;
  *two_nodes.add_nodes() = cross_node;
  EXPECT_THAT(
      ToHumanString(two_nodes, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("Unresolved sum reference")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SharedReferenceStillChecksConcretePayloadAgainstSource) {
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Box<T: type> { Value(T) }
enum Outer { Small(Box<u8>), Large(Box<u16>) }
fn identity(x: Outer) -> Outer { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(proto);
  ASSERT_NE(parameter, nullptr);
  XLS_EXPECT_OK(
      ToHumanString(*parameter, import_data, import_data.file_table()));
  AstNodeTypeInfoProto wrong = *parameter;
  SumTypeProto* outer = wrong.mutable_type()->mutable_sum_type();
  ASSERT_EQ(outer->variants_size(), 2);
  const uint64_t small_id =
      outer->variants(0).payload_members(0).sum_type().definition_id();
  outer->mutable_variants(1)
      ->mutable_payload_members(0)
      ->set_sum_type_reference(small_id);
  EXPECT_THAT(
      ToHumanString(wrong, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("payload type mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SharedReferencesPreserveNominalDeclarationsAndPhantomArguments) {
  constexpr std::string_view kDeclarations = R"(#![feature(generics)]
pub enum Never {}
pub enum Phantom<N: u32> { Only() }
pub enum PhantomType<T: type> { Only() }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK(
      ParseAndTypecheck(kDeclarations, "left.x", "left", &import_data));
  XLS_ASSERT_OK(
      ParseAndTypecheck(kDeclarations, "right.x", "right", &import_data));
  constexpr std::string_view kProgram = R"(#![feature(generics)]
import left;
import right;
enum Outer {
  Left(left::Never), Right(right::Never),
  Small(left::Phantom<u32:1>), Large(left::Phantom<u32:2>),
  Narrow(left::PhantomType<u8>), Wide(left::PhantomType<u16>),
}
fn identity(x: Outer) -> Outer { x }
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(proto);
  ASSERT_NE(parameter, nullptr);
  XLS_EXPECT_OK(
      ToHumanString(*parameter, import_data, import_data.file_table()));
  ASSERT_EQ(parameter->type().sum_type().variants_size(), 6);
  // Each replacement has the same payload shape as its source counterpart.
  // Only the declaration, phantom value, or phantom type distinguishes them.
  for (int64_t source_variant : {0, 2, 4}) {
    SCOPED_TRACE(source_variant);
    AstNodeTypeInfoProto wrong = *parameter;
    SumTypeProto* outer = wrong.mutable_type()->mutable_sum_type();
    const TypeProto& source =
        outer->variants(source_variant).payload_members(0);
    ASSERT_TRUE(source.has_sum_type());
    ASSERT_TRUE(source.sum_type().has_definition_id());
    const uint64_t id = source.sum_type().definition_id();
    outer->mutable_variants(source_variant + 1)
        ->mutable_payload_members(0)
        ->set_sum_type_reference(id);
    EXPECT_THAT(
        ToHumanString(wrong, import_data, import_data.file_table()),
        absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                               ::testing::HasSubstr("payload type mismatch")));
  }
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RoundTripsAggregateSumPayloadsAgainstSource) {
  constexpr std::string_view kProgram = R"(
struct Record { value: u8 }
enum Aggregate {
  Tuple((u8, u16)),
  Array(u8[2]),
  Struct(Record),
}
fn identity(x: Aggregate) -> Aggregate { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(proto.SerializeAsString()));
  const AstNodeTypeInfoProto* parameter = FindParameterNode(parsed);
  ASSERT_NE(parameter, nullptr);
  const SumTypeProto& sum = parameter->type().sum_type();
  ASSERT_EQ(sum.variants_size(), 3);
  for (const SumTypeVariantProto& variant : sum.variants()) {
    ASSERT_EQ(variant.payload_members_size(), 1);
  }
  ASSERT_EQ(sum.variants(0).payload_members(0).tuple_type().members_size(), 2);
  ASSERT_TRUE(sum.variants(1).payload_members(0).has_array_type());
  ASSERT_EQ(sum.variants(2).payload_members(0).struct_type().members_size(), 1);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string standalone,
      ToHumanString(*parameter, import_data, import_data.file_table()));
  EXPECT_THAT(standalone,
              ::testing::EndsWith(
                  " :: Aggregate { Tuple((uN[8], uN[16])) | Array(uN[8][2]) | "
                  "Struct(Record { value: uN[8] }) }"));
  EXPECT_THAT(ToHumanString(parsed, import_data, import_data.file_table()),
              absl_testing::IsOkAndHolds(::testing::HasSubstr(standalone)));

  auto expect_payload_mismatch = [&](const AstNodeTypeInfoProto& node) {
    EXPECT_THAT(
        ToHumanString(node, import_data, import_data.file_table()),
        absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                               ::testing::HasSubstr("payload type mismatch")));
  };
  // Keep each aggregate's kind and shape, but change a nested u8 to u16 so
  // readback must compare its members against the source declaration.
  const TypeProto& wide_member =
      sum.variants(0).payload_members(0).tuple_type().members(1);
  AstNodeTypeInfoProto wrong_tuple = *parameter;
  *wrong_tuple.mutable_type()
       ->mutable_sum_type()
       ->mutable_variants(0)
       ->mutable_payload_members(0)
       ->mutable_tuple_type()
       ->mutable_members(0) = wide_member;
  expect_payload_mismatch(wrong_tuple);

  AstNodeTypeInfoProto wrong_array = *parameter;
  *wrong_array.mutable_type()
       ->mutable_sum_type()
       ->mutable_variants(1)
       ->mutable_payload_members(0)
       ->mutable_array_type()
       ->mutable_element_type() = wide_member;
  expect_payload_mismatch(wrong_array);

  AstNodeTypeInfoProto wrong_struct = *parameter;
  *wrong_struct.mutable_type()
       ->mutable_sum_type()
       ->mutable_variants(2)
       ->mutable_payload_members(0)
       ->mutable_struct_type()
       ->mutable_members(0) = wide_member;
  expect_payload_mismatch(wrong_struct);
}

// Fixed records using only the Phase One schema fields, not output from the
// current writer with fields removed. The source has no concrete root SumType
// for either declaration; the old records never stored their nominal arguments.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsFixedPhaseOneParametricRecordsWithoutRootInstance) {
  constexpr char kPhaseOne[] =
      "\x0a\x5c\x08\x03\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x03\x18\x0a\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x03"
      "\x18\x0b\x1a\x3a\x6a\x38\x0a\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65"
      "\x2e\x78\x10\x01\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x01\x18\x29\x12\x00\x12\x16\x0a\x14\x0a\x12\x08\x00\x12\x0e"
      "\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08\x0a\x44"
      "\x08\x03\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x04"
      "\x18\x0b\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x04\x18\x0c"
      "\x1a\x22\x6a\x20\x0a\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x02\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x02"
      "\x18\x1f\x12\x00";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Option<N: u32> { None, Some(uN[N]) }
enum Phantom<N: u32> { Only() }
fn option(x: Option<u32:8>) -> Option<u32:8> { x }
fn phantom(x: Phantom<u32:9>) -> Phantom<u32:9> { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  for (std::string_view name : {"Option", "Phantom"}) {
    XLS_ASSERT_OK_AND_ASSIGN(SumDef * sum_def,
                             tm.module->GetMemberOrError<SumDef>(name));
    EXPECT_FALSE(tm.type_info->GetItem(sum_def).has_value());
  }
  TypeInfoProto parsed;
  ASSERT_TRUE(
      parsed.ParseFromString(std::string(kPhaseOne, sizeof(kPhaseOne) - 1)));
  ASSERT_EQ(parsed.nodes_size(), 2);
  for (const AstNodeTypeInfoProto& node : parsed.nodes()) {
    const SumTypeProto& sum = node.type().sum_type();
    EXPECT_FALSE(sum.has_tag_bit_count());
    EXPECT_FALSE(sum.has_definition_id());
    EXPECT_EQ(sum.parametric_arguments_size(), 0);
    for (const SumTypeVariantProto& variant : sum.variants()) {
      EXPECT_FALSE(variant.has_discriminant());
    }
  }
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string option,
      ToHumanString(parsed.nodes(0), import_data, import_data.file_table()));
  EXPECT_THAT(option, ::testing::EndsWith(" :: Option { None | Some(uN[8]) }"));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string phantom,
      ToHumanString(parsed.nodes(1), import_data, import_data.file_table()));
  // In particular, the omitted phantom value 9 must not be guessed from source.
  EXPECT_THAT(phantom, ::testing::EndsWith(" :: Phantom { Only() }"));
  EXPECT_THAT(
      ToHumanString(parsed, import_data, import_data.file_table()),
      absl_testing::IsOkAndHolds(::testing::AllOf(
          ::testing::HasSubstr(option), ::testing::HasSubstr(phantom))));
}

// Enclosing source validation must distinguish omitted old metadata from a
// contradictory modern description, including the old one-bit singleton tag.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsFixedPhaseOneParametricRecordsWhenNested) {
  constexpr char kPhaseOne[] =
      "\x0a\x5c\x08\x03\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x03\x18\x0a\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x03"
      "\x18\x0b\x1a\x3a\x6a\x38\x0a\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65"
      "\x2e\x78\x10\x01\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x01\x18\x29\x12\x00\x12\x16\x0a\x14\x0a\x12\x08\x00\x12\x0e"
      "\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08\x0a\x44"
      "\x08\x03\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x04"
      "\x18\x0b\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x04\x18\x0c"
      "\x1a\x22\x6a\x20\x0a\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x02\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x02"
      "\x18\x1f\x12\x00";
  constexpr std::string_view kProgram = R"(#![feature(generics)]
enum Option<N: u32> { None, Some(uN[N]) }
enum Phantom<N: u32> { Only() }
enum Outer { Pair(Option<u32:8>, Phantom<u32:9>) }
fn f(x: Outer) -> Outer { x }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto modern,
                           TypeInfoToProto(*tm.type_info, tm.module));
  XLS_ASSERT_OK(ToHumanString(modern, import_data, import_data.file_table()));
  const AstNodeTypeInfoProto* outer =
      FindSumTypeInfoNode(modern, "Outer", import_data);
  ASSERT_NE(outer, nullptr);
  const SumTypeVariantProto& pair = outer->type().sum_type().variants(0);
  ASSERT_EQ(pair.payload_members_size(), 2);
  ASSERT_TRUE(pair.payload_members(0).has_sum_type());
  ASSERT_TRUE(pair.payload_members(1).has_sum_type());
  TypeInfoProto fixed;
  ASSERT_TRUE(
      fixed.ParseFromString(std::string(kPhaseOne, sizeof(kPhaseOne) - 1)));
  ASSERT_EQ(fixed.nodes_size(), 2);

  for (int legacy_mask : {1, 2, 3}) {
    SCOPED_TRACE(legacy_mask);
    AstNodeTypeInfoProto legacy = *outer;
    SumTypeProto* sum = legacy.mutable_type()->mutable_sum_type();
    if (legacy_mask == 3) {
      // Only the source-location envelope comes from the current writer;
      // the complete legacy payloads come from the fixed old wire records.
      sum->clear_definition_id();
      sum->clear_tag_bit_count();
      sum->mutable_variants(0)->clear_discriminant();
    }
    for (int i = 0; i < 2; ++i) {
      if ((legacy_mask & (1 << i)) != 0) {
        *sum->mutable_variants(0)->mutable_payload_members(i) =
            fixed.nodes(i).type();
      }
    }
    auto human = ToHumanString(legacy, import_data, import_data.file_table());
    XLS_EXPECT_OK(human);
    if (human.ok()) {
      EXPECT_THAT(
          *human,
          testing::HasSubstr(legacy_mask & 1 ? "Option {" : "Option<u32:8> {"));
      EXPECT_THAT(*human,
                  testing::HasSubstr(legacy_mask & 2 ? "Phantom {"
                                                     : "Phantom<u32:9> {"));
    }
  }

  AstNodeTypeInfoProto wrong_argument = *outer;
  auto* phantom = wrong_argument.mutable_type()
                      ->mutable_sum_type()
                      ->mutable_variants(0)
                      ->mutable_payload_members(1)
                      ->mutable_sum_type();
  ASSERT_EQ(phantom->parametric_arguments_size(), 1);
  ASSERT_TRUE(phantom->parametric_arguments(0).has_value());
  phantom->mutable_parametric_arguments(0)
      ->mutable_value()
      ->mutable_bits()
      ->set_data(std::string("\0\0\0\x08", 4));
  EXPECT_THAT(
      ToHumanString(wrong_argument, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             testing::HasSubstr("payload type mismatch")));
  phantom->clear_parametric_arguments();
  EXPECT_THAT(
      ToHumanString(wrong_argument, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             testing::HasSubstr("argument count mismatch")));

  AstNodeTypeInfoProto wrong_payload = *outer;
  TypeProto* option = wrong_payload.mutable_type()
                          ->mutable_sum_type()
                          ->mutable_variants(0)
                          ->mutable_payload_members(0);
  *option = fixed.nodes(0).type();
  option->mutable_sum_type()
      ->mutable_variants(1)
      ->mutable_payload_members(0)
      ->mutable_bits_type()
      ->mutable_dim()
      ->mutable_interp_value()
      ->mutable_bits()
      ->set_data(std::string("\0\0\0\x10", 4));
  EXPECT_THAT(
      ToHumanString(wrong_payload, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             testing::HasSubstr("payload type mismatch")));
}

// Fixed pre-reference wire records for the source below, not output from the
// current writer with fields removed. Both contain a SUM_DEF node at
// fake.x:0:0-0:16, a metatype wrapping E, and one u8 payload. Phase One omits
// layout fields; expanded Phase Two records a zero-bit tag and discriminant.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsFixedPhaseOneAndExpandedPhaseTwoRecords) {
  constexpr char kPhaseOne[] =
      "\x0a\x5e\x08\x4e\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x00"
      "\x18\x10\x1a\x3c\x42\x3a\x0a\x38\x6a\x36\x0a\x1c\x0a\x0c\x0a\x06"
      "\x66\x61\x6b\x65\x2e\x78\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61"
      "\x6b\x65\x2e\x78\x10\x00\x18\x10\x12\x16\x0a\x14\x0a\x12\x08\x00"
      "\x12\x0e\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08";
  constexpr char kExpandedPhaseTwo[] =
      "\x0a\x78\x08\x4e\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x00"
      "\x18\x10\x1a\x56\x42\x54\x0a\x52\x6a\x50\x0a\x1c\x0a\x0c\x0a\x06"
      "\x66\x61\x6b\x65\x2e\x78\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61"
      "\x6b\x65\x2e\x78\x10\x00\x18\x10\x12\x20\x0a\x14\x0a\x12\x08\x00"
      "\x12\x0e\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08"
      "\x12\x08\x0a\x06\x08\x00\x10\x00\x1a\x00\x1a\x0e\x0a\x0c\x0a\x0a"
      "\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x00";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule tm,
                           ParseAndTypecheck("enum E { V(u8) }\n", "fake.x",
                                             "fake", &import_data, nullptr));
  for (std::string_view wire :
       {std::string_view(kPhaseOne, sizeof(kPhaseOne) - 1),
        std::string_view(kExpandedPhaseTwo, sizeof(kExpandedPhaseTwo) - 1)}) {
    TypeInfoProto parsed;
    ASSERT_TRUE(parsed.ParseFromString(std::string(wire)));
    ASSERT_EQ(parsed.nodes_size(), 1);
    const TypeProto& type = parsed.nodes(0).type().meta_type().wrapped();
    ASSERT_TRUE(type.has_sum_type());
    EXPECT_FALSE(type.sum_type().has_definition_id());
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string standalone,
        ToHumanString(parsed.nodes(0), import_data, import_data.file_table()));
    EXPECT_THAT(standalone, ::testing::EndsWith(" :: typeof(E { V(uN[8]) })"));
    EXPECT_THAT(ToHumanString(parsed, import_data, import_data.file_table()),
                absl_testing::IsOkAndHolds(standalone));
  }
}

// Captured from the pre-reference writer at 9d391b5a, keeping the original
// Outer SUM_DEF record. The repeated Inner payloads are fully expanded; no
// current-writer output or removed definition IDs supply this legacy oracle.
TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ReadsFixedExpandedPhaseTwoRecordWithRepeatedNestedSums) {
  constexpr char kExpandedPhaseTwo[] =
      "\x0a\x8f\x02\x08\x4e\x12\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e"
      "\x78\x10\x01\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10"
      "\x01\x18\x1e\x1a\xec\x01\x42\xe9\x01\x0a\xe6\x01\x6a\xe3\x01\x0a"
      "\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x01\x18\x00\x12"
      "\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x01\x18\x1e\x12\xb2\x01"
      "\x0a\x52\x6a\x50\x0a\x1c\x0a\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78"
      "\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65\x2e\x78\x10\x00"
      "\x18\x14\x12\x20\x0a\x14\x0a\x12\x08\x00\x12\x0e\x0a\x0c\x0a\x0a"
      "\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08\x12\x08\x0a\x06\x08\x00"
      "\x10\x00\x1a\x00\x1a\x0e\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04"
      "\x00\x00\x00\x00\x0a\x52\x6a\x50\x0a\x1c\x0a\x0c\x0a\x06\x66\x61"
      "\x6b\x65\x2e\x78\x10\x00\x18\x00\x12\x0c\x0a\x06\x66\x61\x6b\x65"
      "\x2e\x78\x10\x00\x18\x14\x12\x20\x0a\x14\x0a\x12\x08\x00\x12\x0e"
      "\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00\x00\x08\x12\x08"
      "\x0a\x06\x08\x00\x10\x00\x1a\x00\x1a\x0e\x0a\x0c\x0a\x0a\x08\x00"
      "\x10\x20\x1a\x04\x00\x00\x00\x00\x12\x08\x0a\x06\x08\x00\x10\x00"
      "\x1a\x00\x1a\x0e\x0a\x0c\x0a\x0a\x08\x00\x10\x20\x1a\x04\x00\x00"
      "\x00\x00";
  constexpr std::string_view kProgram =
      "enum Inner { V(u8) }\nenum Outer { P(Inner, Inner) }\n";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data, nullptr));
  TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(
      std::string(kExpandedPhaseTwo, sizeof(kExpandedPhaseTwo) - 1)));
  ASSERT_EQ(parsed.nodes_size(), 1);
  const TypeProto& type = parsed.nodes(0).type().meta_type().wrapped();
  ASSERT_TRUE(type.has_sum_type());
  EXPECT_FALSE(type.sum_type().has_definition_id());
  ASSERT_EQ(type.sum_type().variants_size(), 1);
  const SumTypeVariantProto& pair = type.sum_type().variants(0);
  ASSERT_EQ(pair.payload_members_size(), 2);
  for (const TypeProto& member : pair.payload_members()) {
    ASSERT_TRUE(member.has_sum_type());
    EXPECT_FALSE(member.sum_type().has_definition_id());
  }
  SerializedSumCounts counts;
  CountSerializedSums(type, counts);
  EXPECT_EQ(counts.definitions, 3);
  EXPECT_EQ(counts.references, 0);
  EXPECT_EQ(counts.bits_types, 2);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string standalone,
      ToHumanString(parsed.nodes(0), import_data, import_data.file_table()));
  EXPECT_THAT(standalone,
              ::testing::EndsWith(" :: typeof(Outer { P(Inner { V(uN[8]) }, "
                                  "Inner { V(uN[8]) }) })"));
  EXPECT_THAT(ToHumanString(parsed, import_data, import_data.file_table()),
              absl_testing::IsOkAndHolds(standalone));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SharedSumCloneOutlivesWrapperAndPreservesSerialization) {
  constexpr std::string_view kProgram = R"(
enum Option { None, Some(u8) }
fn f() -> Option { Option::None }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  ASSERT_EQ(tm.module->GetSumDefs().size(), 1);
  SumDef* sum_def = tm.module->GetSumDefs().front();
  XLS_ASSERT_OK_AND_ASSIGN(Type * source_type,
                           tm.type_info->GetItemOrError(sum_def));
  ASSERT_TRUE(source_type->IsMeta());
  const SumType& source = source_type->AsMeta().wrapped()->AsSum();
  std::unique_ptr<Type> clone;
  {
    std::unique_ptr<Type> wrapper = source.CloneToUnique();
    clone = wrapper->CloneToUnique();
    EXPECT_EQ(&wrapper->AsSum().variants(), &clone->AsSum().variants());
  }
  // The clone retains completed shared data after its wrapper dies. An
  // independently allocated description must compare and serialize identically.
  std::vector<SumTypeVariant> variants;
  for (const SumTypeVariant& variant : source.variants()) {
    variants.push_back(variant.Clone());
  }
  auto independent = std::make_unique<SumType>(*sum_def, std::move(variants));
  EXPECT_EQ(*clone, *independent);
  EXPECT_NE(&clone->AsSum().variants(), &independent->variants());
  tm.type_info->SetItem(sum_def, std::make_unique<MetaType>(std::move(clone)));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto shared_proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  tm.type_info->SetItem(sum_def,
                        std::make_unique<MetaType>(std::move(independent)));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto independent_proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  EXPECT_EQ(shared_proto.SerializeAsString(),
            independent_proto.SerializeAsString());
  XLS_EXPECT_OK(
      ToHumanString(shared_proto, import_data, import_data.file_table()));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RoundTripsSumPayloadTypesUsingCanonicalSourceDeclaration) {
  std::string program = R"(
enum E {
  None,
  A(u8),
  B(u16),
  Pair { first: u8, second: u16 },
}

fn f() -> E { E::Pair { first: u8:1, second: u16:2 } }
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  ASSERT_EQ(tm.module->GetSumDefs().size(), 1);
  std::optional<Type*> nominal_type =
      tm.type_info->GetItem(tm.module->GetSumDefs().front());
  ASSERT_TRUE(nominal_type.has_value());
  ASSERT_TRUE((*nominal_type)->IsMeta());
  ASSERT_TRUE((*nominal_type)->AsMeta().wrapped()->IsSum());

  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  int sum_index = -1;
  for (int i = 0; i < proto.nodes_size(); ++i) {
    if (proto.nodes(i).type().has_sum_type()) {
      sum_index = i;
      break;
    }
  }
  ASSERT_GE(sum_index, 0);

  std::string wire = proto.SerializeAsString();
  TypeInfoProto parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  const SumTypeProto& sum = parsed.nodes(sum_index).type().sum_type();
  ASSERT_TRUE(sum.has_sum_def_span());
  EXPECT_EQ(sum.sum_def_span().start().filename(), "fake.x");
  ASSERT_EQ(sum.variants_size(), 4);
  EXPECT_EQ(sum.variants(0).payload_members_size(), 0);
  EXPECT_EQ(sum.variants(1).payload_members_size(), 1);
  EXPECT_EQ(sum.variants(2).payload_members_size(), 1);
  ASSERT_EQ(sum.variants(3).payload_members_size(), 2);
  EXPECT_TRUE(sum.variants(3).payload_members(0).has_bits_type());
  EXPECT_TRUE(sum.variants(3).payload_members(1).has_bits_type());
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(parsed, import_data, import_data.file_table()));
  EXPECT_THAT(human,
              ::testing::HasSubstr("Pair { first: uN[8], second: uN[16] }"));

  // A bits constructor array is semantically equal to the source's BitsType,
  // although the two representations have different diagnostic strings.
  TypeInfoProto bits_constructor_payload = parsed;
  TypeProto* bits_payload = bits_constructor_payload.mutable_nodes(sum_index)
                                ->mutable_type()
                                ->mutable_sum_type()
                                ->mutable_variants(1)
                                ->mutable_payload_members(0);
  TypeDimProto bits_size = bits_payload->bits_type().dim();
  ArrayTypeProto* bits_array = bits_payload->mutable_array_type();
  *bits_array->mutable_size() = bits_size;
  BitsValueProto* is_signed = bits_array->mutable_element_type()
                                  ->mutable_bits_constructor_type()
                                  ->mutable_is_signed()
                                  ->mutable_interp_value()
                                  ->mutable_bits();
  is_signed->set_bit_count(1);
  is_signed->set_is_signed(false);
  is_signed->set_data(std::string(1, '\0'));
  XLS_ASSERT_OK_AND_ASSIGN(std::string bits_constructor_text,
                           ToHumanString(bits_constructor_payload, import_data,
                                         import_data.file_table()));
  EXPECT_THAT(bits_constructor_text,
              ::testing::HasSubstr("A(xN[is_signed=0][8])"));

  is_signed->set_data(std::string(1, '\1'));
  EXPECT_THAT(
      ToHumanString(bits_constructor_payload, import_data,
                    import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("payload type mismatch")));

  TypeInfoProto swapped_payload_types = parsed;
  swapped_payload_types.mutable_nodes(sum_index)
      ->mutable_type()
      ->mutable_sum_type()
      ->mutable_variants()
      ->SwapElements(1, 2);
  EXPECT_THAT(
      ToHumanString(swapped_payload_types, import_data,
                    import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("payload type mismatch")));

  TypeInfoProto missing_source_span = proto;
  missing_source_span.mutable_nodes(sum_index)
      ->mutable_type()
      ->mutable_sum_type()
      ->clear_sum_def_span();
  EXPECT_THAT(
      ToHumanString(missing_source_span, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("missing its source definition span")));

  TypeInfoProto missing_payload = proto;
  missing_payload.mutable_nodes(sum_index)
      ->mutable_type()
      ->mutable_sum_type()
      ->mutable_variants(1)
      ->clear_payload_members();
  EXPECT_THAT(
      ToHumanString(missing_payload, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant payload member count mismatch")));

  TypeInfoProto missing_variant = proto;
  missing_variant.mutable_nodes(sum_index)
      ->mutable_type()
      ->mutable_sum_type()
      ->mutable_variants()
      ->RemoveLast();
  EXPECT_THAT(
      ToHumanString(missing_variant, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Sum variant count mismatch")));

  TypeInfoProto meta_payload = proto;
  TypeProto* payload_member = meta_payload.mutable_nodes(sum_index)
                                  ->mutable_type()
                                  ->mutable_sum_type()
                                  ->mutable_variants(1)
                                  ->mutable_payload_members(0);
  TypeProto original_member = *payload_member;
  payload_member->clear_type_oneof();
  *payload_member->mutable_meta_type()->mutable_wrapped() = original_member;
  EXPECT_THAT(
      ToHumanString(meta_payload, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("invalid meta-type payload member")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsSumPayloadsFromDifferentDeclarationsWithIdenticalText) {
  constexpr std::string_view kDeclarations = R"(
pub enum Tag : u8 { A = 0 }
pub enum Never {}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule left,
      ParseAndTypecheck(kDeclarations, "left.x", "left", &import_data));
  (void)left;
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule right,
      ParseAndTypecheck(kDeclarations, "right.x", "right", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(EnumDef * right_enum,
                           right.module->GetMemberOrError<EnumDef>("Tag"));
  constexpr std::string_view kProgram = R"(
import left;
enum E { None, Value(left::Tag), Impossible(left::Never) }
fn f() -> E { E::None }
)";
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "fake.x", "fake", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto proto,
                           TypeInfoToProto(*tm.type_info, tm.module));
  const AstNodeTypeInfoProto* sum_node =
      FindSumTypeInfoNode(proto, "E", import_data);
  ASSERT_NE(sum_node, nullptr);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string human,
      ToHumanString(*sum_node, import_data, import_data.file_table()));
  EXPECT_THAT(human,
              ::testing::EndsWith(
                  " :: E { None | Value(Tag) | Impossible(Never {  }) }"));

  AstNodeTypeInfoProto wrong_enum = *sum_node;
  *wrong_enum.mutable_type()
       ->mutable_sum_type()
       ->mutable_variants(1)
       ->mutable_payload_members(0)
       ->mutable_enum_type()
       ->mutable_enum_def()
       ->mutable_span() = ToProto(right_enum->span(), import_data.file_table());
  EXPECT_THAT(
      ToHumanString(wrong_enum, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("payload type mismatch")));

  AstNodeTypeInfoProto wrong_sum = *sum_node;
  *wrong_sum.mutable_type()
       ->mutable_sum_type()
       ->mutable_variants(2)
       ->mutable_payload_members(0)
       ->mutable_sum_type()
       ->mutable_sum_def_span() = ToProto(
      right.module->GetSumDefs().front()->span(), import_data.file_table());
  EXPECT_THAT(
      ToHumanString(wrong_sum, import_data, import_data.file_table()),
      absl_testing::StatusIs(absl::StatusCode::kInvalidArgument,
                             ::testing::HasSubstr("payload type mismatch")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       ImportModuleAndTypeAliasAnEnum) {
  std::string imported = R"(
pub enum Foo : u32 {
  A = 42,
}
)";

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(imported, "my_imported_module.x", "my_imported_module",
                        &import_data));
  (void)tm;

  std::string program = R"(
import my_imported_module;

type MyFoo = my_imported_module::Foo;
)";
  DoRun(program, /*proto_out=*/nullptr, &import_data);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, ProcWithImpl) {
  std::string program = R"(
proc Foo { a: u32 }
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest, BitsConstructorTypeProto) {
  std::string program = R"(
fn distinct<COUNT: u32, N: u32, S: bool>(items: xN[S][N][COUNT], valid: bool[COUNT]) -> bool { fail!("unimplemented", zero!<bool>()) }

#[test]
fn test_simple_nondistinct() {
    assert_eq(distinct(u2[2]:[1, 1], bool[2]:[true, true]), false)
}
)";
  DoRun(program);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       SkipsSyntheticNoFileEntriesInHumanizedOutput) {
  std::string program = R"(
fn bool_update() -> bool[1] {
  update(bool[1]:[false], u1:0, true)
}

fn bit_update() -> u8 {
  bit_slice_update(u8:0, u3:0, true)
}
)";
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string nodes_text,
      ToHumanString(tip, import_data, import_data.file_table()));

  EXPECT_THAT(nodes_text,
              ::testing::HasSubstr("update(bool[1]:[false], u1:0, true)"));
  EXPECT_THAT(nodes_text,
              ::testing::HasSubstr("bit_slice_update(u8:0, u3:0, true)"));
  EXPECT_THAT(nodes_text, ::testing::Not(::testing::HasSubstr("<no-file>")));
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       TuplePatternUsesDistinctAstNodeKind) {
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_TUPLE_PATTERN), 76);
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_SUM_VARIANT_PAYLOAD_PATTERN), 77);
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_SUM_DEF), 78);
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_SUM_VARIANT), 79);
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_SUM_INSTANCE), 80);
  EXPECT_EQ(static_cast<int>(AST_NODE_KIND_STRUCT_PATTERN), 81);

  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck("fn f() -> u32 { let (x, y) = (u32:1, u32:2); x }",
                        "fake.x", "fake", &import_data, nullptr));
  XLS_ASSERT_OK_AND_ASSIGN(TypeInfoProto tip,
                           TypeInfoToProto(*tm.type_info, tm.module));
  XLS_ASSERT_OK(ToHumanString(tip, import_data, import_data.file_table()));

  bool found_tuple_pattern = false;
  for (const AstNodeTypeInfoProto& node : tip.nodes()) {
    found_tuple_pattern |= node.kind() == AST_NODE_KIND_TUPLE_PATTERN;
    EXPECT_NE(static_cast<int>(node.kind()),
              kLegacyNameDefTreeAstNodeKindProtoValue);
  }
  EXPECT_TRUE(found_tuple_pattern);
}

TEST_F(TypeInfoToProtoWithBothTypecheckVersionsTest,
       RejectsLegacyNameDefTreeAstNodeKind) {
  ImportData import_data = CreateImportDataForTest();
  AstNodeTypeInfoProto legacy;
  legacy.set_kind(
      static_cast<AstNodeKindProto>(kLegacyNameDefTreeAstNodeKindProtoValue));
  legacy.mutable_type()->mutable_token_type();

  EXPECT_THAT(
      ToHumanString(legacy, import_data, import_data.file_table()),
      absl_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          ::testing::HasSubstr("Legacy NameDefTree type-info entries")));
}

}  // namespace
}  // namespace xls::dslx
