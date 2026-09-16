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

#include "xls/dslx/run_routines/run_routines.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_builder.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "re2/re2.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/dslx/bytecode/bytecode.h"
#include "xls/dslx/bytecode/bytecode_cache.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/bytecode/bytecode_interpreter_options.h"
#include "xls/dslx/bytecode/proc_hierarchy_interpreter.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/error_printer.h"
#include "xls/dslx/errors.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/bindings.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_generator.h"
#include "xls/dslx/interp_value_utils.h"
#include "xls/dslx/ir_convert/conversion_info.h"
#include "xls/dslx/ir_convert/convert_options.h"
#include "xls/dslx/ir_convert/function_converter.h"
#include "xls/dslx/ir_convert/ir_converter.h"
#include "xls/dslx/mangle.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/run_routines/test_xml.h"
#include "xls/dslx/sum_type_encoding.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/ir/bits.h"
#include "xls/ir/events.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/value_utils.h"
#include "xls/passes/dce_pass.h"
#include "xls/passes/dfe_pass.h"
#include "xls/passes/inlining_pass.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/optimization_pass_pipeline.h"
#include "xls/passes/pass_base.h"
#include "xls/solvers/solver.h"
#include "xls/solvers/z3_ir_translator.h"

namespace xls::dslx {
namespace {
// A few constants relating to the number of spaces to use in text formatting
// our test-runner output.
constexpr int kUnitSpaces = 7;
constexpr int kQuickcheckSpaces = 15;

// Reconstruct a validated exhaustive candidate as a fresh source value. This
// zeros constructor-owned padding without changing ordinary value transport.
absl::StatusOr<InterpValue> ConstructCanonicalQuickCheckInput(
    const Type& type, const InterpValue& value) {
  if (!TypeContainsSemanticSum(type)) {
    return value;
  } else if (type.IsSum()) {
    const SumType& sum = type.AsSum();
    const SumTypeEncoding encoding(sum);
    XLS_ASSIGN_OR_RETURN(SumTypeEncoding::VariantInfo variant,
                         encoding.GetVariantByTagBits(
                             value.GetValuesOrDie().at(0).GetBitsOrDie()));
    XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> members,
                         GetSumPayloadValues(sum, value));
    for (int64_t i = 0; i < members.size(); ++i) {
      XLS_ASSIGN_OR_RETURN(members[i],
                           ConstructCanonicalQuickCheckInput(
                               variant.variant->GetMemberType(i), members[i]));
    }
    return CreateSumValue(sum, variant.variant->variant().identifier(),
                          members);
  } else {
    auto member_type = [&](int64_t i) -> const Type& {
      if (const auto* array = dynamic_cast<const ArrayType*>(&type)) {
        return array->element_type();
      } else if (const auto* structure =
                     dynamic_cast<const StructTypeBase*>(&type)) {
        return structure->GetMemberType(i);
      } else {
        return type.AsTuple().GetMemberType(i);
      }
    };
    const std::vector<InterpValue>& old_members = value.GetValuesOrDie();
    std::vector<InterpValue> members;
    members.reserve(old_members.size());
    for (int64_t i = 0; i < old_members.size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          InterpValue member,
          ConstructCanonicalQuickCheckInput(member_type(i), old_members[i]));
      members.push_back(std::move(member));
    }
    if (value.IsArray()) {
      return InterpValue::MakeArray(std::move(members));
    } else {
      return InterpValue::MakeTuple(std::move(members));
    }
  }
}

// Helper routine for handling an error that occurs as the result of a test
// execution. Prints the error and that the test failed to stderr. Adds the test
// case to the accumulated test result data in `result`.
//
// Generally we expect that errors should be positional, but we will
// present it as an internal error if it is not instead of crashing because it
// results in better UX at this level of handling.
//
// Precondition: status must not be OK.
//
// Args:
// - result: the test result data to update
// - status: the status, e.g. a positional error that resulted from running
// - test_name: the name of the test (note that these are tests like a
// `--test_filter` flag would target)
// - start_pos: the position of the test construct
// - start: the time this particular test started
// - duration: the duration of the test
// - is_quickcheck: whether this is a quickcheck test
// - file_table: the file table to use for error reporting
// - vfs: the virtual file system to use for error reporting
void HandleError(TestResultData& result, const absl::Status& status,
                 std::string_view test_name, const Pos& start_pos,
                 const absl::Time& start, const absl::Duration& duration,
                 bool is_quickcheck, FileTable& file_table,
                 VirtualizableFilesystem& vfs) {
  CHECK(!status.ok()) << "HandleError called with status that is OK";
  VLOG(1) << "Handling error; status: " << status
          << " test_name: " << test_name;
  absl::StatusOr<PositionalErrorData> data =
      GetPositionalErrorData(status, std::nullopt, file_table);

  std::string one_liner;
  std::string suffix;
  if (data.ok()) {
    CHECK_OK(PrintPositionalError(data->spans, data->GetMessageWithType(),
                                  std::cerr, PositionalErrorColor::kErrorColor,
                                  file_table, vfs));
    one_liner = data->GetMessageWithType();
  } else {
    // If we can't extract positional data we log the error and put the error
    // status into the "failed" prompted.
    LOG(ERROR) << "Internal error -- test " << test_name
               << " failed with a status that did not have DSLX position data: "
               << status;
    suffix = absl::StrCat(": internal error: ", status.ToString());
    one_liner = suffix;
  }

  // Add to test tracking data.
  result.AddTestCase(
      test_xml::TestCase{.name = std::string(test_name),
                         .file = std::string{start_pos.GetFilename(file_table)},
                         .line = start_pos.GetHumanLineno(),
                         .status = test_xml::RunStatus::kRun,
                         .result = test_xml::RunResult::kCompleted,
                         .time = duration,
                         .timestamp = start,
                         .failure = test_xml::Failure{.message = one_liner}});

  std::string spaces((is_quickcheck ? kQuickcheckSpaces : kUnitSpaces), ' ');
  std::cerr << absl::StreamFormat("[ %sFAILED ] %s%s", spaces, test_name,
                                  suffix)
            << "\n";
};

absl::Status RunDslxTestFunction(ImportData* import_data, TypeInfo* type_info,
                                 const Module* module, TestFunction* tf,
                                 const BytecodeInterpreterOptions& options,
                                 std::optional<DslxInterpreterEvents*> events) {
  auto cache = std::make_unique<BytecodeCache>();
  import_data->SetBytecodeCache(std::move(cache));
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<BytecodeFunction> bf,
      BytecodeEmitter::Emit(
          import_data, type_info, tf->fn(), std::nullopt,
          BytecodeEmitterOptions{.format_preference =
                                     options.format_preference()}));
  return BytecodeInterpreter::Interpret(import_data, bf.get(), /*args=*/{},
                                        /*channel_manager=*/std::nullopt,
                                        options, events)
      .status();
}

