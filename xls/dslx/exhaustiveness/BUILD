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

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//xls:xls_internal"],
    licenses = ["notice"],  # Apache 2.0
)

cc_library(
    name = "exhaustiveness",
    srcs = [
        "exhaustiveness.h",
        "exhaustiveness.cc",
    ],
    deps = [
        "//xls/dslx:interp_value",
        "//xls/dslx/type_system:type",
        "//xls/dslx/type_system:type_info",
    ],
)

cc_test(
    name = "exhaustiveness_test",
    srcs = ["exhaustiveness_test.cc"],
    deps = [
        ":exhaustiveness",
        "@com_google_googletest//:gtest",
        "//xls/common:xls_gunit_main",
        "//xls/common/fuzzing:fuzztest",
    ],
)

cc_library(
    name = "match_exhaustiveness_checker",
    srcs = ["match_exhaustiveness_checker.cc"],
    hdrs = ["match_exhaustiveness_checker.h"],
    deps = [
        ":exhaustiveness",
        "//xls/dslx/type_system:type_info",
        "//xls/dslx/type_system:type",
    ],
)

cc_test(
    name = "exhaustiveness_match_test",
    srcs = ["exhaustiveness_match_test.cc"],
    deps = [
        ":match_exhaustiveness_checker",
        "//xls/dslx:parse_and_typecheck",
        "@com_google_googletest//:gtest",
        "//xls/common:xls_gunit_main",
        "//xls/dslx:create_import_data",
        "//xls/common/status:matchers",
    ],
)
