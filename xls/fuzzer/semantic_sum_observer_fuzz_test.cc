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

// Bounded companions to the source fuzzer: observe actual bytecode trace events
// and execute source after AutoFmt, reparsing and typechecking it
// independently. These finite templates cover depth-one/two sums, not arbitrary
// declarations.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "fuzztest/fuzztest.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/bytecode/bytecode.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/bytecode/bytecode_interpreter_options.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/fmt/ast_fmt.h"
#include "xls/dslx/fmt/comments.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/comment_data.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/ir/evaluator_result.pb.h"

namespace xls::dslx {
namespace {

using ::absl_testing::IsOkAndHolds;
using ::absl_testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

enum class TraceShape { kFlat, kNested };
enum class SourceShape { kUnit, kTuple, kNamed, kNested };

absl::StatusOr<std::unique_ptr<BytecodeFunction>> EmitFunction(
    ImportData& import_data, const TypecheckedModule& tm,
    std::string_view entry) {
  XLS_ASSIGN_OR_RETURN(Function * function,
                       tm.module->GetMemberOrError<Function>(entry));
  return BytecodeEmitter::Emit(&import_data, tm.type_info, *function,
                               ParametricEnv());
}

std::string TraceProgram(int64_t small_width, bool is_signed,
                         TraceShape shape) {
  return absl::StrCat(
      "enum Inner: u2 { Small(", is_signed ? "s" : "u", small_width,
      ") = 0, Large(u8) = 1, Empty = 2 }\n",
      "enum Outer: u2 { Wrapped(Inner) = 0, Wide(u16) = 1 }\n",
      "type Observed = ", shape == TraceShape::kFlat ? "Inner" : "Outer",
      R"(;
fn observe(x: Observed) -> Observed {
    let x = trace!(x);
    trace_fmt!("value: {}", x);
    x
}
fn observe_pair(x: Observed) -> Observed {
    trace_fmt!("prefix {} middle {} suffix", u8:17, x);
    x
}
fn identity(prefix: u8, x: Observed) -> Observed { x }
fn aggregate_identity(x: (u8, Observed[1])) -> (u8, Observed[1]) { x }
fn bits_identity(x: u8) -> u8 { x }
)");
}

InterpValue RawSum(uint64_t tag, int64_t payload_width, uint64_t payload) {
  return InterpValue::MakeTuple({InterpValue::MakeUBits(2, tag),
                                 InterpValue::MakeTuple({InterpValue::MakeUBits(
                                     payload_width, payload)})});
}

struct TraceCase {
  InterpValue value;
  std::string text;
};

std::vector<TraceCase> TraceCases(int64_t small_width, bool is_signed,
                                  uint16_t payload, TraceShape shape) {
  const uint64_t inner_payload = payload & 0xff;
  int64_t small = inner_payload & ((uint64_t{1} << small_width) - 1);
  if (is_signed && (small & (int64_t{1} << (small_width - 1))) != 0) {
    small -= int64_t{1} << small_width;
  }
  const std::vector<std::string> inner_text = {
      absl::StrCat("Inner::Small(", small, ")"),
      absl::StrCat("Inner::Large(", inner_payload, ")"), "Inner::Empty"};
  std::vector<TraceCase> cases;
  for (int64_t tag = 0; tag < 3; ++tag) {
    if (shape == TraceShape::kFlat) {
      cases.push_back({RawSum(tag, 8, inner_payload), inner_text[tag]});
    } else {
      // Inner occupies tag[2] ++ payload[8] in the low ten bits. Preserve
      // independently varied padding above Inner and above Small's payload.
      const uint64_t outer_payload =
          (payload & 0xfc00) | (tag << 8) | inner_payload;
      cases.push_back({RawSum(0, 16, outer_payload),
                       absl::StrCat("Outer::Wrapped(", inner_text[tag], ")")});
    }
  }
  if (shape == TraceShape::kNested) {
    cases.push_back(
        {RawSum(1, 16, payload), absl::StrCat("Outer::Wide(", payload, ")")});
  }
  return cases;
}

void ExpectRedactedCall(const DslxInterpreterEvents& events) {
  const auto& messages = events.AsProto().trace_msgs();
  ASSERT_EQ(messages.size(), 2);
  ASSERT_TRUE(messages[0].has_call());
  EXPECT_EQ(messages[0].call().args_size(), 0);
  ASSERT_TRUE(messages[1].has_call_return());
  EXPECT_FALSE(messages[1].call_return().has_return_value());
}

void TraceTextAndCallsPreserveRepresentation(int64_t small_width,
                                             bool is_signed, uint16_t payload,
                                             TraceShape shape) {
  const std::string program = TraceProgram(small_width, is_signed, shape);
  SCOPED_TRACE(program);
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "trace.x", "trace", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(auto observe,
                           EmitFunction(import_data, tm, "observe"));
  XLS_ASSERT_OK_AND_ASSIGN(auto identity,
                           EmitFunction(import_data, tm, "identity"));
  XLS_ASSERT_OK_AND_ASSIGN(auto aggregate_identity,
                           EmitFunction(import_data, tm, "aggregate_identity"));
  BytecodeInterpreterOptions call_options;
  call_options.trace_calls(true);

  for (const TraceCase& c :
       TraceCases(small_width, is_signed, payload, shape)) {
    SCOPED_TRACE(c.text);
    SCOPED_TRACE(c.value.ToString());
    DslxInterpreterEvents events;
    EXPECT_THAT(BytecodeInterpreter::Interpret(
                    &import_data, observe.get(), {c.value}, std::nullopt,
                    BytecodeInterpreterOptions(), &events),
                IsOkAndHolds(c.value));
    EXPECT_THAT(events.GetTraceMessageStrings(),
                ElementsAre(absl::StrCat("trace of x: ", c.text),
                            absl::StrCat("value: ", c.text)));

    DslxInterpreterEvents call_events;
    EXPECT_THAT(
        BytecodeInterpreter::Interpret(
            &import_data, identity.get(), {InterpValue::MakeU8(17), c.value},
            std::nullopt, call_options, &call_events),
        IsOkAndHolds(c.value));
    EXPECT_THAT(call_events.GetTraceMessageStrings(),
                ElementsAre(absl::StrCat("identity(u8:17, ", c.text, ")"),
                            absl::StrCat("identity(...) => ", c.text)));
    ExpectRedactedCall(call_events);

    XLS_ASSERT_OK_AND_ASSIGN(InterpValue array,
                             InterpValue::MakeArray({c.value}));
    const InterpValue aggregate =
        InterpValue::MakeTuple({InterpValue::MakeU8(17), array});
    DslxInterpreterEvents aggregate_events;
    EXPECT_THAT(BytecodeInterpreter::Interpret(
                    &import_data, aggregate_identity.get(), {aggregate},
                    std::nullopt, call_options, &aggregate_events),
                IsOkAndHolds(aggregate));
    EXPECT_THAT(aggregate_events.GetTraceMessageStrings(),
                ElementsAre(HasSubstr(c.text), HasSubstr(c.text)));
    ExpectRedactedCall(aggregate_events);
  }

  // A non-sum call is a positive control: redaction must not erase all values.
  XLS_ASSERT_OK_AND_ASSIGN(auto bits_identity,
                           EmitFunction(import_data, tm, "bits_identity"));
  DslxInterpreterEvents bits_events;
  EXPECT_THAT(BytecodeInterpreter::Interpret(
                  &import_data, bits_identity.get(), {InterpValue::MakeU8(17)},
                  std::nullopt, call_options, &bits_events),
              IsOkAndHolds(InterpValue::MakeU8(17)));
  const auto& messages = bits_events.AsProto().trace_msgs();
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0].call().args_size(), 1);
  EXPECT_TRUE(messages[1].call_return().has_return_value());
}
FUZZ_TEST(SemanticSumObserverFuzzTest, TraceTextAndCallsPreserveRepresentation)
    .WithDomains(fuzztest::ElementOf<int64_t>({1, 4, 7}),
                 fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<uint16_t>(),
                 fuzztest::ElementOf<TraceShape>({TraceShape::kFlat,
                                                  TraceShape::kNested}));

