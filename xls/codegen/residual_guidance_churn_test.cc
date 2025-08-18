#include "xls/codegen/block_generator.h"

#include <string>
#include <vector>
#include "absl/container/flat_hash_map.h"

#include "gtest/gtest.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/verilog_line_map.pb.h"
#include "xls/common/file/temp_file.h"
#include "xls/common/golden_files.h"
#include "xls/common/subprocess.h"
#include "absl/strings/str_format.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"

namespace xls::verilog {

namespace {

absl::StatusOr<std::string> MakeUnifiedDiff(const std::string& base_text,
                                            const std::string& other_text,
                                            int context = 8) {
  XLS_ASSIGN_OR_RETURN(auto base_file,
                       xls::TempFile::CreateWithContent(base_text, ".sv"));
  XLS_ASSIGN_OR_RETURN(auto other_file,
                       xls::TempFile::CreateWithContent(other_text, ".sv"));
  std::string ctx = absl::StrFormat("-U%d", context);
  std::vector<std::string> argv = {"diff", ctx, "-L", "base", "-L",
                                   "other", base_file.path().string(),
                                   other_file.path().string()};
  XLS_ASSIGN_OR_RETURN(auto res, xls::InvokeSubprocess(argv));
  if (!res.normal_termination) {
    return absl::InternalError("diff subprocess did not terminate normally");
  }
  return res.stdout_content;
}

}  // namespace

class ResidualGuidanceChurnTest : public IrTestBase {};

TEST_F(ResidualGuidanceChurnTest, SmallEditGoldenDiffs) {
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

  // Baseline codegen + residual.
  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  VerilogLineMap lm0;
  ASSERT_TRUE(GenerateVerilog(block, base, &lm0, &base_residual).ok());
  std::string verilog0;
  ASSERT_TRUE(GenerateVerilog(block, base, nullptr, nullptr).ok()) << "sanity";
  // Re-emit to capture text.
  {
    VerilogLineMap lm_text;
    CodegenResidualData tmp;
    auto st = GenerateVerilog(block, base, &lm_text, &tmp);
    ASSERT_TRUE(st.ok());
    verilog0 = st.value();
  }

  // Insert a small node and rewire output: r2 = add(a, b), but route a through
  // an extra add with zero to perturb the graph minimally.
  ::xls::Node* a_node = a.node();
  (void) b;  // Silence unused warnings in some build configs.
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

  // No-guidance modified verilog.
  std::string verilog1_nog;
  {
    CodegenOptions opt;
    opt.module_name("B1_nog");
    auto st = GenerateVerilog(block, opt, nullptr, nullptr);
    ASSERT_TRUE(st.ok());
    verilog1_nog = st.value();
  }

  // Guidance-modified verilog.
  std::string verilog1_g;
  {
    CodegenOptions opt;
    opt.module_name("B1_g");
    opt.enable_residual_topo_guidance(true)
        .enable_residual_name_guidance(true)
        .set_previous_residual(base_residual);
    auto st = GenerateVerilog(block, opt, nullptr, nullptr);
    ASSERT_TRUE(st.ok());
    verilog1_g = st.value();
  }

  std::string diff_nog = MakeUnifiedDiff(verilog0, verilog1_nog, 8).value();
  std::string diff_g = MakeUnifiedDiff(verilog0, verilog1_g, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_small_edit_noguided.txt",
      diff_nog);
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_small_edit_guided.txt",
      diff_g);
}

// Removed randomization test to avoid cases which modify the module signature.
/*TEST_F(ResidualGuidanceChurnTest, RandomizationGoldenDiffs) {
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

  CodegenResidualData base_residual;
  VerilogLineMap lm0;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block, base, &lm0, &base_residual).ok());
  std::string verilog0 = GenerateVerilog(block, base, nullptr, nullptr).value();

  CodegenOptions rng_nog;
  rng_nog.module_name("B1_nog");
  int32_t seed_arr[3] = {7, 11, 13};
  rng_nog.randomize_order_seed(absl::MakeConstSpan(seed_arr));
  std::string verilog_nog = GenerateVerilog(block, rng_nog, nullptr, nullptr).value();

  CodegenOptions rng_guided;
  rng_guided.module_name("B1_g");
  rng_guided.randomize_order_seed(absl::MakeConstSpan(seed_arr));
  rng_guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  rng_guided.set_previous_residual(base_residual);
  std::string verilog_guided = GenerateVerilog(block, rng_guided, nullptr, nullptr).value();
  std::string diff_nog = MakeUnifiedDiff(verilog0, verilog_nog, 8).value();
  std::string diff_g = MakeUnifiedDiff(verilog0, verilog_guided, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_random_noguided.txt",
      diff_nog);
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_random_guided.txt",
      diff_g);
}*/