template <typename ProcType>
absl::Status RunDslxTestProc(ImportData* import_data, const Module* module,
                             ProcType* tp, TypeInfo* ti,
                             std::optional<std::string> expected_fail_label,
                             const BytecodeInterpreterOptions& options) {
  auto cache = std::make_unique<BytecodeCache>();
  import_data->SetBytecodeCache(std::move(cache));

  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<ProcHierarchyInterpreter> hierarchy_interpreter,
      ProcHierarchyInterpreter::Create(import_data, ti, tp, options));

  // There should be a single top config argument: the terminator
  // channel. Determine the actual channel object.
  XLS_RET_CHECK_EQ(hierarchy_interpreter->InterfaceArgs().size(), 1);
  std::string terminal_channel_name =
      std::string{hierarchy_interpreter->GetInterfaceChannelName(0)};
  InterpValueChannel& terminal_channel =
      hierarchy_interpreter->GetInterfaceChannel(0);

  // Run until a single output appears in the terminal channel.
  absl::Status status =
      hierarchy_interpreter->TickUntilOutput({{terminal_channel_name, 1}})
          .status();
  std::optional<std::string> fail_label_opt =
      GetAssertionLabelFromError(status);
  if (!status.ok() && expected_fail_label.has_value() &&
      fail_label_opt.has_value()) {
    if (*fail_label_opt == *expected_fail_label) {
      return absl::OkStatus();
    }
    return FailureErrorStatus(
        tp->span(),
        absl::StrFormat("Proc failed on '%s', but expected to fail on '%s'",
                        *fail_label_opt, *expected_fail_label),
        import_data->file_table());
  }
  XLS_RETURN_IF_ERROR(status);

  InterpValue ret_val = terminal_channel.Read();
  XLS_RET_CHECK(ret_val.IsBool());
  if (!ret_val.IsTrue()) {
    return FailureErrorStatus(tp->span(), "Proc reported failure upon exit.",
                              import_data->file_table());
  }
  return absl::OkStatus();
}

// Pass that adds any asserts in a qc function to the goal and removes the
// assert nodes.
class QuickCheckProveAssertsNotFiredPass final
    : public OptimizationFunctionBasePass {
 public:
  QuickCheckProveAssertsNotFiredPass()
      : OptimizationFunctionBasePass("quickcheck-assert-suplement",
                                     "add asserts to quickcheck goal") {}
  ~QuickCheckProveAssertsNotFiredPass() final = default;

  RedundancyGuard GetRedundancyGuard(
      const OptimizationPassOptions& options,
      OptimizationContext& context) const override {
    return RedundancyGuard::CanSkip();
  }

 protected:
  absl::StatusOr<bool> RunOnFunctionBaseInternal(
      FunctionBase* fb, const OptimizationPassOptions& options,
      PassResults* results, OptimizationContext& context) const override {
    XLS_RET_CHECK(fb->IsFunction()) << "not a qc function";
    xls::Function* f = fb->AsFunctionOrDie();
    XLS_RET_CHECK(f->return_value()->GetType()->IsBits())
        << "not a qc function";
    XLS_RET_CHECK_EQ(f->return_value()->GetType()->GetFlatBitCount(), 1)
        << "not a qc function";
    std::vector<Node*> ret = {f->return_value()};
    XLS_ASSIGN_OR_RETURN(std::vector<Node*> reverse_topo_sort_nodes,
                         context.ReverseTopoSort(f));
    for (Node* n : reverse_topo_sort_nodes) {
      if (n->Is<Assert>()) {
        Assert* a = n->As<Assert>();
        XLS_RETURN_IF_ERROR(a->ReplaceUsesWith(a->token()));
        ret.push_back(a->condition());
        XLS_RETURN_IF_ERROR(fb->RemoveNode(a));
      }
    }

    if (ret.size() == 1) {
      return false;
    }

    XLS_ASSIGN_OR_RETURN(
        Node * new_ret,
        fb->MakeNodeWithName<NaryOp>(f->return_value()->loc(), ret, Op::kAnd,
                                     "result_and_asserts_pass"));
    XLS_RETURN_IF_ERROR(f->set_return_value(new_ret));
    return true;
  }
};
}  // namespace

absl::StatusOr<std::unique_ptr<AbstractParsedTestRunner>>
DslxInterpreterTestRunner::CreateTestRunner(ImportData* import_data,
                                            TypeInfo* type_info, Module* module,
                                            ConvertOptions options) const {
  return std::make_unique<DslxInterpreterParsedTestRunner>(import_data,
                                                           type_info, module);
}
absl::StatusOr<RunResult> DslxInterpreterParsedTestRunner::RunTestFunction(
    std::string_view name, const BytecodeInterpreterOptions& options,
    std::optional<DslxInterpreterEvents*> events) {
  XLS_ASSIGN_OR_RETURN(TestFunction * tf, entry_module_->GetTest(name));
  return RunResult{.result =
                       RunDslxTestFunction(import_data_, type_info_,
                                           entry_module_, tf, options, events)};
}

absl::StatusOr<RunResult> DslxInterpreterParsedTestRunner::RunTestProc(
    std::string_view name, const BytecodeInterpreterOptions& options) {
  if (std::optional<TestProc*> tp = entry_module_->GetMember<TestProc>(name);
      tp.has_value()) {
    XLS_ASSIGN_OR_RETURN(TypeInfo * ti,
                         type_info_->GetTopLevelProcTypeInfo((*tp)->proc()));
    return RunResult{
        .result = RunDslxTestProc(import_data_, entry_module_, (*tp)->proc(),
                                  ti, (*tp)->expected_fail_label(), options)};
  }

  XLS_ASSIGN_OR_RETURN(ProcDef * proc_def,
                       entry_module_->GetMemberOrError<ProcDef>(name));
  XLS_ASSIGN_OR_RETURN(std::vector<ProcInitializerWithTypeInfo> initializers,
                       type_info_->GetCanonicalProcInitializers(proc_def));
  XLS_RET_CHECK_EQ(initializers.size(), 1);

  // TODO: https://github.com/google/xls/issues/4125 - Support an expected fail
  // label.
  return RunResult{
      .result = RunDslxTestProc(import_data_, entry_module_, proc_def,
                                initializers[0].next_type_info,
                                /*expected_fail_label=*/std::nullopt, options)};
}

TestResultData::TestResultData(absl::Time start_time,
                               std::vector<test_xml::TestCase> test_cases)
    : start_time_(start_time), test_cases_(std::move(test_cases)) {}

int64_t TestResultData::GetFailedCount() const {
  return std::count_if(
      test_cases_.begin(), test_cases_.end(),
      [](const auto& test_case) { return test_case.failure.has_value(); });
}
int64_t TestResultData::GetSkippedCount() const {
  return std::count_if(
      test_cases_.begin(), test_cases_.end(), [](const auto& test_case) {
        return test_case.result == test_xml::RunResult::kFiltered;
      });
}