void MalformedTracesProduceNoPartialEvent(int64_t small_width, uint16_t payload,
                                          TraceShape shape) {
  const std::string program = TraceProgram(small_width, false, shape);
  SCOPED_TRACE(program);
  ImportData import_data = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(program, "malformed.x", "malformed", &import_data));
  XLS_ASSERT_OK_AND_ASSIGN(auto observe_pair,
                           EmitFunction(import_data, tm, "observe_pair"));
  XLS_ASSERT_OK_AND_ASSIGN(auto identity,
                           EmitFunction(import_data, tm, "identity"));
  std::vector<InterpValue> malformed;
  if (shape == TraceShape::kFlat) {
    malformed.push_back(RawSum(3, 8, payload & 0xff));
  } else {
    malformed.push_back(RawSum(3, 16, payload));
    malformed.push_back(RawSum(0, 16, (payload & 0xfcff) | 0x300));
  }
  for (const InterpValue& value : malformed) {
    SCOPED_TRACE(value.ToString());
    const std::vector<InterpValue> args = {InterpValue::MakeU8(17), value};
    // The input is transportable. Failure must arise from the observer.
    EXPECT_THAT(
        BytecodeInterpreter::Interpret(&import_data, identity.get(), args),
        IsOkAndHolds(value));
    DslxInterpreterEvents events;
    EXPECT_THAT(BytecodeInterpreter::Interpret(
                    &import_data, observe_pair.get(), {value}, std::nullopt,
                    BytecodeInterpreterOptions(), &events),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("is not declared")));
    EXPECT_THAT(events.GetTraceMessageStrings(), IsEmpty());
    EXPECT_EQ(events.AsProto().trace_msgs_size(), 0);

    // The earlier ordinary argument must not publish a partial call event.
    BytecodeInterpreterOptions call_options;
    call_options.trace_calls(true);
    DslxInterpreterEvents call_events;
    EXPECT_THAT(BytecodeInterpreter::Interpret(&import_data, identity.get(),
                                               args, std::nullopt, call_options,
                                               &call_events),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("is not declared")));
    EXPECT_THAT(call_events.GetTraceMessageStrings(), IsEmpty());
    EXPECT_EQ(call_events.AsProto().trace_msgs_size(), 0);
  }
}
FUZZ_TEST(SemanticSumObserverFuzzTest, MalformedTracesProduceNoPartialEvent)
    .WithDomains(fuzztest::ElementOf<int64_t>({1, 4, 7}),
                 fuzztest::Arbitrary<uint16_t>(),
                 fuzztest::ElementOf<TraceShape>({TraceShape::kFlat,
                                                  TraceShape::kNested}));

