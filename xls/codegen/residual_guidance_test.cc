#include "xls/codegen/combinational_generator.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "xls/codegen/block_generator.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_result.h"
#include "xls/codegen/verilog_line_map.pb.h"
#include "xls/ir/block.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_test_base.h"

namespace xls::verilog {

class ResidualGuidanceTest : public IrTestBase {};

TEST_F(ResidualGuidanceTest, TopoAndNamesHoldUnderRandomizationWhenGuided) {
  auto p = CreatePackage();
  ::xls::BlockBuilder bb("B", p.get());
  ::xls::Type* u8 = p->GetBitsType(8);
  ::xls::BValue x = bb.InputPort("x", u8);
  ::xls::BValue y = bb.InputPort("y", u8);
  // Two independent ops whose relative order among ready nodes is ambiguous.
  ::xls::BValue a = bb.And(x, y);
  ::xls::BValue b = bb.Or(x, y);
  ::xls::BValue r = bb.Add(a, b);
  bb.OutputPort("out", r);
  absl::StatusOr<::xls::Block*> block_or = bb.Build();
  ASSERT_TRUE(block_or.ok()) << block_or.status();
  ::xls::Block* block = block_or.value();

  // First run: baseline residual without randomization.
  CodegenOptions base;
  base.module_name("B_base");
  VerilogLineMap lm1;
  CodegenResidualData first;
  ASSERT_TRUE(GenerateVerilog(block, base, &lm1, &first).ok());

  // Second run on the same block: randomize order, but enable guidance using previous residual.
  CodegenOptions guided;
  guided.module_name("B_guided");
  int32_t seed_arr[3] = {1, 2, 3};
  guided.randomize_order_seed(absl::MakeConstSpan(seed_arr));
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(first);
  VerilogLineMap lm2;
  CodegenResidualData second;
  ASSERT_TRUE(GenerateVerilog(block, guided, &lm2, &second).ok());

  // Compare topo orders after filtering out ports, which can be permuted.
  ASSERT_EQ(first.blocks_size(), second.blocks_size());
  ASSERT_GT(first.blocks_size(), 0);
  const auto& b1 = first.blocks(0);
  const auto& b2 = second.blocks(0);
  std::vector<int64_t> f1;
  std::vector<int64_t> f2;
  f1.reserve(b1.topo_ir_node_order_size());
  f2.reserve(b2.topo_ir_node_order_size());
  auto is_port = [&](int64_t id) {
    for (Node* n : block->nodes()) if (n->id() == id) return n->Is<InputPort>() || n->Is<OutputPort>();
    return false;
  };
  for (int i = 0; i < b1.topo_ir_node_order_size(); ++i) if (!is_port(b1.topo_ir_node_order(i))) f1.push_back(b1.topo_ir_node_order(i));
  for (int i = 0; i < b2.topo_ir_node_order_size(); ++i) if (!is_port(b2.topo_ir_node_order(i))) f2.push_back(b2.topo_ir_node_order(i));
  ASSERT_EQ(f1.size(), f2.size());
  absl::flat_hash_map<int64_t, int> pos2;
  for (int i = 0; i < f2.size(); ++i) pos2[f2[i]] = i;
  for (int i = 0; i < f1.size(); ++i) {
    for (int j = i + 1; j < f1.size(); ++j) {
      EXPECT_LT(pos2[f1[i]], pos2[f1[j]]) << "Relative order changed for ids " << f1[i] << ", " << f1[j];
    }
  }

  // Build maps from id->name to check surviving nodes keep the same names.
  std::unordered_map<int64_t, std::string> first_names;
  for (const auto& m : b1.mappings()) first_names[m.ir_node_id()] = m.signal_name();
  for (const auto& m : b2.mappings()) {
    auto it = first_names.find(m.ir_node_id());
    if (it != first_names.end()) {
      EXPECT_EQ(it->second, m.signal_name())
          << "name changed for node id " << m.ir_node_id();
    }
  }
}

TEST_F(ResidualGuidanceTest, GuidedTopoHonorsPriorRanksWithManyReadyNodes) {
  auto p = CreatePackage();
  ::xls::BlockBuilder bb("B", p.get());
  ::xls::Type* u8 = p->GetBitsType(8);
  ::xls::BValue x = bb.InputPort("x", u8);
  ::xls::BValue y = bb.InputPort("y", u8);
  // Create multiple independent ready nodes to stress tie-breaking.
  ::xls::BValue a = bb.Xor(x, y);
  ::xls::BValue b = bb.And(x, y);
  ::xls::BValue c = bb.Or(x, y);
  ::xls::BValue d = bb.Add(x, y);
  ::xls::BValue e = bb.Subtract(y, x);
  // Consume them in a small tree so all contribute to the output.
  ::xls::BValue s1 = bb.Add(a, b);
  ::xls::BValue s2 = bb.Add(c, d);
  ::xls::BValue s3 = bb.Add(s1, s2);
  ::xls::BValue out = bb.Add(s3, e);
  bb.OutputPort("out", out);
  absl::StatusOr<::xls::Block*> block_or = bb.Build();
  ASSERT_TRUE(block_or.ok()) << block_or.status();
  ::xls::Block* block = block_or.value();

  // Baseline residual without randomization.
  CodegenOptions base;
  base.module_name("B_base");
  CodegenResidualData first;
  ASSERT_TRUE(GenerateVerilog(block, base, /*verilog_line_map=*/nullptr, &first).ok());

  // Randomize and guide with prior residual; topo order should be identical.
  CodegenOptions guided;
  guided.module_name("B_guided");
  int32_t seed_arr[3] = {9, 8, 7};
  guided.randomize_order_seed(absl::MakeConstSpan(seed_arr));
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(first);
  CodegenResidualData second;
  ASSERT_TRUE(GenerateVerilog(block, guided, /*verilog_line_map=*/nullptr, &second).ok());

  ASSERT_EQ(first.blocks_size(), second.blocks_size());
  ASSERT_GT(first.blocks_size(), 0);
  const auto& b1 = first.blocks(0);
  const auto& b2 = second.blocks(0);
  std::vector<int64_t> f1;
  std::vector<int64_t> f2;
  auto is_port = [&](int64_t id) {
    for (Node* n : block->nodes()) if (n->id() == id) return n->Is<InputPort>() || n->Is<OutputPort>();
    return false;
  };
  for (int i = 0; i < b1.topo_ir_node_order_size(); ++i) if (!is_port(b1.topo_ir_node_order(i))) f1.push_back(b1.topo_ir_node_order(i));
  for (int i = 0; i < b2.topo_ir_node_order_size(); ++i) if (!is_port(b2.topo_ir_node_order(i))) f2.push_back(b2.topo_ir_node_order(i));
  ASSERT_EQ(f1.size(), f2.size());
  absl::flat_hash_map<int64_t, int> pos2;
  for (int i = 0; i < f2.size(); ++i) pos2[f2[i]] = i;
  for (int i = 0; i < f1.size(); ++i) {
    for (int j = i + 1; j < f1.size(); ++j) {
      EXPECT_LT(pos2[f1[i]], pos2[f1[j]]) << "Relative order changed for ids " << f1[i] << ", " << f1[j];
    }
  }
}

TEST_F(ResidualGuidanceTest, GuidedTopoDeterministicForNewReadyNodes) {
  auto p = CreatePackage();
  ::xls::BlockBuilder bb("B", p.get());
  ::xls::Type* u8 = p->GetBitsType(8);
  ::xls::BValue x = bb.InputPort("x", u8);
  ::xls::BValue y = bb.InputPort("y", u8);
  // Base graph.
  ::xls::BValue a = bb.Add(x, y);
  ::xls::BValue r = bb.Add(a, x);
  bb.OutputPort("out", r);
  absl::StatusOr<::xls::Block*> block_or = bb.Build();
  ASSERT_TRUE(block_or.ok()) << block_or.status();
  ::xls::Block* block = block_or.value();

  // Baseline residual.
  CodegenOptions base;
  base.module_name("B_base");
  CodegenResidualData base_res;
  ASSERT_TRUE(GenerateVerilog(block, base, /*verilog_line_map=*/nullptr, &base_res).ok());

  // Add two new independent ready nodes that depend only on inputs, and make them
  // contribute to the output so they appear in codegen.
  ::xls::Node* x_node = x.node();
  ::xls::Node* y_node = y.node();
  absl::StatusOr<::xls::Node*> n1_or =
      block->MakeNode<::xls::NaryOp>(SourceInfo(), std::vector<Node*>{x_node, y_node}, Op::kXor);
  ASSERT_TRUE(n1_or.ok()) << n1_or.status();
  ::xls::Node* n1 = n1_or.value();
  absl::StatusOr<::xls::Node*> n2_or =
      block->MakeNode<::xls::NaryOp>(SourceInfo(), std::vector<Node*>{x_node, y_node}, Op::kAnd);
  ASSERT_TRUE(n2_or.ok()) << n2_or.status();
  ::xls::Node* n2 = n2_or.value();

  // Replace out with a sum including the new nodes.
  OutputPort* outp = nullptr;
  for (Node* n : block->nodes()) {
    if (n->Is<OutputPort>()) { outp = n->As<OutputPort>(); break; }
  }
  ASSERT_NE(outp, nullptr);
  absl::StatusOr<::xls::Node*> new_sum_or =
      block->MakeNode<::xls::BinOp>(SourceInfo(), n1, n2, Op::kAdd);
  ASSERT_TRUE(new_sum_or.ok()) << new_sum_or.status();
  ::xls::Node* new_sum = new_sum_or.value();
  ASSERT_TRUE(outp->ReplaceOperandNumber(0, new_sum).ok());

  auto guided_run = [&](absl::Span<const int32_t> seed) -> std::pair<int,int> {
    CodegenOptions guided;
    guided.module_name("B_guided");
    guided.randomize_order_seed(seed);
    guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
    guided.set_previous_residual(base_res);
    CodegenResidualData res;
    auto st = GenerateVerilog(block, guided, /*verilog_line_map=*/nullptr, &res);
    if (!st.ok()) {
      ADD_FAILURE() << st.status();
      return std::pair<int,int>(-1, -1);
    }
    if (res.blocks_size() != 1) {
      ADD_FAILURE() << "unexpected blocks_size: " << res.blocks_size();
      return std::pair<int,int>(-1, -1);
    }
    const auto& br = res.blocks(0);
    // Find positions of the new nodes.
    int pos_n1 = -1, pos_n2 = -1;
    for (int i = 0; i < br.topo_ir_node_order_size(); ++i) {
      int64_t id = br.topo_ir_node_order(i);
      if (id == n1->id()) pos_n1 = i;
      if (id == n2->id()) pos_n2 = i;
    }
    EXPECT_NE(pos_n1, -1);
    EXPECT_NE(pos_n2, -1);
    return std::pair<int,int>(pos_n1, pos_n2);
  };

  int32_t s1[3] = {5, 6, 7};
  int32_t s2[3] = {11, 13, 17};
  auto p1 = guided_run(absl::MakeConstSpan(s1));
  auto p2 = guided_run(absl::MakeConstSpan(s2));
  // Order between new nodes must be deterministic across seeds (id order fallback).
  EXPECT_EQ((p1.first < p1.second), (n1->id() < n2->id()));
  EXPECT_EQ((p2.first < p2.second), (n1->id() < n2->id()));
  EXPECT_EQ(p1.first < p1.second, p2.first < p2.second);
}

TEST_F(ResidualGuidanceTest, NameBasedStabilityAcrossIdChanges) {
  // Build base block A and residual.
  auto p1 = CreatePackage();
  ::xls::BlockBuilder b1("B", p1.get());
  ::xls::Type* u8 = p1->GetBitsType(8);
  ::xls::BValue x1 = b1.InputPort("x", u8);
  ::xls::BValue y1 = b1.InputPort("y", u8);
  // Two nodes each used twice to force assignment emission.
  ::xls::BValue a1 = b1.Add(x1, y1);            // name auto
  ::xls::BValue b1n = b1.Xor(x1, y1);           // name auto
  ::xls::BValue s1a = b1.Add(a1, b1n);
  ::xls::BValue s1b = b1.Add(a1, s1a);          // a1 used twice
  ::xls::BValue s1c = b1.Xor(b1n, s1a);         // b1n used twice
  b1.OutputPort("out", b1.Add(s1b, s1c));
  ASSERT_TRUE(b1.Build().ok());
  ::xls::Block* A = p1->GetBlock("B").value();
  CodegenResidualData base_res;
  ASSERT_TRUE(GenerateVerilog(A, CodegenOptions().module_name("A"), nullptr, &base_res).ok());

  // Build structurally identical block B in a different package (different ids).
  auto p2 = CreatePackage();
  ::xls::BlockBuilder b2("B", p2.get());
  ::xls::Type* u8b = p2->GetBitsType(8);
  ::xls::BValue x2 = b2.InputPort("x", u8b);
  ::xls::BValue y2 = b2.InputPort("y", u8b);
  ::xls::BValue a2 = b2.Add(x2, y2);
  ::xls::BValue b2n = b2.Xor(x2, y2);
  ::xls::BValue t2a = b2.Add(a2, b2n);
  ::xls::BValue t2b = b2.Add(a2, t2a);
  ::xls::BValue t2c = b2.Xor(b2n, t2a);
  b2.OutputPort("out", b2.Add(t2b, t2c));
  ASSERT_TRUE(b2.Build().ok());
  ::xls::Block* B = p2->GetBlock("B").value();

  // Guided generation on B should preserve relative order by names and keep assignment shape for a*, b*.
  CodegenResidualData guided_res;
  CodegenOptions guided;
  guided.module_name("B_guided");
  int32_t seed[3] = {42, 7, 9};
  guided.randomize_order_seed(absl::MakeConstSpan(seed));
  guided.enable_residual_topo_guidance(true).enable_residual_name_guidance(true);
  guided.set_previous_residual(base_res);
  ASSERT_TRUE(GenerateVerilog(B, guided, nullptr, &guided_res).ok());

  // Collect emission events seq by ir_node_name for base and guided.
  absl::flat_hash_map<std::string, int64_t> base_seq_by_name;
  for (const auto& ev : base_res.blocks(0).emission_events()) {
    base_seq_by_name[std::string(ev.ir_node_name())] = ev.seq();
  }
  absl::flat_hash_map<std::string, int64_t> guid_seq_by_name;
  for (const auto& ev : guided_res.blocks(0).emission_events()) {
    guid_seq_by_name[std::string(ev.ir_node_name())] = ev.seq();
  }
  // Ensure relative order of key nodes by name is preserved.
  std::string a_name_base = a1.node()->GetName();
  std::string b_name_base = b1n.node()->GetName();
  std::string a_name_guid = a2.node()->GetName();
  std::string b_name_guid = b2n.node()->GetName();
  auto expect_before = [&](const std::string& ab, const std::string& bb,
                           const std::string& ag, const std::string& bg) {
    ASSERT_TRUE(base_seq_by_name.contains(ab));
    ASSERT_TRUE(base_seq_by_name.contains(bb));
    ASSERT_TRUE(guid_seq_by_name.contains(ag));
    ASSERT_TRUE(guid_seq_by_name.contains(bg));
    EXPECT_LT(base_seq_by_name[ab], base_seq_by_name[bb]);
    EXPECT_LT(guid_seq_by_name[ag], guid_seq_by_name[bg]);
  };
  expect_before(a_name_base, b_name_base, a_name_guid, b_name_guid);

  // Check assignment shape preserved by name for nodes used twice (add, xor likely assignments).
  auto was_assignment = [&](const CodegenResidualData& r, const std::string& name) {
    for (const auto& m : r.blocks(0).mappings()) {
      if (m.ir_node_name() == name) return m.emission_kind() == CodegenResidualData::ASSIGNMENT;
    }
    return false;
  };
  EXPECT_TRUE(was_assignment(base_res, a_name_base));
  EXPECT_TRUE(was_assignment(base_res, b_name_base));
  EXPECT_TRUE(was_assignment(guided_res, a_name_guid));
  EXPECT_TRUE(was_assignment(guided_res, b_name_guid));
}

}  // namespace xls::verilog
