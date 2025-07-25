# Copyright 2023 The XLS Authors
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

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

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
    name = "extract_nodes",
    srcs = ["extract_nodes.cc"],
    hdrs = ["extract_nodes.h"],
    deps = [
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/ir:source_location",
        "//xls/ir:type",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_test(
    name = "extract_nodes_test",
    srcs = ["extract_nodes_test.cc"],
    deps = [
        ":extract_nodes",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_matcher",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "grpc_synthesizer",
    srcs = ["grpc_synthesizer.cc"],
    hdrs = ["grpc_synthesizer.h"],
    deps = [
        ":synthesizer",
        "//xls/common:casts",
        "//xls/common:module_initializer",
        "//xls/common/status:status_macros",
        "//xls/synthesis:synthesis_cc_proto",
        "//xls/synthesis:synthesis_client",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status:statusor",
    ],
    alwayslink = True,  # Always link because it has a module-level initialization that registers the synthesizer.
)

cc_library(
    name = "synthesized_delay_diff_utils",
    srcs = ["synthesized_delay_diff_utils.cc"],
    hdrs = ["synthesized_delay_diff_utils.h"],
    deps = [
        ":synthesizer",
        "//xls/common/status:status_macros",
        "//xls/estimators/delay_model:analyze_critical_path",
        "//xls/estimators/delay_model:delay_estimator",
        "//xls/ir",
        "//xls/scheduling:extract_stage",
        "//xls/scheduling:pipeline_schedule",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_test(
    name = "synthesized_delay_diff_utils_test",
    srcs = ["synthesized_delay_diff_utils_test.cc"],
    deps = [
        ":synthesized_delay_diff_utils",
        ":synthesizer",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/estimators/delay_model:analyze_critical_path",
        "//xls/ir",
        "//xls/ir:ir_test_base",
        "//xls/scheduling:pipeline_schedule",
        "//xls/scheduling:run_pipeline_schedule",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "synthesizer",
    srcs = ["synthesizer.cc"],
    hdrs = ["synthesizer.h"],
    deps = [
        ":extract_nodes",
        "//xls/codegen:block_conversion",
        "//xls/codegen:block_generator",
        "//xls/codegen:codegen_options",
        "//xls/codegen:codegen_pass",
        "//xls/common:casts",
        "//xls/common:thread",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/scheduling:pipeline_schedule",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/base:no_destructor",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/types:span",
    ],
)

cc_test(
    name = "synthesizer_test",
    srcs = ["synthesizer_test.cc"],
    data = glob([
        "testdata/synthesizer_test_*",
    ]),
    deps = [
        ":synthesizer",
        "//xls/common:golden_files",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "yosys_synthesizer",
    srcs = ["yosys_synthesizer.cc"],
    hdrs = ["yosys_synthesizer.h"],
    deps = [
        ":synthesizer",
        "//xls/common:casts",
        "//xls/common:module_initializer",
        "//xls/common/status:status_macros",
        "//xls/scheduling:scheduling_options",
        "//xls/synthesis:synthesis_cc_proto",
        "//xls/synthesis/yosys:yosys_synthesis_service",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
    alwayslink = True,  # Always link because it has a module-level initialization that registers the synthesizer.
)

cc_library(
    name = "delay_manager",
    srcs = ["delay_manager.cc"],
    hdrs = ["delay_manager.h"],
    deps = [
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/estimators/delay_model:delay_estimator",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:schedule_graph",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_test(
    name = "delay_manager_test",
    srcs = ["delay_manager_test.cc"],
    deps = [
        ":delay_manager",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/container:flat_hash_map",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "delay_synthesis_service",
    srcs = ["delay_synthesis_service.cc"],
    hdrs = ["delay_synthesis_service.h"],
    deps = [
        ":synthesizer",
        "//xls/synthesis:synthesis_cc_proto",
        "//xls/synthesis:synthesis_service_cc_grpc",
        "@com_google_absl//absl/status:statusor",
        "@grpc//:grpc++",
    ],
)

cc_library(
    name = "node_cut",
    srcs = ["node_cut.cc"],
    hdrs = ["node_cut.h"],
    deps = [
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:schedule_graph",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_test(
    name = "node_cut_test",
    srcs = ["node_cut_test.cc"],
    deps = [
        ":node_cut",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "//xls/scheduling:scheduling_options",
        "@com_google_absl//absl/container:flat_hash_set",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "iterative_sdc_scheduler",
    srcs = ["iterative_sdc_scheduler.cc"],
    hdrs = ["iterative_sdc_scheduler.h"],
    deps = [
        ":delay_manager",
        ":node_cut",
        ":synthesizer",
        ":yosys_synthesizer",
        "//xls/common/logging:log_lines",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:schedule_graph",
        "//xls/scheduling:schedule_util",
        "//xls/scheduling:scheduling_options",
        "//xls/scheduling:sdc_scheduler",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/types:span",
        "@com_google_ortools//ortools/math_opt/cpp:math_opt",
    ],
)
