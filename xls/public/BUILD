# Copyright 2021 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Public (exported) XLS APIs.

# pytype binary only

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//xls:xls_public"],
    features = [
        "layering_check",
        "parse_headers",
    ],
    licenses = ["notice"],  # Apache 2.0
)

cc_library(
    name = "runtime_build_actions",
    srcs = ["runtime_build_actions.cc"],
    hdrs = ["runtime_build_actions.h"],
    data = [
        "//xls/dslx/ir_convert:ir_converter_main",
        "//xls/tools:opt_main",
    ],
    deps = [
        ":ir",
        "//xls/codegen:module_signature",
        "//xls/common/file:filesystem",
        "//xls/common/status:status_macros",
        "//xls/dslx:create_import_data",
        "//xls/dslx:default_dslx_stdlib_path",
        "//xls/dslx:extract_module_name",
        "//xls/dslx:import_data",
        "//xls/dslx:mangle",
        "//xls/dslx:parse_and_typecheck",
        "//xls/dslx:warning_kind",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:pos",
        "//xls/dslx/ir_convert:convert_options",
        "//xls/dslx/ir_convert:ir_converter",
        "//xls/ir",
        "//xls/passes:optimization_pass",
        "//xls/scheduling:pipeline_schedule_cc_proto",
        "//xls/tools:codegen",
        "//xls/tools:codegen_flags_cc_proto",
        "//xls/tools:opt",
        "//xls/tools:proto_to_dslx",
        "//xls/tools:scheduling_options_flags_cc_proto",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/types:span",
    ],
)