bool TestResultData::DidAnyFail() const {
  return std::any_of(
      test_cases_.begin(), test_cases_.end(),
      [](const auto& test_case) { return test_case.failure.has_value(); });
}

test_xml::TestSuites TestResultData::ToXmlSuites(
    std::string_view module_name) const {
  test_xml::TestCounts counts = {
      .tests = static_cast<int64_t>(test_cases_.size()),
      .failures = GetFailedCount(),
      .disabled = 0,
      .skipped = GetSkippedCount(),
      .errors = 0,
  };
  test_xml::TestSuites suites = {
      .counts = counts,
      .time = duration_,
      .timestamp = start_time_,
      .test_suites =
          {
              // We currently consider all the test cases inside of a single
              // file to be part of one suite.
              //
              // TODO(leary): 2024-02-08 We may want to break out quickcheck
              // tests vs
              // unit tests in the future.
              test_xml::TestSuite{
                  .name = absl::StrCat(module_name, " tests"),
                  .counts = counts,
                  .time = duration_,
                  .timestamp = start_time_,
                  .test_cases = test_cases_,
              },
          },
  };
  return suites;
}

static bool TestMatchesFilter(std::string_view test_name,
                              const RE2* test_filter) {
  if (test_filter == nullptr) {
    // All tests vacuously match the filter if there is no filter (i.e. we run
    // them all).
    return true;
  }
  return RE2::FullMatch(test_name, *test_filter);
}

// Populates a value from a given tuple type with a flat bit contents given by
// `i` -- this is useful for exhaustively iterating through a space to populate
// an aggregate value.
//
// Precondition: tuple_type->GetFlatBitCount() must be <= 64 so we have enough
// data in `i` to populate it.
static std::vector<Value> MakeFromUint64(xls::TupleType* tuple_type,
                                         uint64_t i) {
  // We turn the uint64_t contents into a bit vector of the flat bit count of
  // the tuple type and populate that tuple type from the bit string.
  InlineBitmap bitmap =
      InlineBitmap::FromWord(i, tuple_type->GetFlatBitCount());
  BitmapView view(bitmap);
  Value value = ZeroOfType(tuple_type);
  CHECK_OK(value.PopulateFrom(view));
  return value.GetElements().value();
};

// Skips a range whose members all contain an undeclared active sum tag. The
// result is only a candidate: carrying out of a nested field can change an
// enclosing tag. Call this on rejected exhaustive inputs, not on every valid
// input. Keeping the raw index preserves padding multiplicities and order.
// The exhaustive runner limits the complete index to 48 bits.
static absl::StatusOr<uint64_t> SkipInvalidSumTagRange(const Type& type,
                                                       uint64_t index,
                                                       int64_t bit_offset) {
  auto scan_members = [&](const auto& aggregate,
                          int64_t upper_bit) -> absl::StatusOr<uint64_t> {
    for (int64_t i = 0; i < aggregate.size(); ++i) {
      const Type& member = aggregate.GetMemberType(i);
      XLS_ASSIGN_OR_RETURN(TypeDim member_size, member.GetTotalBitCount());
      XLS_ASSIGN_OR_RETURN(int64_t member_bits, member_size.GetAsInt64());
      upper_bit -= member_bits;
      XLS_ASSIGN_OR_RETURN(uint64_t next,
                           SkipInvalidSumTagRange(member, index, upper_bit));
      if (next != index) {
        return next;
      }
    }
    return index;
  };

  if (!TypeContainsSemanticSum(type)) {
    return index;
  } else if (auto* sum = dynamic_cast<const SumType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(TypeDim slot_size, sum->GetMaxPayloadBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t slot_bits, slot_size.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t tag_bits, sum->tag_bit_count().GetAsInt64());
    const int64_t tag_offset = bit_offset + slot_bits;
    const uint64_t tag_limit = uint64_t{1} << tag_bits;
    const uint64_t tag = (index >> tag_offset) & (tag_limit - 1);
    uint64_t next_tag = tag_limit;
    const SumTypeVariant* active = nullptr;
    for (int64_t i = 0; i < sum->variant_count(); ++i) {
      XLS_ASSIGN_OR_RETURN(uint64_t declared,
                           sum->GetDiscriminant(i).GetBitsOrDie().ToUint64());
      if (declared == tag) {
        active = &sum->variants().at(i);
        break;
      } else if (declared > tag && declared < next_tag) {
        next_tag = declared;
      }
    }
    if (active == nullptr) {
      // Clear this sum and every less-significant field. If no larger tag is
      // declared, next_tag carries into the containing field (or past the
      // complete domain). Every skipped index has this same invalid tag gap.
      const uint64_t low_mask = (uint64_t{1} << (tag_offset + tag_bits)) - 1;
      return (index & ~low_mask) + (next_tag << tag_offset);
    } else {
      XLS_ASSIGN_OR_RETURN(TypeDim active_size, active->GetTotalBitCount());
      XLS_ASSIGN_OR_RETURN(int64_t active_bits, active_size.GetAsInt64());
      // Only active payload bits participate. Inactive high padding remains
      // part of the raw domain and may cause repeated canonical executions.
      return scan_members(*active, bit_offset + active_bits);
    }
  } else if (auto* tuple = dynamic_cast<const TupleType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(TypeDim size, tuple->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t bits, size.GetAsInt64());
    return scan_members(*tuple, bit_offset + bits);
  } else if (auto* structure = dynamic_cast<const StructTypeBase*>(&type)) {
    XLS_ASSIGN_OR_RETURN(TypeDim size, structure->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t bits, size.GetAsInt64());
    return scan_members(*structure, bit_offset + bits);
  } else if (auto* array = dynamic_cast<const ArrayType*>(&type)) {
    XLS_ASSIGN_OR_RETURN(int64_t count, array->size().GetAsInt64());
    XLS_ASSIGN_OR_RETURN(TypeDim size,
                         array->element_type().GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t element_bits, size.GetAsInt64());
    // Walk physical chunks from most to least significant. IR arrays and
    // arrays packed into sum payloads reverse logical element order, but every
    // chunk has the same element type, so this search is valid for both.
    for (int64_t i = count; i-- > 0;) {
      XLS_ASSIGN_OR_RETURN(
          uint64_t next, SkipInvalidSumTagRange(array->element_type(), index,
                                                bit_offset + i * element_bits));
      if (next != index) {
        return next;
      }
    }
    return index;
  } else {
    return index;
  }
}