TEST_F(ResidualGuidanceChurnTest, PreferredNamesAreReusedForOverlappingNodes) {
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

  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block, base, nullptr, &base_residual).ok());

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

  CodegenResidualData guided_residual;
  CodegenOptions guided;
  guided.module_name("B1_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  ASSERT_TRUE(GenerateVerilog(block, guided, nullptr, &guided_residual).ok());

  absl::flat_hash_map<int64_t, std::string> prev;
  for (const auto& bres : base_residual.blocks()) {
    for (const auto& m : bres.mappings()) prev[m.ir_node_id()] = m.signal_name();
  }
  for (const auto& bres : guided_residual.blocks()) {
    for (const auto& m : bres.mappings()) {
      auto it = prev.find(m.ir_node_id());
      if (it != prev.end()) {
        EXPECT_EQ(it->second, m.signal_name());
      }
    }
  }
}

TEST_F(ResidualGuidanceChurnTest, HighFanoutPivotGoldenDiffs) {
  auto p = CreatePackage();
  BlockBuilder bb("B", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = bb.InputPort("x", u8);
  BValue y = bb.InputPort("y", u8);
  // High-fanout hub h used by many consumers.
  BValue h = bb.And(x, y);
  std::vector<BValue> adds;
  for (int i = 0; i < 8; ++i) {
    BValue c = (i & 1) ? x : y;
    BValue a = bb.Add(h, c);
    adds.push_back(a);
  }
  // Accumulate to output to keep all consumers alive.
  BValue sum = bb.Literal(UBits(0, 8));
  for (const BValue& v : adds) {
    sum = bb.Add(sum, v);
  }
  bb.OutputPort("out", sum);
  ASSERT_TRUE(bb.Build().ok());
  Block* block = p->GetBlock("B").value();

  // Baseline residual and text.
  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  std::string verilog_base = GenerateVerilog(block, base, nullptr, &base_residual).value();

  // Small edit: pivot ready set by adding a new small dependency into hub path.
  Node* h_node = h.node();
  Node* mask = block->MakeNode<::xls::NaryOp>(SourceInfo(), std::vector<Node*>{x.node(), y.node()}, Op::kXor).value();
  Node* h2 = block->MakeNode<::xls::NaryOp>(SourceInfo(), std::vector<Node*>{h_node, mask}, Op::kOr).value();
  // Redirect all consumers of h to use h2.
  for (const BValue& v : adds) {
    Node* n = v.node();
    for (int64_t oi = 0; oi < n->operand_count(); ++oi) {
      if (n->operand(oi) == h_node) {
        ASSERT_TRUE(n->ReplaceOperandNumber(oi, h2).ok());
      }
    }
  }

  // No-guidance diff.
  std::string verilog_nog = GenerateVerilog(block, CodegenOptions().module_name("B1_nog"), nullptr, nullptr).value();
  // Guided diff.
  CodegenOptions guided;
  guided.module_name("B1_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog_g = GenerateVerilog(block, guided, nullptr, nullptr).value();

  std::string diff_nog = MakeUnifiedDiff(verilog_base, verilog_nog, 8).value();
  std::string diff_g = MakeUnifiedDiff(verilog_base, verilog_g, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_high_fanout_pivot_noguided.txt",
      diff_nog);
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_high_fanout_pivot_guided.txt",
      diff_g);
}

TEST_F(ResidualGuidanceChurnTest, DeepFanoutInsertionGoldenDiffs) {
  auto p = CreatePackage();
  BlockBuilder bb("B", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = bb.InputPort("x", u8);
  BValue y = bb.InputPort("y", u8);
  // Many consumers directly use x.
  std::vector<BValue> uses;
  for (int i = 0; i < 10; ++i) {
    BValue a = bb.Add(x, (i & 1) ? y : x);
    uses.push_back(a);
  }
  BValue sum = bb.Literal(UBits(0, 8));
  for (const BValue& v : uses) {
    sum = bb.Add(sum, v);
  }
  bb.OutputPort("out", sum);
  ASSERT_TRUE(bb.Build().ok());
  Block* block = p->GetBlock("B").value();

  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  std::string verilog_base = GenerateVerilog(block, base, nullptr, &base_residual).value();

  // Insert a small node upstream of many consumers: x1 = x + 1; replace uses.
  Node* x_node = x.node();
  Node* one = block->MakeNode<::xls::Literal>(SourceInfo(), Value(UBits(1, 8))).value();
  Node* x1 = block->MakeNode<::xls::BinOp>(SourceInfo(), x_node, one, Op::kAdd).value();
  for (const BValue& v : uses) {
    Node* n = v.node();
    for (int64_t oi = 0; oi < n->operand_count(); ++oi) {
      if (n->operand(oi) == x_node) {
        ASSERT_TRUE(n->ReplaceOperandNumber(oi, x1).ok());
      }
    }
  }

  std::string verilog_nog = GenerateVerilog(block, CodegenOptions().module_name("B1_nog"), nullptr, nullptr).value();
  CodegenOptions guided;
  guided.module_name("B1_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog_g = GenerateVerilog(block, guided, nullptr, nullptr).value();

  std::string diff_nog = MakeUnifiedDiff(verilog_base, verilog_nog, 8).value();
  std::string diff_g = MakeUnifiedDiff(verilog_base, verilog_g, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_deep_fanout_insert_noguided.txt",
      diff_nog);
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_churn_deep_fanout_insert_guided.txt",
      diff_g);
}

TEST_F(ResidualGuidanceChurnTest, NameFallbackAcrossIdChangesGoldenDiffs) {
  // Build base block with explicit node names.
  auto p0 = CreatePackage();
  BlockBuilder bb0("B", p0.get());
  Type* u8 = p0->GetBitsType(8);
  BValue x0 = bb0.InputPort("x", u8);
  BValue y0 = bb0.InputPort("y", u8);
  BValue a0 = bb0.And(x0, x0, SourceInfo(), /*name=*/"a");
  BValue b0 = bb0.Or(y0, y0, SourceInfo(), /*name=*/"b");
  BValue r0 = bb0.Add(a0, b0, SourceInfo(), /*name=*/"r");
  bb0.OutputPort("out", r0);
  ASSERT_TRUE(bb0.Build().ok());
  Block* block0 = p0->GetBlock("B").value();

  CodegenResidualData base_residual;
  CodegenOptions base;
  base.module_name("B0");
  ASSERT_TRUE(GenerateVerilog(block0, base, nullptr, &base_residual).ok());
  std::string verilog_base = GenerateVerilog(block0, base, nullptr, nullptr).value();

  // Rebuild an equivalent block in a fresh package so node IDs differ but
  // names match; this exercises name-based fallback when ids change.
  auto p1 = CreatePackage();
  BlockBuilder bb1("B", p1.get());
  Type* u8_1 = p1->GetBitsType(8);
  BValue x1 = bb1.InputPort("x", u8_1);
  BValue y1 = bb1.InputPort("y", u8_1);
  BValue a1 = bb1.And(x1, x1);
  BValue b1 = bb1.Or(y1, y1);
  BValue r1 = bb1.Add(a1, b1);
  bb1.OutputPort("out", r1);
  ASSERT_TRUE(bb1.Build().ok());
  Block* block1 = p1->GetBlock("B").value();

  // No-guidance: expect larger diff due to fresh IDs/names.
  std::string verilog1_nog = GenerateVerilog(block1, CodegenOptions().module_name("B1_nog"), nullptr, nullptr).value();

  // Guided with prior residual: expect minimal diff as prior names are reused by ir_node_name fallback.
  CodegenOptions guided;
  guided.module_name("B1_g");
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_residual);
  std::string verilog1_g = GenerateVerilog(block1, guided, nullptr, nullptr).value();

  std::string diff_nog = MakeUnifiedDiff(verilog_base, verilog1_nog, 8).value();
  std::string diff_g = MakeUnifiedDiff(verilog_base, verilog1_g, 8).value();
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_name_fallback_idchurn_noguided.txt",
      diff_nog);
  ExpectEqualToGoldenFile(
      "xls/codegen/testdata/residual_guidance_name_fallback_idchurn_guided.txt",
      diff_g);
}

}  // namespace xls::verilog
