# Copyright 2024 The XLS Authors
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

load("@bazel_skylib//rules:diff_test.bzl", "diff_test")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//xls/build_rules:xls_build_defs.bzl", "xls_dslx_verilog")
load(
    "//xls/build_rules:xls_dslx_rules.bzl",
    "xls_dslx_library",
    "xls_dslx_to_verilog_package",
)

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//xls:xls_internal"],
    features = [
        "layering_check",
        "parse_headers",
    ],
    licenses = ["notice"],  # Apache 2.0
)

cc_library(
    name = "verilog_sum_naming",
    srcs = ["verilog_sum_naming.cc"],
    hdrs = ["verilog_sum_naming.h"],
    visibility = ["//visibility:private"],
    deps = [
        "//xls/codegen/vast",
        "//xls/common/status:status_macros",
        "//xls/dslx:channel_direction",
        "//xls/dslx:interp_value",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:module",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system_v2:type_annotation_utils",
        "//xls/ir:bits",
        "//xls/ir:name_uniquer",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@boringssl//:crypto",
    ],
)

cc_test(
    name = "verilog_sum_naming_test",
    srcs = ["verilog_sum_naming_test.cc"],
    deps = [
        ":verilog_sum_naming",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/common/status:status_macros",
        "//xls/dslx:create_import_data",
        "//xls/dslx:import_data",
        "//xls/dslx:interp_value",
        "//xls/dslx:parse_and_typecheck",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:module",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system:type_info",
        "//xls/ir:name_uniquer",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "dslx_to_verilog",
    srcs = ["dslx_to_verilog.cc"],
    hdrs = ["dslx_to_verilog.h"],
    deps = [
        ":verilog_sum_naming",
        "//xls/codegen/vast",
        "//xls/common:visitor",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/dslx:import_data",
        "//xls/dslx:interp_value",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:module",
        "//xls/dslx/frontend:pos",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system:type_info",
        "//xls/ir:bits",
        "//xls/ir:name_uniquer",
        "//xls/ir:source_location",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@abseil-cpp//absl/types:variant",
    ],
)

cc_test(
    name = "dslx_to_verilog_test",
    srcs = ["dslx_to_verilog_test.cc"],
    data = glob(["testdata/*"]),
    deps = [
        ":dslx_to_verilog",
        "//xls/common:golden_files",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/dslx:create_import_data",
        "//xls/dslx:import_data",
        "//xls/dslx:parse_and_typecheck",
        "//xls/dslx:virtualizable_file_system",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:module",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system:type_info",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@googletest//:gtest",
        "@re2",
    ],
)

cc_binary(
    name = "dslx_to_verilog_main",
    srcs = ["dslx_to_verilog_main.cc"],
    deps = [
        ":dslx_to_verilog",
        "//xls/codegen/vast",
        "//xls/common:exit_status",
        "//xls/common:init_xls",
        "//xls/common:visitor",
        "//xls/common/file:filesystem",
        "//xls/common/status:status_macros",
        "//xls/dslx:command_line_utils",
        "//xls/dslx:create_import_data",
        "//xls/dslx:import_data",
        "//xls/dslx:import_routines",
        "//xls/dslx:parse_and_typecheck",
        "//xls/dslx:virtualizable_file_system",
        "//xls/dslx:warning_kind",
        "//xls/dslx/frontend:ast",
        "//xls/dslx/frontend:module",
        "//xls/dslx/frontend:pos",
        "//xls/dslx/frontend:scanner",
        "//xls/dslx/frontend:token",
        "//xls/dslx/ir_convert:ir_converter_options_flags",
        "//xls/dslx/ir_convert:ir_converter_options_flags_cc_proto",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system:type_info",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@abseil-cpp//absl/types:variant",
    ],
)

# Target to test dslx_to_verilog_main against golden.
xls_dslx_to_verilog_package(
    name = "memory_proc_pkg",
    library = "//xls/examples:memory_proc_dslx",
    lint_waivers = ["struct-union-name-style"],
    namespace = "memory_proc",
    package_file = "memory_proc_pkg.sv",
)

diff_test(
    name = "dslx_to_verilog_main_memory_proc_test",
    file1 = "testdata/memory_proc_pkg.sv",
    file2 = ":memory_proc_pkg.sv",
)

xls_dslx_library(
    name = "semantic_sum_dslx",
    srcs = ["testdata/semantic_sum_pkg.x"],
)

xls_dslx_library(
    name = "semantic_sum_lowering_dslx",
    srcs = ["testdata/semantic_sum_lowering.x"],
    deps = [":semantic_sum_dslx"],
)

xls_dslx_verilog(
    name = "semantic_sum_producer_rtl",
    codegen_args = {
        "generator": "combinational",
        "module_name": "semantic_sum_producer",
        "use_system_verilog": "false",
    },
    dslx_top = "make_message",
    enable_generated_file = False,
    library = ":semantic_sum_lowering_dslx",
    verilog_file = "semantic_sum_producer.v",
)

xls_dslx_verilog(
    name = "semantic_sum_matcher_rtl",
    codegen_args = {
        "generator": "combinational",
        "module_name": "semantic_sum_matcher",
        "use_system_verilog": "false",
    },
    dslx_top = "read_message",
    enable_generated_file = False,
    library = ":semantic_sum_lowering_dslx",
    verilog_file = "semantic_sum_matcher.v",
)

xls_dslx_verilog(
    name = "semantic_sum_sparse_outer_producer_rtl",
    codegen_args = {
        "generator": "combinational",
        "module_name": "semantic_sum_sparse_outer_producer",
        "use_system_verilog": "false",
    },
    dslx_top = "make_sparse_and_outer",
    enable_generated_file = False,
    library = ":semantic_sum_lowering_dslx",
    verilog_file = "semantic_sum_sparse_outer_producer.v",
)

xls_dslx_verilog(
    name = "semantic_sum_sparse_outer_matcher_rtl",
    codegen_args = {
        "generator": "combinational",
        "module_name": "semantic_sum_sparse_outer_matcher",
        "use_system_verilog": "false",
    },
    dslx_top = "read_sparse_and_outer",
    enable_generated_file = False,
    library = ":semantic_sum_lowering_dslx",
    verilog_file = "semantic_sum_sparse_outer_matcher.v",
)

xls_dslx_to_verilog_package(
    name = "semantic_sum_pkg",
    library = ":semantic_sum_dslx",
    lint_waivers = ["struct-union-name-style"],
    namespace = "semantic_sum",
    package_file = "semantic_sum_pkg.sv",
)

diff_test(
    name = "dslx_to_verilog_main_semantic_sum_test",
    file1 = "testdata/semantic_sum_pkg.sv",
    file2 = ":semantic_sum_pkg.sv",
)

sh_test(
    name = "dslx_to_verilog_main_multifile_sum_test",
    srcs = ["testdata/multifile_sum.sh"],
    args = [
        "$(rootpath :dslx_to_verilog_main)",
        "$(rootpath testdata/sum_input_a.x)",
        "$(rootpath testdata/sum_input_b.x)",
        "$(rootpath @com_icarus_iverilog//:iverilog)",
        "$(rootpath @com_icarus_iverilog//:vvp)",
    ],
    data = [
        ":dslx_to_verilog_main",
        "testdata/sum_input_a.x",
        "testdata/sum_input_b.x",
        "@com_icarus_iverilog//:iverilog",
        "@com_icarus_iverilog//:vvp",
    ],
)

# Exercise the actual generated package and DSLX RTL in normal CI using the
# repository's four-state simulator.
sh_test(
    name = "dslx_to_verilog_semantic_sum_consumer_test",
    srcs = ["testdata/semantic_sum_consumer.sh"],
    args = [
        "$(rootpath :semantic_sum_pkg.sv)",
        "$(rootpath testdata/semantic_sum_consumer.sv)",
        "$(rootpath :semantic_sum_producer.v)",
        "$(rootpath :semantic_sum_matcher.v)",
        "$(rootpath :semantic_sum_sparse_outer_producer.v)",
        "$(rootpath :semantic_sum_sparse_outer_matcher.v)",
        "$(rootpath @com_icarus_iverilog//:iverilog)",
        "$(rootpath @com_icarus_iverilog//:vvp)",
    ],
    data = [
        ":semantic_sum_matcher.v",
        ":semantic_sum_pkg.sv",
        ":semantic_sum_producer.v",
        ":semantic_sum_sparse_outer_matcher.v",
        ":semantic_sum_sparse_outer_producer.v",
        "testdata/semantic_sum_consumer.sv",
        "@com_icarus_iverilog//:iverilog",
        "@com_icarus_iverilog//:vvp",
    ],
)

# Also check implicit signedness and ordinary enum assignment of nested packed
# member selects on four-state simulators such as Icarus 13 or VCS. Icarus 12
# requires explicit casts for those operations. Invoke with
# --test_env=XLS_SV_SIMULATOR=/path/to/iverilog (or /path/to/vcs).
sh_test(
    name = "dslx_to_verilog_semantic_sum_consumer_extended_test",
    srcs = ["testdata/semantic_sum_consumer.sh"],
    args = [
        "$(rootpath :semantic_sum_pkg.sv)",
        "$(rootpath testdata/semantic_sum_consumer.sv)",
        "$(rootpath :semantic_sum_producer.v)",
        "$(rootpath :semantic_sum_matcher.v)",
        "$(rootpath :semantic_sum_sparse_outer_producer.v)",
        "$(rootpath :semantic_sum_sparse_outer_matcher.v)",
    ],
    data = [
        ":semantic_sum_matcher.v",
        ":semantic_sum_pkg.sv",
        ":semantic_sum_producer.v",
        ":semantic_sum_sparse_outer_matcher.v",
        ":semantic_sum_sparse_outer_producer.v",
        "testdata/semantic_sum_consumer.sv",
    ],
    tags = [
        "local",
        "manual",
    ],
)

xls_dslx_library(
    name = "semantic_sum_name_scope_dslx",
    srcs = ["testdata/semantic_sum_name_scope.x"],
)

xls_dslx_to_verilog_package(
    name = "semantic_sum_name_scope_pkg",
    library = ":semantic_sum_name_scope_dslx",
    namespace = "semantic_sum_name_scope",
    package_file = "semantic_sum_name_scope_pkg.sv",
)

# Icarus rejects legal packed members that share a name with a package typedef.
# Check semantic name resolution with a local Slang installation instead; invoke
# with --test_env=XLS_SV_SLANG=/path/to/slang.
sh_test(
    name = "dslx_to_verilog_semantic_sum_name_scope_test",
    srcs = ["testdata/semantic_sum_name_scope.sh"],
    args = [
        "$(rootpath :semantic_sum_name_scope_pkg.sv)",
        "$(rootpath testdata/semantic_sum_name_scope.sv)",
    ],
    data = [
        ":semantic_sum_name_scope_pkg.sv",
        "testdata/semantic_sum_name_scope.sv",
    ],
    tags = [
        "local",
        "manual",
    ],
)