absl::StatusOr<QuickCheckResults> DoQuickCheck(
    bool requires_implicit_token, dslx::FunctionType* dslx_fn_type,
    xls::Function* ir_function, std::string_view ir_name,
    AbstractRunComparator* quickcheck_runner, int64_t seed,
    QuickCheckTestCases test_cases) {
  QuickCheckResults results;
  std::minstd_rand rng_engine(seed);
  absl::BitGenRef bit_gen(rng_engine);
  xls::TupleType* ir_param_tuple = ir_function->package()->GetTupleType(
      ir_function->GetType()->parameters());
  xls::TupleType* source_ir_param_tuple = ir_param_tuple;
  if (requires_implicit_token) {
    XLS_RET_CHECK_GE(ir_param_tuple->size(), 2);
    source_ir_param_tuple = ir_function->package()->GetTupleType(
        ir_function->GetType()->parameters().subspan(2));
  }

  int64_t num_tests;
  switch (test_cases.tag()) {
    case QuickCheckTestCasesTag::kExhaustive: {
      int64_t parameter_bit_count = source_ir_param_tuple->GetFlatBitCount();
      if (parameter_bit_count > 48) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Cannot run an exhaustive quickcheck for `%s` "
                            "because it has too large a parameter bit count; "
                            "got: %d",
                            ir_function->name(), parameter_bit_count));
      }
      num_tests = int64_t{1} << parameter_bit_count;
      break;
    }
    case QuickCheckTestCasesTag::kCounted:
      num_tests =
          test_cases.count().value_or(QuickCheckTestCases::kDefaultTestCount);
      break;
  }

  std::vector<const Type*> dslx_param_types;
  dslx_param_types.reserve(dslx_fn_type->params().size());
  for (const std::unique_ptr<Type>& type : dslx_fn_type->params()) {
    dslx_param_types.push_back(type.get());
  }
  const int64_t source_arg_offset = requires_implicit_token ? 2 : 0;
  // Every bit pattern is valid unless the source signature contains an enum or
  // semantic sum, including those nested in aggregates. Ordinary exhaustive
  // inputs need source reconstruction only if they become counterexamples.
  const bool skip_invalid_sum_ranges =
      test_cases.tag() == QuickCheckTestCasesTag::kExhaustive &&
      std::any_of(
          dslx_param_types.begin(), dslx_param_types.end(),
          [](const Type* type) { return TypeContainsSemanticSum(*type); });
  const bool use_direct_ir_inputs =
      test_cases.tag() == QuickCheckTestCasesTag::kExhaustive &&
      !skip_invalid_sum_ranges &&
      std::none_of(dslx_param_types.begin(), dslx_param_types.end(),
                   [](const Type* type) { return type->HasEnum(); });

  // Check that, after we've accounted for the potential implicit-token calling
  // convention, the number of IR function parameters and DSLX parameters line
  // up.
  XLS_RET_CHECK_EQ(ir_param_tuple->size(),
                   dslx_param_types.size() + source_arg_offset)
      << "IR param tuple size should match DSLX param types size";

  InterpValueGenerator value_generator;
  for (int64_t i = 0; i < num_tests; i++) {
    std::vector<Value> arg_set;
    std::vector<InterpValue> dslx_arg_set;
    if (test_cases.tag() == QuickCheckTestCasesTag::kCounted) {
      XLS_ASSIGN_OR_RETURN(dslx_arg_set, value_generator.GenerateValues(
                                             bit_gen, dslx_param_types));
      arg_set.reserve(source_arg_offset + dslx_arg_set.size());
      if (requires_implicit_token) {
        arg_set.push_back(Value::Token());
        arg_set.push_back(Value::Bool(true));
      }
      for (const InterpValue& source_value : dslx_arg_set) {
        XLS_ASSIGN_OR_RETURN(Value ir_value, source_value.ConvertToIr());
        arg_set.push_back(std::move(ir_value));
      }
    } else {
      arg_set = MakeFromUint64(source_ir_param_tuple, i);
      if (requires_implicit_token) {
        // The activation bit is a calling convention, not a source input to
        // enumerate. Every exhaustive case must execute enabled side effects.
        arg_set.insert(arg_set.begin(), {Value::Token(), Value::Bool(true)});
      }
      if (!use_direct_ir_inputs) {
        dslx_arg_set.reserve(dslx_param_types.size());
        bool is_valid_source_input = true;
        for (int64_t arg_index = 0; arg_index < dslx_param_types.size();
             ++arg_index) {
          Value& ir_value = arg_set.at(source_arg_offset + arg_index);
          const Type& source_type = *dslx_param_types.at(arg_index);
          absl::StatusOr<InterpValue> source_value =
              ValueToInterpValue(ir_value, &source_type);
          if (!source_value.ok()) {
            if (!absl::IsInvalidArgument(source_value.status()) &&
                !absl::IsNotFound(source_value.status())) {
              return source_value.status();
            }
            is_valid_source_input = false;
            break;
          }
          absl::Status validation_status =
              ValidateInterpValueMatchesType(*source_value, source_type);
          if (!validation_status.ok()) {
            if (!absl::IsInvalidArgument(validation_status) &&
                !absl::IsNotFound(validation_status)) {
              return validation_status;
            }
            is_valid_source_input = false;
            break;
          }
          XLS_ASSIGN_OR_RETURN(
              InterpValue constructed,
              ConstructCanonicalQuickCheckInput(source_type, *source_value));
          XLS_ASSIGN_OR_RETURN(ir_value, constructed.ConvertToIr());
          dslx_arg_set.push_back(std::move(constructed));
        }
        if (!is_valid_source_input) {
          if (skip_invalid_sum_ranges) {
            int64_t upper_bit = source_ir_param_tuple->GetFlatBitCount();
            for (const Type* source_type : dslx_param_types) {
              XLS_ASSIGN_OR_RETURN(TypeDim size,
                                   source_type->GetTotalBitCount());
              XLS_ASSIGN_OR_RETURN(int64_t bits, size.GetAsInt64());
              upper_bit -= bits;
              XLS_ASSIGN_OR_RETURN(
                  uint64_t next,
                  SkipInvalidSumTagRange(*source_type, i, upper_bit));
              if (next != i) {
                // The loop increment reaches the first not-yet-excluded raw
                // candidate, including the one-past-domain sentinel.
                i = static_cast<int64_t>(next) - 1;
                break;
              }
            }
          }
          continue;
        }
      }
    }
    results.arg_sets.push_back(std::move(arg_set));

    // TODO(https://github.com/google/xls/issues/506): 2021-10-15
    // Assertion failures should work out, but we should consciously decide
    // if/how we want to dump traces when running QuickChecks (always, for
    // failures, flag-controlled, ...).
    absl::Span<const Value> this_arg_set = results.arg_sets.back();
    XLS_ASSIGN_OR_RETURN(xls::Value result,
                         DropInterpreterEvents(quickcheck_runner->RunIrFunction(
                             ir_name, ir_function, this_arg_set)));

    // In the case of an implicit token signature we get (token, bool) as the
    // result of the quickcheck'd function, so we unbox the boolean here.
    if (result.IsTuple()) {
      result = result.elements()[1];
      XLS_RET_CHECK(result.IsBits());
    }

    XLS_RET_CHECK(result.IsBits())
        << "quickcheck properties must return `bool`, should be validated by "
           "type checking; got: "
        << result;

    results.results.push_back(result);

    if (result.IsAllZeros()) {
      if (use_direct_ir_inputs) {
        dslx_arg_set.reserve(dslx_param_types.size());
        for (int64_t arg_index = 0; arg_index < dslx_param_types.size();
             ++arg_index) {
          XLS_ASSIGN_OR_RETURN(
              InterpValue source_value,
              ValueToInterpValue(this_arg_set[source_arg_offset + arg_index],
                                 dslx_param_types.at(arg_index)));
          dslx_arg_set.push_back(std::move(source_value));
        }
      }
      results.falsifying_dslx_arg_set = std::move(dslx_arg_set);
      // We were able to falsify the xls_function (predicate), bail out early
      // and present this evidence.
      break;
    }
  }

  return results;
}