std::string FormatterProgram(SourceShape shape, int64_t small_width) {
  const std::string narrow = absl::StrCat("u", small_width);
  switch (shape) {
    case SourceShape::kUnit:
      return absl::StrCat(
          "enum Choice:u2{Idle=0,Value(", narrow, ")=3}\n",
          "fn main(select:u2,x:u8,y:u8)->u32{let v=if select==u2:0{",
          "Choice::Idle}else{Choice::Value(x as ", narrow,
          ")};match v{Choice::Idle=>u32:1000,",
          "Choice::Value(p)=>u32:2000+(p as u32)}}");
    case SourceShape::kTuple:
      return absl::StrCat(
          "enum Choice:u2{Left(", narrow, ",u8)=0,Right(u8,", narrow,
          ")=3}\nfn main(select:u2,x:u8,y:u8)->u32{",
          "let v=if select==u2:0{Choice::Left(x as ", narrow,
          ",y)}else{Choice::Right(x,y as ", narrow, ")};match v{",
          "Choice::Left(a,b)=>u32:1000+(a as u32)*u32:257+(b as u32),",
          "Choice::Right(a,b)=>u32:2000+(a as u32)*u32:257+(b as u32)}}");
    case SourceShape::kNamed:
      return absl::StrCat(
          "enum Choice:u2{Left{small:", narrow,
          ",wide:u8}=0,Right{wide:u8,small:", narrow, "}=3}\n",
          "fn main(select:u2,x:u8,y:u8)->u32{let v=if select==u2:0{",
          "Choice::Left{wide:y,small:x as ", narrow,
          "}}else{Choice::Right{small:y as ", narrow, ",wide:x}};match v{",
          "Choice::Left{wide:b,small:a}=>",
          "u32:1000+(a as u32)*u32:257+(b as u32),",
          "Choice::Right{small:b,wide:a}=>",
          "u32:2000+(a as u32)*u32:257+(b as u32)}}");
    case SourceShape::kNested:
      return absl::StrCat(
          "enum Inner{Empty,Data(", narrow,
          ",u8)}\nenum Choice{Idle,Wrapped(Inner)}\n",
          "fn main(select:u2,x:u8,y:u8)->u32{let v=if select==u2:0{",
          "Choice::Idle}else if select==u2:1{Choice::Wrapped(Inner::Empty)}",
          "else{Choice::Wrapped(Inner::Data(x as ", narrow, ",y))};match v{",
          "Choice::Idle=>u32:1000,Choice::Wrapped(Inner::Empty)=>u32:2000,",
          "Choice::Wrapped(Inner::Data(a,b))=>",
          "u32:3000+(a as u32)*u32:257+(b as u32),invalid!=>u32:4000}}");
  }
}

