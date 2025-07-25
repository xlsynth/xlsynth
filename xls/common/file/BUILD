# Copyright 2020 The XLS Authors
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

# Common utilities for filesystem interaction shared among XLA subfolders.

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
# Load proto_library
# cc_proto_library is used in this file

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
    name = "file_descriptor",
    srcs = ["file_descriptor.cc"],
    hdrs = ["file_descriptor.h"],
    deps = [
        "//xls/common/status:error_code_to_status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_test(
    name = "file_descriptor_test",
    srcs = ["file_descriptor_test.cc"],
    deps = [
        ":file_descriptor",
        ":filesystem",
        ":temp_directory",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@com_google_absl//absl/status:status_matchers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "filesystem",
    srcs = ["filesystem.cc"],
    hdrs = ["filesystem.h"],
    deps = [
        ":temp_file",
        "//xls/common/status:error_code_to_status",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_protobuf//:protobuf",
        "@com_google_protobuf//src/google/protobuf/io",
        "@com_google_protobuf//src/google/protobuf/io:tokenizer",
        "@re2",
    ],
)

cc_test(
    name = "filesystem_test",
    srcs = ["filesystem_test.cc"],
    deps = [
        ":filesystem",
        ":filesystem_test_cc_proto",
        ":temp_directory",
        ":temp_file",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:status_matchers",
        "@com_google_absl//absl/status:statusor",
        "@googletest//:gtest",
    ],
)

proto_library(
    name = "filesystem_test_proto",
    srcs = ["filesystem_test.proto"],
)

cc_proto_library(
    name = "filesystem_test_cc_proto",
    deps = [":filesystem_test_proto"],
)

cc_library(
    name = "get_runfile_path",
    srcs = ["get_runfile_path.cc"],
    hdrs = ["get_runfile_path.h"],
    visibility = ["//xls:xls_utility_users"],
    deps = [
        ":filesystem",
        "//xls/common/status:status_macros",
        "@bazel_tools//tools/cpp/runfiles",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_test(
    name = "get_runfile_path_test",
    srcs = ["get_runfile_path_test.cc"],
    data = [
        "get_runfile_path_test.cc",
        "@llvm-project//clang:builtin_headers_gen",
    ],
    deps = [
        ":filesystem",
        ":get_runfile_path",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@com_google_absl//absl/status:status_matchers",
        "@com_google_absl//absl/status:statusor",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "path",
    srcs = ["path.cc"],
    hdrs = ["path.h"],
    deps = [
        "//xls/common/status:error_code_to_status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_test(
    name = "path_test",
    srcs = ["path_test.cc"],
    deps = [
        ":path",
        "//xls/common:xls_gunit_main",
        "@com_google_absl//absl/status:status_matchers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "temp_directory",
    srcs = ["temp_directory.cc"],
    hdrs = ["temp_directory.h"],
    visibility = ["//xls:xls_utility_users"],
    deps = [
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "temp_directory_test",
    srcs = ["temp_directory_test.cc"],
    deps = [
        ":filesystem",
        ":temp_directory",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "temp_file",
    srcs = ["temp_file.cc"],
    hdrs = ["temp_file.h"],
    deps = [
        "//xls/common:strerror",
        "//xls/common/status:status_macros",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "named_pipe",
    srcs = ["named_pipe.cc"],
    hdrs = ["named_pipe.h"],
    deps = [
        ":file_descriptor",
        "//xls/common/status:error_code_to_status",
        "//xls/common/status:status_macros",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_test(
    name = "named_pipe_test",
    srcs = ["named_pipe_test.cc"],
    deps = [
        ":filesystem",
        ":named_pipe",
        ":temp_directory",
        "//xls/common:thread",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:status_matchers",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@googletest//:gtest",
    ],
)

cc_test(
    name = "temp_file_test",
    srcs = ["temp_file_test.cc"],
    deps = [
        ":filesystem",
        ":temp_file",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:status_matchers",
        "@com_google_absl//absl/strings",
        "@googletest//:gtest",
    ],
)