struct QuickcheckIrFn {
  std::string ir_name;
  xls::Function* ir_function;
  CallingConvention calling_convention;
};

static absl::StatusOr<QuickcheckIrFn> FindQuickcheckIrFn(Function* dslx_fn,
                                                         Package* ir_package) {
  // First we try to get the version of the function that doesn't need a token.
  XLS_ASSIGN_OR_RETURN(
      std::string ir_name,
      MangleDslxName(dslx_fn->owner()->name(), dslx_fn->identifier(),
                     CallingConvention::kTypical,
                     dslx_fn->GetFreeParametricKeySet()));
  std::optional<xls::Function*> maybe_ir_function =
      ir_package->TryGetFunction(ir_name);
  if (maybe_ir_function.has_value()) {
    return QuickcheckIrFn{ir_name, maybe_ir_function.value(),
                          CallingConvention::kTypical};
  }

  XLS_ASSIGN_OR_RETURN(
      ir_name, MangleDslxName(dslx_fn->owner()->name(), dslx_fn->identifier(),
                              CallingConvention::kImplicitToken,
                              dslx_fn->GetFreeParametricKeySet()));
  maybe_ir_function = ir_package->TryGetFunction(ir_name);
  if (maybe_ir_function.has_value()) {
    return QuickcheckIrFn{ir_name, maybe_ir_function.value(),
                          CallingConvention::kImplicitToken};
  }
  return absl::InternalError(
      absl::StrFormat("Could not find DSLX quickcheck function `%s` in IR "
                      "package `%s`; available IR functions: [%s]",
                      dslx_fn->identifier(), ir_package->name(),
                      absl::StrJoin(ir_package->GetFunctionNames(), ", ")));
}

static absl::StatusOr<bool> QuickCheckHasInhabitedInputs(QuickCheck* quickcheck,
                                                         TypeInfo* type_info) {
  XLS_ASSIGN_OR_RETURN(
      dslx::FunctionType * dslx_fn_type,
      type_info->GetItemAs<dslx::FunctionType>(quickcheck->fn()));
  for (const std::unique_ptr<Type>& param_type : dslx_fn_type->params()) {
    XLS_ASSIGN_OR_RETURN(bool param_is_inhabited, TypeIsInhabited(*param_type));
    if (!param_is_inhabited) {
      return false;
    }
  }
  return true;
}

static absl::StatusOr<bool> HasUninhabitedQuickCheck(Module* entry_module,
                                                     TypeInfo* type_info,
                                                     const RE2* test_filter) {
  for (QuickCheck* quickcheck : entry_module->GetQuickChecks()) {
    if (TestMatchesFilter(quickcheck->identifier(), test_filter)) {
      XLS_ASSIGN_OR_RETURN(bool has_inhabited_inputs,
                           QuickCheckHasInhabitedInputs(quickcheck, type_info));
      if (!has_inhabited_inputs) {
        return true;
      }
    }
  }
  return false;
}

static absl::Status RunQuickCheck(AbstractRunComparator* quickcheck_runner,
                                  Package* ir_package, QuickCheck* quickcheck,
                                  TypeInfo* type_info, ImportData* import_data,
                                  const ConvertOptions& convert_options,
                                  int64_t seed) {
  // Note: DSLX function.
  dslx::Function* dslx_fn = quickcheck->fn();

  XLS_ASSIGN_OR_RETURN(dslx::FunctionType * dslx_fn_type,
                       type_info->GetItemAs<dslx::FunctionType>(dslx_fn));

  // Validate the return type is a bool, we rely on that assumption here.
  const Type& return_type = dslx_fn_type->return_type();
  std::optional<BitsLikeProperties> bits_like_properties =
      GetBitsLike(return_type);
  XLS_RET_CHECK(bits_like_properties.has_value())
      << "quickcheck properties must return `bool`, should be validated by "
         "type checking";
  XLS_RET_CHECK(IsKnownU1(bits_like_properties.value()))
      << "quickcheck properties must return `bool`, should be validated by "
         "type checking";

  XLS_ASSIGN_OR_RETURN(bool has_inhabited_inputs,
                       QuickCheckHasInhabitedInputs(quickcheck, type_info));
  if (!has_inhabited_inputs) {
    return FailureErrorStatus(
        dslx_fn->span(),
        absl::StrFormat("quickcheck of `%s` rejected all input samples",
                        dslx_fn->identifier()),
        *dslx_fn->owner()->file_table());
  }

  std::unique_ptr<Package> local_package;
  if (ir_package == nullptr) {
    dslx::PackageConversionData conv{
        .package = std::make_unique<Package>(dslx_fn->owner()->name())};
    XLS_RETURN_IF_ERROR(ConvertOneFunctionIntoPackage(
        dslx_fn, import_data,
        /*parametric_env=*/nullptr, convert_options, &conv));
    local_package = std::move(conv.package);
    ir_package = local_package.get();
  }
  XLS_ASSIGN_OR_RETURN(QuickcheckIrFn qc_fn,
                       FindQuickcheckIrFn(dslx_fn, ir_package));

  XLS_ASSIGN_OR_RETURN(
      QuickCheckResults qc_results,
      DoQuickCheck(
          qc_fn.calling_convention == CallingConvention::kImplicitToken,
          dslx_fn_type, qc_fn.ir_function, qc_fn.ir_name, quickcheck_runner,
          seed, quickcheck->test_cases()));

  // Extract the (inputs, outputs) from the results.
  const std::vector<std::vector<Value>>& inputs = qc_results.arg_sets;
  const std::vector<Value>& outputs = qc_results.results;
  XLS_RET_CHECK(inputs.size() == outputs.size())
      << "inputs and outputs must have the same size";

  if (outputs.empty()) {
    // If we have a value like an empty enum we'll reject all samples, so we
    // want to make a reasonable error message for that case.
    return FailureErrorStatus(
        dslx_fn->span(),
        absl::StrFormat("quickcheck of `%s` rejected all input samples",
                        dslx_fn->identifier()),
        *dslx_fn->owner()->file_table());
  }

  XLS_ASSIGN_OR_RETURN(Bits last_result, outputs.back().GetBitsWithStatus());
  if (!last_result.IsZero()) {
    // Did not find a falsifying example.
    return absl::OkStatus();
  }

  const std::vector<std::unique_ptr<Type>>& dslx_params =
      dslx_fn_type->params();
  XLS_RET_CHECK(qc_results.falsifying_dslx_arg_set.has_value())
      << "falsifying source inputs must be retained";
  const std::vector<InterpValue>& dslx_argset =
      *qc_results.falsifying_dslx_arg_set;
  XLS_RET_CHECK_EQ(dslx_argset.size(), dslx_params.size());

  std::string dslx_argset_str = absl::StrJoin(
      dslx_argset, ", ", [](std::string* out, const InterpValue& v) {
        absl::StrAppend(out, v.ToString());
      });
  return FailureErrorStatus(
      dslx_fn->span(),
      absl::StrFormat("Found falsifying example after %d tests: [%s]",
                      outputs.size(), dslx_argset_str),
      *dslx_fn->owner()->file_table());
}

