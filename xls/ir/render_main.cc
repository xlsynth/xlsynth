#include "xls/common/init_xls.h"

#include "absl/log/log.h"
#include "absl/strings/str_join.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "xls/common/exit_status.h"

#include "xls/ir/op_specification.h"
#include "xls/ir/render_op_header.h"
#include "xls/ir/render_op_source.h"
#include "xls/ir/render_nodes_header.h"
#include "xls/ir/render_nodes_source.h"

namespace xls {
namespace {

static constexpr std::string_view kUsage = R"(
Emits op.{h,cc} and nodes.{h,cc} files from XLS' operation metadata.
)";

std::string RenderOpProto() {
  std::vector<std::string> lines = {
    // TODO(cdleary): 2024-08-07 Change this banner.
    "// DO NOT EDIT: this file is AUTOMATICALLY GENERATED from op_specification.py",
    "// and op_proto.tmpl and should not be changed.",
    "syntax = \"proto3\";",
    "",
    "package xls;",
    "",
    "// Listing of all XLS IR operations.",
    "enum OpProto {",
    "  OP_INVALID = 0;",
  };
  const std::vector<Op>& ops = GetOpsSingleton();
  for (int64_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    lines.push_back(absl::StrFormat("  OP_%s = %d;", absl::AsciiStrToUpper(op.name), i+1));
  }
  lines.push_back("}");
  return absl::StrJoin(lines, "\n");
}

std::string RenderEnumOpPairs() {
  std::vector<std::string> lines;
  for (const Op& op : GetOpsSingleton()) {
    lines.push_back(absl::StrFormat("%s %s", op.enum_name, op.name));
  }
  return absl::StrJoin(lines, "\n");
}

absl::Status RealMain(std::string_view selection) {
  if (selection == "op_header") {
    std::cout << RenderOpHeader();
  } else if (selection == "op_source") {
    std::cout << RenderOpSource();
  } else if (selection == "op_proto") {
    std::cout << RenderOpProto();
  } else if (selection == "nodes_header") {
    std::cout << RenderNodesHeader();
  } else if (selection == "nodes_source") {
    std::cout << RenderNodesSource();
  } else if (selection == "enum_op_pairs") {
    std::cout << RenderEnumOpPairs();
  } else {
    return absl::InvalidArgumentError(absl::StrFormat("Cannot render selection: `%s`", selection));
  }
  return absl::OkStatus();
}

}  // namespace
}  // namespace xls

int main(int argc, char** argv) {
  std::vector<std::string_view> args =
      xls::InitXls(xls::kUsage, argc, argv);
  if (args.empty()) {
    LOG(QFATAL) << "Wrong number of command-line arguments; got " << args.size()
                << ": `" << absl::StrJoin(args, " ") << "`; want " << argv[0]
                << " <selection>";
  }

  return xls::ExitStatus(xls::RealMain(args[0]));
}