cc_test(
    name = "runtime_build_actions_test",
    srcs = ["runtime_build_actions_test.cc"],
    deps = [
        ":runtime_build_actions",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/dslx:default_dslx_stdlib_path",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "function_builder",
    hdrs = ["function_builder.h"],
    deps = [
        "//xls/ir:function_builder",
        "//xls/ir:source_location",
    ],
)

cc_library(
    name = "status_macros",
    hdrs = ["status_macros.h"],
    deps = [
        "//xls/common/status:status_macros",
    ],
)

cc_library(
    name = "status_matchers",
    testonly = True,
    hdrs = ["status_matchers.h"],
    deps = [
        "//xls/common/status:matchers",
    ],
)

cc_library(
    name = "value",
    hdrs = ["value.h"],
    deps = [
        "//xls/ir:bits",
        "//xls/ir:bits_ops",
        "//xls/ir:value",
        "//xls/ir:value_view",
    ],
)

cc_library(
    name = "xls_gunit_main",
    testonly = True,
    deps = [
        "//xls/common:xls_gunit_main",
    ],
)

py_binary(
    name = "verify_checksum",
    srcs = ["verify_checksum.py"],
    python_version = "PY3",
    srcs_version = "PY3",
    deps = [
        "//xls/common:runfiles",
        "@com_google_absl_py//absl:app",
    ],
)

cc_library(
    name = "ir",
    srcs = ["ir.cc"],
    hdrs = ["ir.h"],
    deps = [
        "//xls/ir",
        "//xls/ir:events",
    ],
)

cc_library(
    name = "ir_parser",
    srcs = ["ir_parser.cc"],
    hdrs = ["ir_parser.h"],
    deps = [
        ":ir",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_library(
    name = "c_api_impl_helpers",
    srcs = ["c_api_impl_helpers.cc"],
    hdrs = ["c_api_impl_helpers.h"],
    deps = [
        ":c_api_format_preference",
        "//xls/ir:format_preference",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_library(
    name = "c_api_format_preference",
    hdrs = ["c_api_format_preference.h"],
)

cc_library(
    name = "c_api_vast",
    srcs = ["c_api_vast.cc"],
    hdrs = ["c_api_vast.h"],
    deps = [
        ":c_api_format_preference",
        ":c_api_impl_helpers",
        "//xls/codegen/vast",
        "//xls/ir:bits",
        "//xls/ir:format_preference",
        "//xls/ir:source_location",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "c_api_dslx",
    srcs = ["c_api_dslx.cc"],
    hdrs = ["c_api_dslx.h"],
    deps = [
        ":c_api_impl_helpers",
        "//xls/common:visitor",
        "//xls/dslx:create_import_data",
        "//xls/dslx:parse_and_typecheck",
        "//xls/dslx:warning_kind",
        "//xls/dslx/type_system:unwrap_meta_type",
        "@com_google_absl//absl/types:variant",
    ],
)

cc_library(
    name = "c_api",
    srcs = ["c_api.cc"],
    hdrs = ["c_api.h"],
    deps = [
        ":c_api_dslx",
        ":c_api_format_preference",
        ":c_api_impl_helpers",
        ":c_api_vast",
        ":runtime_build_actions",
        "//xls/common:init_xls",
        "//xls/interpreter:ir_interpreter",
        "//xls/ir",
        "//xls/ir:bits",
        "//xls/ir:events",
        "//xls/ir:format_preference",
        "//xls/ir:ir_parser",
        "//xls/ir:type",
        "//xls/ir:value",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_test(
    name = "c_api_test",
    srcs = ["c_api_test.cc"],
    deps = [
        ":c_api",
        ":c_api_format_preference",
        ":c_api_vast",
        "//xls/common:xls_gunit_main",
        "//xls/common/file:filesystem",
        "//xls/common/file:temp_directory",
        "//xls/common/status:matchers",
        "//xls/dslx:default_dslx_stdlib_path",
        "@com_google_absl//absl/cleanup",
        "@com_google_googletest//:gtest",
    ],
)

cc_binary(
    name = "libxls.dylib",
    srcs = ["c_api.cc", "c_api.h"],
    linkshared = True,
    deps = [
        ":c_api_dslx",
        ":c_api_format_preference",
        ":c_api_impl_helpers",
        ":c_api_vast",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
        ":runtime_build_actions",
        "//xls/codegen/vast",
        "//xls/common:init_xls",
        "//xls/interpreter:ir_interpreter",
        "//xls/ir",
        "//xls/ir:bits",
        "//xls/ir:events",
        "//xls/ir:format_preference",
        "//xls/ir:ir_parser",
        "//xls/ir:source_location",
        "//xls/ir:type",
        "//xls/ir:value",
    ],
    copts = [
        "-fvisibility=hidden",
        "-fno-exceptions",
    ],
    target_compatible_with = select({
        "@platforms//os:osx": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }),
)

filegroup(
    name = "exported_symbols_map",
    srcs = ["exported_symbols.map"],
)

cc_binary(
    name = "libxls.so",
    additional_linker_inputs = [":exported_symbols_map"],
    linkopts = [
        "-Wl,--version-script=$(location :exported_symbols_map)",
        "-Wl,-u,xls_bits_free",
        "-Wl,-u,xls_convert_dslx_path_to_ir",
        "-Wl,-u,xls_convert_dslx_to_ir",
        "-Wl,-u,xls_c_str_free",
        "-Wl,-u,xls_format_preference_from_string",
        "-Wl,-u,xls_function_get_name",
        "-Wl,-u,xls_function_get_type",
        "-Wl,-u,xls_function_type_to_string",
        "-Wl,-u,xls_init_xls",
        "-Wl,-u,xls_interpret_function",
        "-Wl,-u,xls_mangle_dslx_name",
        "-Wl,-u,xls_optimize_ir",
        "-Wl,-u,xls_package_free",
        "-Wl,-u,xls_package_get_function",
        "-Wl,-u,xls_package_get_type_for_value",
        "-Wl,-u,xls_package_to_string",
        "-Wl,-u,xls_parse_ir_package",
        "-Wl,-u,xls_parse_typed_value",
        "-Wl,-u,xls_type_to_string",
        "-Wl,-u,xls_value_eq",
        "-Wl,-u,xls_value_free",
        "-Wl,-u,xls_value_get_bits",
        "-Wl,-u,xls_value_make_false",
        "-Wl,-u,xls_value_make_token",
        "-Wl,-u,xls_value_make_true",
        "-Wl,-u,xls_value_to_string",
        "-Wl,-u,xls_value_to_string_format_preference",
        "-Wl,-u,xls_dslx_enum_def_get_identifier",
        "-Wl,-u,xls_dslx_enum_def_get_member",
        "-Wl,-u,xls_dslx_enum_def_get_member_count",
        "-Wl,-u,xls_dslx_enum_member_get_name",
        "-Wl,-u,xls_dslx_enum_member_get_value",
        "-Wl,-u,xls_dslx_import_data_create",
        "-Wl,-u,xls_dslx_import_data_free",
        "-Wl,-u,xls_dslx_interp_value_convert_to_ir",
        "-Wl,-u,xls_dslx_interp_value_free",
        "-Wl,-u,xls_dslx_module_get_type_definition_as_enum_def",
        "-Wl,-u,xls_dslx_module_get_type_definition_as_struct_def",
        "-Wl,-u,xls_dslx_module_get_type_definition_count",
        "-Wl,-u,xls_dslx_module_get_type_definition_kind",
        "-Wl,-u,xls_dslx_parse_and_typecheck",
        "-Wl,-u,xls_dslx_struct_def_get_identifier",
        "-Wl,-u,xls_dslx_struct_def_get_member",
        "-Wl,-u,xls_dslx_struct_member_get_name",
        "-Wl,-u,xls_dslx_struct_member_get_type",
        "-Wl,-u,xls_dslx_typechecked_module_free",
        "-Wl,-u,xls_dslx_typechecked_module_get_module",
        "-Wl,-u,xls_dslx_typechecked_module_get_type_info",
        "-Wl,-u,xls_dslx_type_get_total_bit_count",
        "-Wl,-u,xls_dslx_type_info_get_const_expr",
        "-Wl,-u,xls_dslx_type_info_get_type_enum_def",
        "-Wl,-u,xls_dslx_type_info_get_type_struct_def",
        "-Wl,-u,xls_dslx_type_info_get_type_type_annotation",
    ],
    linkshared = True,
    deps = [
        ":c_api",
    ],
)