static absl::Status RunQuickChecksIfEnabled(
    const RE2* test_filter, Module* entry_module, TypeInfo* type_info,
    AbstractRunComparator* quickcheck_runner, Package* ir_package,
    ImportData* import_data, const ConvertOptions& convert_options,
    std::optional<int64_t> seed, TestResultData& result,
    VirtualizableFilesystem& vfs) {
  if (quickcheck_runner == nullptr) {
    // TODO(leary): 2024-02-08 Note that this skips /all/ the quickchecks so we
    // don't make an entry for it right now in the test XML.
    std::cerr << "[ SKIPPING QUICKCHECKS  ] (JIT is disabled and quickcheck is "
                 "not enabled with the interpreter)"
              << "\n";
    return absl::OkStatus();
  }
  if (!seed.has_value()) {
    // Note: we *want* to *provide* non-determinism by default. See
    // https://abseil.io/docs/cpp/guides/random#stability-of-generated-sequences
    // for rationale.
    seed = static_cast<int64_t>(getpid()) * static_cast<int64_t>(time(nullptr));
  }
  FileTable& file_table = *entry_module->file_table();
  bool any_quicktest_run = false;
  for (QuickCheck* quickcheck : entry_module->GetQuickChecks()) {
    const std::string& quickcheck_name = quickcheck->identifier();
    const Pos& start_pos = quickcheck->span().start();
    const absl::Time test_case_start = absl::Now();
    if (!TestMatchesFilter(quickcheck_name, test_filter)) {
      auto test_case_end = absl::Now();
      result.AddTestCase(test_xml::TestCase{
          .name = quickcheck_name,
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kFiltered,
          .time = test_case_end - test_case_start,
          .timestamp = test_case_start});
      continue;
    }

    if (!any_quicktest_run) {
      // Only print the SEED if there is actually a test that is executed.
      std::cerr << absl::StreamFormat("[ SEED %*d ]\n", kQuickcheckSpaces + 1,
                                      *seed);
      any_quicktest_run = true;
    }
    std::cerr << "[ RUN QUICKCHECK        ] " << quickcheck_name
              << " cases: " << quickcheck->test_cases().ToString() << "\n";
    const absl::Status status =
        RunQuickCheck(quickcheck_runner, ir_package, quickcheck, type_info,
                      import_data, convert_options, *seed);
    const absl::Duration duration = absl::Now() - test_case_start;
    if (!status.ok()) {
      HandleError(result, status, quickcheck_name, start_pos, test_case_start,
                  duration, /*is_quickcheck=*/true, file_table, vfs);
    } else {
      result.AddTestCase(test_xml::TestCase{
          .name = quickcheck_name,
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kCompleted,
          .time = duration,
          .timestamp = test_case_start});
      std::cerr << "[                    OK ] " << quickcheck_name << "\n";
    }
  }
  std::cerr << absl::StreamFormat(
                   "[=======================] %d quickcheck(s) ran.",
                   entry_module->GetQuickChecks().size())
            << "\n";
  return absl::OkStatus();
}

