#include "xls/codegen/block_generator.h"

#include <string>

#include "gtest/gtest.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/verilog_line_map.pb.h"
#include "xls/common/golden_files.h"
#include "xls/common/file/temp_file.h"
#include "xls/common/subprocess.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/ir_test_base.h"

namespace xls::verilog {

namespace {

absl::StatusOr<std::string> MakeUnifiedDiff(const std::string& base_text,
                                            const std::string& guided_text,
                                            int context = 8) {
  XLS_ASSIGN_OR_RETURN(auto base_file,
                       xls::TempFile::CreateWithContent(base_text, ".sv"));
  XLS_ASSIGN_OR_RETURN(auto guid_file,
                       xls::TempFile::CreateWithContent(guided_text, ".sv"));
  std::string ctx = absl::StrFormat("-U%d", context);
  std::vector<std::string> argv = {"diff", ctx, "-L", "base", "-L",
                                   "guided", base_file.path().string(),
                                   guid_file.path().string()};
  XLS_ASSIGN_OR_RETURN(auto res, xls::InvokeSubprocess(argv));
  // diff returns exit code 1 when files differ; still capture stdout.
  // If diff is not found or crashed, normal_termination will be false.
  if (!res.normal_termination) {
    return absl::InternalError("diff subprocess did not terminate normally");
  }
  return res.stdout_content;
}

}  // namespace

class ResidualGuidanceGoldenDiffTest : public IrTestBase {};

TEST_F(ResidualGuidanceGoldenDiffTest, SmallEditGuidedGoldenDiff) {
  auto p = CreatePackage();
  BlockBuilder bb("B", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = bb.InputPort("x", u8);
  BValue y = bb.InputPort("y", u8);
  BValue a = bb.And(x, x);
  BValue b = bb.Or(y, y);
  BValue r = bb.Add(a, b);
  bb.OutputPort("out", r);
  ASSERT_TRUE(bb.Build().ok());
  Block* block = p->GetBlock("B").value();

  // Base residual and text.
  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block, base, nullptr, &base_residual).ok());
  std::string verilog_base = GenerateVerilog(block, base, nullptr, nullptr).value();

  // Minimal edit: route 'a' through an add-zero before out.
  ::xls::Node* a_node = a.node();
  absl::StatusOr<::xls::Node*> zero_or =
      block->MakeNode<::xls::Literal>(SourceInfo(), Value(UBits(0, 8)));
  ASSERT_TRUE(zero_or.ok()) << zero_or.status();
  ::xls::Node* zero = zero_or.value();
  absl::StatusOr<::xls::Node*> a_plus_zero_or =
      block->MakeNode<::xls::BinOp>(SourceInfo(), a_node, zero, Op::kAdd);
  ASSERT_TRUE(a_plus_zero_or.ok()) << a_plus_zero_or.status();
  ::xls::Node* a_plus_zero = a_plus_zero_or.value();
  OutputPort* out = nullptr;
  for (Node* n : block->nodes()) {
    if (n->Is<OutputPort>()) { out = n->As<OutputPort>(); break; }
  }
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(out->ReplaceOperandNumber(0, a_plus_zero).ok());

  // Guided codegen using prior residual.
  CodegenOptions guided;
  guided.module_name("B_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog_guided = GenerateVerilog(block, guided, nullptr, nullptr).value();

  // Unified diff for small edit should match golden.
  std::string diff_text = MakeUnifiedDiff(verilog_base, verilog_guided, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_golden_diff_small_edit.txt",
      diff_text);
}

TEST_F(ResidualGuidanceGoldenDiffTest, CommutativeReorderGoldenDiff) {
  auto p = CreatePackage();
  BlockBuilder bb("B", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = bb.InputPort("x", u8);
  BValue y = bb.InputPort("y", u8);
  BValue a = bb.And(x, x);
  BValue b = bb.Or(y, y);
  BValue r = bb.Add(a, b);
  bb.OutputPort("out", r);
  ASSERT_TRUE(bb.Build().ok());
  Block* block = p->GetBlock("B").value();

  // Base residual and text.
  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block, base, nullptr, &base_residual).ok());
  std::string verilog_base = GenerateVerilog(block, base, nullptr, nullptr).value();

  // Swap operands of the add (commutative reorder).
  ::xls::Node* add_node = r.node();
  ASSERT_EQ(add_node->op(), Op::kAdd);
  ASSERT_TRUE(add_node->ReplaceOperandNumber(0, b.node()).ok());
  ASSERT_TRUE(add_node->ReplaceOperandNumber(1, a.node()).ok());

  // Guided codegen using prior residual.
  CodegenOptions guided;
  guided.module_name("B_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog_guided = GenerateVerilog(block, guided, nullptr, nullptr).value();

  std::string diff_text = MakeUnifiedDiff(verilog_base, verilog_guided, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_golden_diff_commutative.txt",
      diff_text);
}

TEST_F(ResidualGuidanceGoldenDiffTest, NameCollisionGoldenDiff) {
  auto p = CreatePackage();
  BlockBuilder bb("B", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = bb.InputPort("x", u8);
  BValue y = bb.InputPort("y", u8);
  BValue a = bb.And(x, x);
  BValue b = bb.Or(y, y);
  BValue r = bb.Add(a, b);
  bb.OutputPort("out", r);
  ASSERT_TRUE(bb.Build().ok());
  Block* block = p->GetBlock("B").value();

  // Base residual and text.
  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block, base, nullptr, &base_residual).ok());
  std::string verilog_base = GenerateVerilog(block, base, nullptr, nullptr).value();

  // Introduce a duplicate AND node that likely desires a similar name, and use
  // it alongside the original to provoke uniquification under guidance.
  ::xls::Node* and_dup =
      block
          ->MakeNode<::xls::NaryOp>(SourceInfo(), std::vector<Node*>{x.node(), x.node()},
                                     Op::kAnd)
          .value();
  ::xls::Node* add_node = r.node();
  ASSERT_EQ(add_node->op(), Op::kAdd);
  // Replace RHS operand (originally 'b') with the duplicate AND node.
  ASSERT_TRUE(add_node->ReplaceOperandNumber(1, and_dup).ok());

  // Guided codegen using prior residual.
  CodegenOptions guided;
  guided.module_name("B_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog_guided = GenerateVerilog(block, guided, nullptr, nullptr).value();

  std::string diff_text = MakeUnifiedDiff(verilog_base, verilog_guided, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_golden_diff_name_collision.txt",
      diff_text);
}

}  // namespace xls::verilog