void FormatterPreservesRuntimeBehavior(SourceShape shape, int64_t small_width,
                                       uint8_t payload, int64_t text_width) {
  const std::string program = FormatterProgram(shape, small_width);
  SCOPED_TRACE(program);
  ImportData original_imports = CreateImportDataForTest();
  std::vector<CommentData> comment_data;
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule original,
                           ParseAndTypecheck(program, "original.x", "original",
                                             &original_imports, &comment_data));
  Comments comments = Comments::Create(comment_data);
  UniformContentFilesystem vfs(program);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::string formatted,
      AutoFmt(vfs, *original.module, comments, program, text_width));
  SCOPED_TRACE(formatted);

  // A separate owner prevents the second typecheck from reusing the first AST
  // or TypeInfo. Both executions are checked against the known input
  // arithmetic.
  ImportData formatted_imports = CreateImportDataForTest();
  XLS_ASSERT_OK_AND_ASSIGN(TypecheckedModule reparsed,
                           ParseAndTypecheck(formatted, "formatted.x",
                                             "formatted", &formatted_imports));
  XLS_ASSERT_OK_AND_ASSIGN(auto original_main,
                           EmitFunction(original_imports, original, "main"));
  XLS_ASSERT_OK_AND_ASSIGN(auto formatted_main,
                           EmitFunction(formatted_imports, reparsed, "main"));
  const uint64_t mask = (uint64_t{1} << small_width) - 1;
  const uint64_t x = payload;
  const uint64_t y =
      payload ^ 0xff;  // Distinct values expose payload reordering.
  const int64_t choice_count = shape == SourceShape::kNested ? 3 : 2;
  for (int64_t select = 0; select < choice_count; ++select) {
    SCOPED_TRACE(select);
    uint64_t expected;
    if (shape == SourceShape::kUnit) {
      expected = select == 0 ? 1000 : 2000 + (x & mask);
    } else if (shape == SourceShape::kNested) {
      expected = select < 2 ? 1000 * (select + 1) : 3000 + (x & mask) * 257 + y;
    } else {
      expected = select == 0 ? 1000 + (x & mask) * 257 + y
                             : 2000 + x * 257 + (y & mask);
    }
    const std::vector<InterpValue> args = {InterpValue::MakeUBits(2, select),
                                           InterpValue::MakeU8(x),
                                           InterpValue::MakeU8(y)};
    XLS_ASSERT_OK_AND_ASSIGN(InterpValue before,
                             BytecodeInterpreter::Interpret(
                                 &original_imports, original_main.get(), args));
    XLS_ASSERT_OK_AND_ASSIGN(
        InterpValue after, BytecodeInterpreter::Interpret(
                               &formatted_imports, formatted_main.get(), args));
    EXPECT_EQ(before, InterpValue::MakeU32(expected));
    EXPECT_EQ(after, InterpValue::MakeU32(expected));
    EXPECT_EQ(after, before);
  }
}
FUZZ_TEST(SemanticSumObserverFuzzTest, FormatterPreservesRuntimeBehavior)
    .WithDomains(fuzztest::ElementOf<SourceShape>({SourceShape::kUnit,
                                                   SourceShape::kTuple,
                                                   SourceShape::kNamed,
                                                   SourceShape::kNested}),
                 fuzztest::ElementOf<int64_t>({1, 4, 7}),
                 fuzztest::Arbitrary<uint8_t>(),
                 fuzztest::ElementOf<int64_t>({32, 60, 100}));

TEST(SemanticSumObserverFuzzTest, SmallTemplatesHaveConstructorWitnesses) {
  for (TraceShape shape : {TraceShape::kFlat, TraceShape::kNested}) {
    TraceTextAndCallsPreserveRepresentation(4, true, 0xfdae, shape);
    MalformedTracesProduceNoPartialEvent(4, 0xfda5, shape);
  }
  for (SourceShape shape : {SourceShape::kUnit, SourceShape::kTuple,
                            SourceShape::kNamed, SourceShape::kNested}) {
    FormatterPreservesRuntimeBehavior(shape, 4, 0xa5, 32);
  }
}

}  // namespace
}  // namespace xls::dslx