absl::StatusOr<ParseAndProveResult> ParseAndProve(
    std::string_view program, std::string_view module_name,
    std::string_view filename, const ParseAndProveOptions& options) {
  const absl::Time parse_and_prove_start = absl::Now();
  TestResultData result(parse_and_prove_start, /*test_cases=*/{});

  std::unique_ptr<VirtualizableFilesystem> vfs;
  if (options.vfs_factory != nullptr) {
    vfs = options.vfs_factory();
  } else {
    vfs = std::make_unique<RealFilesystem>();
  }
  const ParseAndTypecheckOptions& parse_and_typecheck_options =
      options.parse_and_typecheck_options;

  auto import_data =
      CreateImportData(parse_and_typecheck_options.dslx_stdlib_path,
                       parse_and_typecheck_options.dslx_paths,
                       parse_and_typecheck_options.warnings, std::move(vfs));
  FileTable& file_table = import_data.file_table();
  absl::StatusOr<TypecheckedModule> tm =
      ParseAndTypecheck(program, filename, module_name, &import_data);
  if (!tm.ok()) {
    if (TryPrintError(tm.status(), file_table, import_data.vfs())) {
      result.Finish(TestResult::kParseOrTypecheckError,
                    absl::Now() - parse_and_prove_start);
      return ParseAndProveResult{.test_result_data = result};
    }
    return tm.status();
  }

  // If we're not executing, then we're just scanning for errors -- if warnings
  // are *not* errors, just elide printing them (or e.g. we'd show warnings for
  // files that had warnings suppressed at build time, which would gunk up build
  // logs unnecessarily.).
  if (parse_and_typecheck_options.warnings_as_errors) {
    PrintWarnings(tm->warnings, file_table, import_data.vfs());
  }

  if (parse_and_typecheck_options.warnings_as_errors &&
      !tm->warnings.warnings().empty()) {
    result.Finish(TestResult::kFailedWarnings,
                  absl::Now() - parse_and_prove_start);
    return ParseAndProveResult{.test_result_data = result};
  }

  Module* entry_module = tm->module;

  // We need to IR-convert the quickcheck property and then try to prove that
  // the return value is always true.
  absl::flat_hash_map<std::string, QuickCheck*> qcs =
      entry_module->GetQuickCheckByName();

  // Counter-examples map from failing test name -> counterexample values.
  absl::flat_hash_map<std::string, std::vector<Value>> counterexamples;

  for (const std::string& quickcheck_name :
       entry_module->GetQuickCheckNames()) {
    QuickCheck* quickcheck = qcs.at(quickcheck_name);
    const Pos& start_pos = quickcheck->span().start();
    Function* f = quickcheck->fn();
    VLOG(1) << "Found quickcheck function: " << f->identifier();

    auto test_case_start = absl::Now();

    if (!TestMatchesFilter(quickcheck_name, options.test_filter)) {
      auto test_case_end = absl::Now();
      result.AddTestCase(test_xml::TestCase{
          .name = quickcheck_name,
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kFiltered,
          .time = test_case_end - test_case_start,
          .timestamp = test_case_start});
      continue;
    }
    std::cerr << "[ RUN QUICKCHECK        ] " << quickcheck_name << '\n';
    dslx::PackageConversionData conv{
        .package = std::make_unique<Package>(entry_module->name())};
    Package& package = *conv.package;

    // Helper that prevents us from mishandling various errors in the routine
    // via single use point.
    auto handle_if_error = [&](absl::Status status) {
      if (status.ok()) {
        return false;
      }
      HandleError(result, status, quickcheck_name, start_pos, test_case_start,
                  absl::Now() - test_case_start, /*is_quickcheck=*/true,
                  file_table, import_data.vfs());
      return true;
    };

    const absl::Status convert_status = ConvertOneFunctionIntoPackage(
        f, &import_data,
        /*parametric_env=*/nullptr, ConvertOptions{}, &conv);
    if (handle_if_error(convert_status)) {
      continue;
    }

    // Note: we need this to eliminate unoptimized IR constructs that are not
    // currently handled for translation; e.g. bounded-for-loops, asserts and
    // non-inlined function calls.
    auto pipeline = CreateOptimizationPassPipeline();

    // By the time this pass executes only the single top function is left (and
    // non-synth function).
    // Strip any remaining asserts from it and 'and' them to the quickcheck
    // goal.
    pipeline->Add<InliningPass>();
    pipeline->Add<DeadFunctionEliminationPass>();
    pipeline->Add<DeadCodeEliminationPass>();
    pipeline->Add<QuickCheckProveAssertsNotFiredPass>();
    pipeline->Add<DeadCodeEliminationPass>();
    PassResults results;
    OptimizationContext ctx;
    const absl::Status opt_status =
        pipeline->Run(conv.package.get(), {}, &results, ctx).status();
    if (handle_if_error(opt_status)) {
      continue;
    }

    const absl::StatusOr<std::string> ir_function_name = MangleDslxName(
        entry_module->name(), f->identifier(), CallingConvention::kTypical);
    if (handle_if_error(ir_function_name.status())) {
      continue;
    }

    const absl::StatusOr<xls::Function*> ir_function =
        package.GetFunction(*ir_function_name);
    if (handle_if_error(ir_function.status())) {
      continue;
    }

    VLOG(1) << "Found IR function: " << (*ir_function)->name();

    absl::StatusOr<solvers::ProverResult> proven = solvers::z3::TryProve(
        *ir_function, (*ir_function)->return_value(),
        solvers::Predicate::NotEqualToZero(), absl::InfiniteDuration());

    if (handle_if_error(proven.status())) {
      continue;
    }

    VLOG(1) << "Proven? "
            << (std::holds_alternative<solvers::ProvenTrue>(*proven) ? "true"
                                                                     : "false");

    if (std::holds_alternative<solvers::ProvenTrue>(*proven)) {
      absl::Time test_case_end = absl::Now();
      absl::Duration duration = test_case_end - test_case_start;
      result.AddTestCase(test_xml::TestCase{
          .name = std::string(quickcheck_name),
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kCompleted,
          .time = duration,
          .timestamp = test_case_start,
      });
      std::cerr << "[                    OK ] " << quickcheck_name << "\n";
      continue;
    }

    const auto& proven_false = std::get<solvers::ProvenFalse>(*proven);

    // Extract the counterexample, and collapse it back into sequential order.
    std::vector<Value> counterexample;
    using InputValues = absl::flat_hash_map<xls::Node*, Value>;
    XLS_ASSIGN_OR_RETURN(InputValues counterexample_map,
                         proven_false.counterexample);
    for (const xls::Param* param : (*ir_function)->params()) {
      auto it = counterexample_map.find(param);
      if (it == counterexample_map.end()) {
        counterexample.push_back(ZeroOfType(param->GetType()));
      }
      counterexample.push_back(it->second);
    }
    std::string one_liner =
        absl::StrCat("counterexample: ", absl::StrJoin(counterexample, ", "));
    const absl::Status proof_error_status =
        ProofErrorStatus(quickcheck->span(), one_liner, file_table);
    counterexamples[quickcheck_name] = std::move(counterexample);

    if (handle_if_error(proof_error_status)) {
      continue;
    }
  }

  result.Finish(TestResult::kSomeFailed, absl::Now() - parse_and_prove_start);
  std::cerr
      << absl::StreamFormat(
             "[=======================] %d test(s) ran; %d failed; %d skipped.",
             result.GetRanCount(), result.GetFailedCount(),
             result.GetSkippedCount())
      << '\n';

  result.Finish(
      result.DidAnyFail() ? TestResult::kSomeFailed : TestResult::kAllPassed,
      absl::Now() - parse_and_prove_start);
  return ParseAndProveResult{.test_result_data = std::move(result),
                             .counterexamples = std::move(counterexamples)};
}

