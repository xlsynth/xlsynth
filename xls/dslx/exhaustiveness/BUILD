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
