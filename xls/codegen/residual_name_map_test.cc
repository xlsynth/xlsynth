#include "xls/codegen/combinational_generator.h"

#include <string>
#include <unordered_set>

#include "gtest/gtest.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_result.h"
#include "xls/codegen/codegen_residual.pb.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_test_base.h"

namespace xls::verilog {

class ResidualNameMapTest : public IrTestBase {};

TEST_F(ResidualNameMapTest, CombinationalSimpleAddPortsAppearInResidual) {
  auto p = CreatePackage();
  FunctionBuilder fb("add8", p.get());
  Type* u8 = p->GetBitsType(8);
  BValue x = fb.Param("x", u8);
  BValue y = fb.Param("y", u8);
  BValue sum = fb.Add(x, y);
  absl::StatusOr<::xls::Function*> f_or = fb.BuildWithReturnValue(sum);
  ASSERT_TRUE(f_or.ok()) << f_or.status();
  ::xls::Function* f = f_or.value();

  CodegenOptions options;
  // Default options suffice; ensure combinational path is used.
  absl::StatusOr<CodegenResult> result_or =
      GenerateCombinationalModule(f, options, /*delay_estimator=*/nullptr);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  CodegenResult result = std::move(result_or).value();

  // Collect signal names present in the residual map.
  std::unordered_set<std::string> names;
  for (const CodegenResidualData::BlockResidual& b : result.residual.blocks()) {
    for (const CodegenResidualData::BlockResidual::Mapping& m : b.mappings()) {
      names.insert(m.signal_name());
    }
    // Topo order should be present for combinational emission.
    EXPECT_FALSE(b.topo_ir_node_order().empty());
  }

  // Expect input ports and the output port name to be present.
  // Input ports preserve their IR names.
  EXPECT_TRUE(names.count("x")) << "missing residual mapping for input x";
  EXPECT_TRUE(names.count("y")) << "missing residual mapping for input y";
  // Default output port name for functions is "out".
  EXPECT_TRUE(names.count("out")) << "missing residual mapping for output port";
}

}  // namespace xls::verilog