absl::StatusOr<TestResultData> AbstractTestRunner::ParseAndTest(
    std::string_view program, std::string_view module_name,
    std::string_view filename, const ParseAndTestOptions& options) const {
  const absl::Time start = absl::Now();
  TestResultData result(start, /*test_cases=*/{});

  std::unique_ptr<VirtualizableFilesystem> vfs;
  if (options.vfs_factory != nullptr) {
    vfs = options.vfs_factory();
  } else {
    vfs = std::make_unique<RealFilesystem>();
  }
  const ParseAndTypecheckOptions& parse_and_typecheck_options =
      options.parse_and_typecheck_options;
  auto import_data =
      CreateImportData(parse_and_typecheck_options.dslx_stdlib_path,
                       parse_and_typecheck_options.dslx_paths,
                       parse_and_typecheck_options.warnings, std::move(vfs));
  FileTable& file_table = import_data.file_table();

  absl::StatusOr<TypecheckedModule> tm = ParseAndTypecheck(
      program, filename, module_name, &import_data, nullptr,
      ConvertOptions{.configured_values =
                         parse_and_typecheck_options.configured_values});
  if (!tm.ok()) {
    if (TryPrintError(tm.status(), import_data.file_table(),
                      import_data.vfs())) {
      result.Finish(TestResult::kParseOrTypecheckError, absl::Now() - start);
      return result;
    }
    return tm.status();
  }

  // If we're not executing, then we're just scanning for errors -- if warnings
  // are *not* errors, just elide printing them (or e.g. we'd show warnings for
  // files that had warnings suppressed at build time, which would gunk up build
  // logs unnecessarily.).
  if (options.execute || parse_and_typecheck_options.warnings_as_errors) {
    PrintWarnings(tm->warnings, import_data.file_table(), import_data.vfs());
  }

  if (parse_and_typecheck_options.warnings_as_errors &&
      !tm->warnings.warnings().empty()) {
    result.Finish(TestResult::kFailedWarnings, absl::Now() - start);
    return result;
  }

  // If not executing tests and quickchecks, then return vacuous success.
  if (!options.execute) {
    result.Finish(TestResult::kAllPassed, absl::Now() - start);
    return result;
  }

  Module* entry_module = tm->module;

  // If JIT comparisons are "on", we register a post-evaluation hook to compare
  // with the interpreter.
  std::unique_ptr<Package> ir_package;
  PostFnEvalHook post_fn_eval_hook;
  XLS_ASSIGN_OR_RETURN(bool has_uninhabited_quickcheck,
                       HasUninhabitedQuickCheck(entry_module, tm->type_info,
                                                options.test_filter));
  if (options.run_comparator != nullptr ||
      (options.quickcheck_runner != nullptr && !has_uninhabited_quickcheck)) {
    absl::StatusOr<dslx::PackageConversionData> ir_package_conversion_data =
        ConvertModuleToPackage(entry_module, &import_data,
                               options.convert_options);
    if (!ir_package_conversion_data.ok()) {
      if (TryPrintError(ir_package_conversion_data.status(),
                        import_data.file_table(), import_data.vfs())) {
        result.Finish(TestResult::kSomeFailed, absl::Now() - start);
        return result;
      }
      return absl::StatusBuilder(ir_package_conversion_data.status())
             << "Failed to convert input to IR for comparison. Consider "
                "turning off comparison with `--compare=none`: ";
    }
    ir_package = (*std::move(ir_package_conversion_data)).package;
    if (options.run_comparator != nullptr) {
      post_fn_eval_hook = [&ir_package, &import_data, &options](
                              const Function* f,
                              absl::Span<const InterpValue> args,
                              const ParametricEnv& parametric_env,
                              const InterpValue& got) -> absl::Status {
        XLS_RET_CHECK(f != nullptr);

        bool requires_implicit_token =
            GetRequiresImplicitToken(*f, &import_data, options.convert_options);
        return options.run_comparator->RunComparison(ir_package.get(),
                                                     requires_implicit_token, f,
                                                     args, parametric_env, got);
      };
    }
  }

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<AbstractParsedTestRunner> runner,
                       CreateTestRunner(&import_data, tm->type_info,
                                        entry_module, options.convert_options));
  // Run unit tests.
  for (const std::string& test_name : entry_module->GetTestNames()) {
    auto test_case_start = absl::Now();
    ModuleMember* member = entry_module->FindMemberWithName(test_name).value();
    const Pos start_pos = GetPos(*member);

    if (!TestMatchesFilter(test_name, options.test_filter)) {
      auto test_case_end = absl::Now();
      result.AddTestCase(test_xml::TestCase{
          .name = test_name,
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kFiltered,
          .time = test_case_end - test_case_start,
          .timestamp = test_case_start});
      continue;
    }

    std::cerr << "[ RUN UNITTEST  ] " << test_name << '\n';
    RunResult out;
    BytecodeInterpreterOptions interpreter_options;

    // If requested, create a result entry and capture trace messages.
    xls::EvaluatorResultProto* result_proto = nullptr;
    if (options.results_out != nullptr) {
      result_proto = options.results_out->add_results();
    }

    // Create an events collector for this specific test run.
    InfoLoggingDslxInterpreterEvents test_events;

    interpreter_options.post_fn_eval_hook(post_fn_eval_hook)
        .trace_channels(options.trace_channels)
        .trace_calls(options.trace_calls)
        .max_ticks(options.max_ticks)
        .format_preference(options.format_preference);
    if (std::holds_alternative<TestFunction*>(*member)) {
      XLS_ASSIGN_OR_RETURN(
          out, runner->RunTestFunction(test_name, interpreter_options,
                                       /*events=*/&test_events));
    } else {
      if (options.results_out != nullptr) {
        return absl::UnimplementedError(
            "Collecting EvaluatorResultsProto for proc tests is not yet "
            "implemented");
      }
      XLS_ASSIGN_OR_RETURN(out,
                           runner->RunTestProc(test_name, interpreter_options));
    }
    auto test_case_end = absl::Now();

    // If collecting results, copy the events into the result proto for this
    // test invocation.
    if (result_proto != nullptr) {
      *result_proto->mutable_events() = test_events.AsProto();
    }

    if (out.result.ok()) {
      // Add to the tracking data.
      result.AddTestCase(test_xml::TestCase{
          .name = test_name,
          .file = std::string{start_pos.GetFilename(file_table)},
          .line = start_pos.GetHumanLineno(),
          .status = test_xml::RunStatus::kRun,
          .result = test_xml::RunResult::kCompleted,
          .time = test_case_end - test_case_start,
          .timestamp = test_case_start});
      std::cerr << "[            OK ]" << '\n';
    } else {
      if (result_proto != nullptr) {
        xls::AssertMessageProto* am =
            result_proto->mutable_events()->add_assert_msgs();
        am->set_message(std::string(out.result.message()));
      }
      HandleError(result, out.result, test_name, start_pos, test_case_start,
                  test_case_end - test_case_start,
                  /*is_quickcheck=*/false, file_table, import_data.vfs());
    }
  }

  std::cerr << absl::StreamFormat(
                   "[===============] %d test(s) ran; %d failed; %d skipped.",
                   result.GetRanCount(), result.GetFailedCount(),
                   result.GetSkippedCount())
            << '\n';

  // Run quickchecks, but only if the JIT is enabled.
  if (!entry_module->GetQuickChecks().empty()) {
    XLS_RETURN_IF_ERROR(RunQuickChecksIfEnabled(
        options.test_filter, entry_module, tm->type_info,
        options.quickcheck_runner, ir_package.get(), &import_data,
        options.convert_options, options.seed, result, import_data.vfs()));
  }

  result.Finish(
      result.DidAnyFail() ? TestResult::kSomeFailed : TestResult::kAllPassed,
      absl::Now() - start);
  return result;
}

std::string_view TestResultToString(TestResult tr) {
  switch (tr) {
    case TestResult::kFailedWarnings:
      return "failed-warnings";
    case TestResult::kSomeFailed:
      return "some-failed";
    case TestResult::kAllPassed:
      return "all-passed";
    case TestResult::kParseOrTypecheckError:
      return "parse-or-typecheck-error";
  }
  LOG(FATAL) << "Invalid test result value: " << static_cast<int>(tr);
}

}  // namespace xls::dslx
