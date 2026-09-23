// Copyright 2024 The XLS Authors
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
//
// DSLX-to-SystemVerilog type and constant converter.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/import_routines.h"
#include "xls/dslx/ir_convert/ir_converter_options_flags.h"
#include "xls/dslx/ir_convert/ir_converter_options_flags.pb.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/translators/dslx_to_verilog.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/virtualizable_file_system.h"
#include "xls/dslx/warning_kind.h"

ABSL_FLAG(std::string, namespace, "xls",
          "The Verilog namespace to generate the code in (e.g., `foo::bar`).");
// TODO: google/xls#922 - use a more generic way to add waivers.
ABSL_FLAG(std::vector<std::string>, lint_waivers, {},
          "Lint waivers to add to the generated code.");

namespace xls {
namespace dslx {
namespace {
bool TypeDefinitionSourceIsPublic(const TypeInfo::TypeSource& def_source) {
  return absl::visit(
      Visitor{
          [](const TypeAlias* type_alias) { return type_alias->is_public(); },
          [](const ProcDef* proc_def) { return proc_def->is_public(); },
          [](const EnumDef* enum_def) { return enum_def->is_public(); },
          [](const SumDef* sum_def) { return sum_def->is_public(); },
          [](const StructDef* struct_def) { return struct_def->is_public(); },
      },
      def_source.definition);
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path,
                                   const std::filesystem::path& cwd) {
  return (path.is_absolute() ? path : cwd / path).lexically_normal();
}

// An explicitly supplied file and a source-level import must use the same cache
// entry when this is the spelling the compiler resolves to that file.
std::optional<std::string> RelativeModuleName(
    const std::filesystem::path& path, const std::filesystem::path& root) {
  std::filesystem::path relative = path.lexically_relative(root);
  if (relative.extension() != ".x") {
    return std::nullopt;
  }
  relative.replace_extension();
  std::vector<std::string> components;
  for (const std::filesystem::path& component : relative) {
    std::string name = component.string();
    if (name.empty() ||
        !(absl::ascii_isalpha(name.front()) || name.front() == '_') ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) {
          return absl::ascii_isalnum(c) || c == '_';
        })) {
      return std::nullopt;
    }
    components.push_back(std::move(name));
  }
  return absl::StrJoin(components, ".");
}

std::vector<std::string> StandalonePathComponents(
    const std::filesystem::path& path) {
  std::vector<std::string> components;
  for (const std::filesystem::path& component :
       path.parent_path().relative_path()) {
    components.push_back(component.string());
  }
  // Non-.x inputs are valid CLI inputs but do not have an import spelling.
  // Preserve the extension distinction if both `foo` and `foo.x` are inputs.
  components.push_back(path.extension() == ".x"
                           ? path.stem().string()
                           : path.filename().string() + ".");
  return components;
}

// '$' cannot occur in a source-level DSLX import identifier. It makes these
// cache keys distinct from named imports while projecting to '_' in Verilog.
// For example, two files named shared.x can use $_one__shared and $_two__shared
// without embedding the checkout's absolute location in generated sum names.
std::string StandaloneModuleName(absl::Span<const std::string> components,
                                 size_t depth) {
  std::string result = "$_";
  bool first = true;
  for (const std::string& component :
       components.last(std::min(depth, components.size()))) {
    if (!first) {
      absl::StrAppend(&result, "__");
    }
    first = false;
    for (unsigned char c : component) {
      if (absl::ascii_isalnum(c)) {
        result.push_back(c);
      } else {
        absl::StrAppend(&result, "_", absl::Hex(c, absl::kZeroPad2));
      }
    }
  }
  return result;
}

struct ExplicitInput {
  std::string_view path;
  std::filesystem::path absolute;
  std::optional<std::string> module_name;
};

bool PreferModuleName(std::string_view a, std::string_view b) {
  return std::make_pair(std::count(a.begin(), a.end(), '.'), a) <
         std::make_pair(std::count(b.begin(), b.end(), '.'), b);
}

absl::StatusOr<std::vector<ExplicitInput>> GetExplicitInputs(
    absl::Span<const std::string_view> paths, ImportData& import_data) {
  XLS_ASSIGN_OR_RETURN(std::filesystem::path cwd,
                       import_data.vfs().GetCurrentDirectory());
  std::vector<ExplicitInput> inputs;
  std::set<std::filesystem::path> known_paths;
  for (std::string_view path : paths) {
    std::filesystem::path absolute = AbsolutePath(path, cwd);
    known_paths.insert(absolute);
    inputs.push_back({path, std::move(absolute), std::nullopt});
  }
  for (ExplicitInput& input : inputs) {
    if (input.path == "/dev/stdin") {
      // Keep stdin outside named imports and the standalone "$_" namespace.
      input.module_name = "_$stdin";
      continue;
    }

    auto resolves_to_input =
        [&](std::string_view name) -> absl::StatusOr<bool> {
      XLS_ASSIGN_OR_RETURN(ImportTokens tokens, ImportTokens::FromString(name));
      absl::StatusOr<std::filesystem::path> resolved =
          FindImportFilesystemPath(tokens, input.path, import_data);
      if (resolved.ok()) {
        std::filesystem::path absolute = AbsolutePath(*resolved, cwd);
        known_paths.insert(absolute);
        return absolute == input.absolute;
      } else if (absl::IsNotFound(resolved.status())) {
        return false;
      } else {
        return resolved.status();
      }
    };
    auto try_root = [&](const std::filesystem::path& root) -> absl::Status {
      std::optional<std::string> name =
          RelativeModuleName(input.absolute, AbsolutePath(root, cwd));
      if (name.has_value()) {
        XLS_ASSIGN_OR_RETURN(bool matches, resolves_to_input(*name));
        if (matches && (!input.module_name.has_value() ||
                        PreferModuleName(*name, *input.module_name))) {
          input.module_name = std::move(name);
        }
      }
      return absl::OkStatus();
    };

    for (const std::filesystem::path& root :
         import_data.additional_search_paths()) {
      if (!root.empty()) {
        XLS_RETURN_IF_ERROR(try_root(root));
      }
    }
    if (!input.module_name.has_value()) {
      XLS_RETURN_IF_ERROR(try_root(cwd));
    }
    if (!input.module_name.has_value()) {
      XLS_ASSIGN_OR_RETURN(std::string basename, PathToName(input.path));
      XLS_RETURN_IF_ERROR(resolves_to_input(basename).status());
    }
  }

  for (ExplicitInput& input : inputs) {
    if (!input.module_name.has_value()) {
      std::vector<std::string> components =
          StandalonePathComponents(input.absolute);
      size_t depth = 1;
      auto conflicts = [&](std::string_view name) {
        return std::any_of(known_paths.begin(), known_paths.end(),
                           [&](const std::filesystem::path& path) {
                             return path != input.absolute &&
                                    StandaloneModuleName(
                                        StandalonePathComponents(path),
                                        depth) == name;
                           });
      };
      std::string name = StandaloneModuleName(components, depth);
      while (depth < components.size() && conflicts(name)) {
        name = StandaloneModuleName(components, ++depth);
      }
      input.module_name = std::move(name);
    }
  }
  return inputs;
}

// An input path is not a source-level module spelling. If another root imports
// that file, use the compiler's already-typechecked declaration for the
// explicit export too. Overlapping search roots can otherwise give the same
// input a second nominal sum identity. Distinct source-level imports remain
// distinct as defined by the DSLX compiler; this only selects which one an
// explicit path adds.
absl::StatusOr<std::vector<std::pair<Module*, TypeInfo*>>>
SelectExplicitModules(
    absl::Span<const ExplicitInput> inputs,
    absl::Span<const std::pair<Module*, TypeInfo*>> parsed_inputs,
    ImportData& import_data) {
  if (inputs.size() != parsed_inputs.size()) {
    return absl::InternalError(
        "Explicit inputs and typechecked modules differ");
  }
  XLS_ASSIGN_OR_RETURN(std::filesystem::path cwd,
                       import_data.vfs().GetCurrentDirectory());
  using ModuleAndType = std::pair<Module*, TypeInfo*>;
  std::map<std::filesystem::path, std::map<std::string, ModuleAndType>>
      imported_by_path;
  std::vector<ModuleAndType> pending(parsed_inputs.begin(),
                                     parsed_inputs.end());
  std::set<Module*> visited;
  std::set<Module*> resolved_imports;
  while (!pending.empty()) {
    auto [module, type_info] = pending.back();
    pending.pop_back();
    if (!visited.insert(module).second) {
      continue;
    }
    for (const auto& [subject, imported] : type_info->GetRootImports()) {
      pending.emplace_back(imported.module, imported.type_info);
      if (resolved_imports.insert(imported.module).second) {
        XLS_ASSIGN_OR_RETURN(ImportTokens tokens,
                             ImportTokens::FromString(imported.module->name()));
        std::filesystem::path importing_path = module->fs_path().value_or(
            std::filesystem::path(inputs.front().path));
        XLS_ASSIGN_OR_RETURN(std::filesystem::path resolved,
                             FindImportFilesystemPath(
                                 tokens, importing_path.string(), import_data));
        imported_by_path[AbsolutePath(resolved, cwd)].emplace(
            imported.module->name(),
            ModuleAndType{imported.module, imported.type_info});
      }
    }
  }

  std::vector<ModuleAndType> result;
  std::set<Module*> explicit_modules;
  for (size_t i = 0; i < inputs.size(); ++i) {
    ModuleAndType selected = parsed_inputs[i];
    auto imported = imported_by_path.find(inputs[i].absolute);
    if (imported != imported_by_path.end() &&
        !imported->second.contains(selected.first->name())) {
      auto preferred =
          std::min_element(imported->second.begin(), imported->second.end(),
                           [](const auto& a, const auto& b) {
                             return PreferModuleName(a.first, b.first);
                           });
      selected = preferred->second;
    }
    if (explicit_modules.insert(selected.first).second) {
      result.push_back(selected);
    }
  }
  return result;
}

absl::Status RealMain(absl::Span<const std::string_view> paths) {
  // Reuse IR converter options as they align closely with the DSLX-to-Verilog
  // use case. The IR converter converts DSLX to IR, which involves getting
  // concrete types as a prerequisite.
  XLS_ASSIGN_OR_RETURN(IrConverterOptionsFlagsProto ir_converter_options,
                       GetIrConverterOptionsFlagsProto());

  std::optional<std::filesystem::path> output_file =
      ir_converter_options.has_output_file()
          ? std::make_optional<std::filesystem::path>(
                ir_converter_options.output_file())
          : std::nullopt;

  std::string_view dslx_stdlib_path = ir_converter_options.dslx_stdlib_path();
  std::string_view dslx_path = ir_converter_options.dslx_path();
  std::vector<std::string_view> dslx_path_strs = absl::StrSplit(dslx_path, ':');

  std::vector<std::filesystem::path> dslx_paths;
  dslx_paths.reserve(dslx_path_strs.size());
  for (const auto& path : dslx_path_strs) {
    dslx_paths.push_back(std::filesystem::path(path));
  }

  std::optional<std::string_view> top;
  if (ir_converter_options.has_top()) {
    top = ir_converter_options.top();
  }

  std::optional<std::string_view> package_name;
  if (ir_converter_options.has_package_name()) {
    package_name = ir_converter_options.package_name();
  }

  XLS_ASSIGN_OR_RETURN(WarningKindSet enabled_warnings,
                       WarningKindSetFromDisabledString(
                           ir_converter_options.disable_warnings()));

  std::string resolved_package_name;
  if (package_name.has_value()) {
    resolved_package_name = package_name.value();
  } else {
    if (paths.size() > 1) {
      return absl::InvalidArgumentError(
          "Package name must be given when multiple input paths are supplied");
    }
    // Get it from the one module name (if package name was unspecified and we
    // just have one path).
    XLS_ASSIGN_OR_RETURN(resolved_package_name, PathToName(paths[0]));
  }

  if (paths.size() > 1 && top.has_value()) {
    return absl::InvalidArgumentError(
        "Top cannot be supplied with multiple input paths (need a single input "
        "path to know where to resolve the entry function");
  }

  XLS_ASSIGN_OR_RETURN(
      DslxTypeToVerilogManager type_to_verilog,
      DslxTypeToVerilogManager::Create(absl::GetFlag(FLAGS_namespace)));

  ImportData import_data(CreateImportData(dslx_stdlib_path, dslx_paths,
                                          enabled_warnings,
                                          std::make_unique<RealFilesystem>()));
  XLS_ASSIGN_OR_RETURN(std::vector<ExplicitInput> inputs,
                       GetExplicitInputs(paths, import_data));
  std::vector<std::pair<Module*, TypeInfo*>> parsed_inputs;
  for (const ExplicitInput& input : inputs) {
    XLS_ASSIGN_OR_RETURN(ImportTokens tokens,
                         ImportTokens::FromString(*input.module_name));
    if (import_data.Contains(tokens)) {
      XLS_RETURN_IF_ERROR(import_data.vfs().FileExists(input.path));
    } else {
      XLS_ASSIGN_OR_RETURN(std::string text,
                           import_data.vfs().GetFileContents(input.path));
      XLS_RETURN_IF_ERROR(ParseAndTypecheck(text, input.path,
                                            *input.module_name, &import_data,
                                            {})
                              .status());
    }
    XLS_ASSIGN_OR_RETURN(ModuleInfo * module, import_data.Get(tokens));
    parsed_inputs.emplace_back(&module->module(), module->type_info());
  }
  std::vector<std::pair<Module*, TypeInfo*>> input_modules = parsed_inputs;
  if (inputs.size() > 1) {
    XLS_ASSIGN_OR_RETURN(
        input_modules,
        SelectExplicitModules(inputs, parsed_inputs, import_data));
  }
  // Name collisions among sum declarations must be known before any root is
  // emitted, independently of the order the caller supplied those roots.
  type_to_verilog.PrepareForModules(input_modules);

  for (const auto& [module, type_info] : input_modules) {
    for (const auto& def : module->GetTypeDefinitions()) {
      // Ignore private type definitions.
      XLS_ASSIGN_OR_RETURN(const TypeInfo::TypeSource type_definition_source,
                           type_info->ResolveTypeDefinition(def));
      if (!TypeDefinitionSourceIsPublic(type_definition_source)) {
        continue;
      }
      AstNode* def_node = TypeDefinitionToAstNode(def);
      std::optional<Type*> type_from_type_info = type_info->GetItem(def_node);
      if (!type_from_type_info.has_value()) {
        VLOG(3) << absl::StreamFormat("Skipping %s with no type info.",
                                      def_node->ToInlineString());
        continue;
      }

      VLOG(3) << absl::StreamFormat("Converting definition %s to Verilog",
                                    def_node->ToInlineString());
      XLS_RETURN_IF_ERROR(
          type_to_verilog.AddTypeForTypeDefinition(def, &import_data));
    }
  }
  std::string output;
  // Build lint waivers.
  for (std::string_view waiver : absl::GetFlag(FLAGS_lint_waivers)) {
    absl::StrAppend(&output, "// verilog_lint: waive-start ", waiver, "\n");
  }
  absl::StrAppend(&output, type_to_verilog.Emit());
  for (std::string_view waiver : absl::GetFlag(FLAGS_lint_waivers)) {
    absl::StrAppend(&output, "// verilog_lint: waive-end ", waiver, "\n");
  }
  return SetFileContents(output_file.value(), output);
}
}  // namespace
}  // namespace dslx
}  // namespace xls

int main(int argc, char* argv[]) {
  std::vector<std::string_view> args = xls::InitXls(argv[0], argc, argv);
  if (args.empty()) {
    LOG(QFATAL) << "Wrong number of command-line arguments; got " << args.size()
                << ": `" << absl::StrJoin(args, " ") << "`; want " << argv[0]
                << " <input-file>";
  }
  // "-" is a special path that is shorthand for /dev/stdin. Update here as
  // there isn't a better place later.
  for (auto& arg : args) {
    if (arg == "-") {
      arg = "/dev/stdin";
    }
  }

  return xls::ExitStatus(xls::dslx::RealMain(args));
}
