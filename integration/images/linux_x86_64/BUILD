# *******************************************************************************
# Copyright (c) 2025 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@score_tooling//:defs.bzl", "rust_coverage_report", )

alias(
    name = "image",
    actual = "//showcases:showcases_all",
    visibility = ["//visibility:public"],
)

sh_binary(
    name = "run",
    srcs = ["//runners/docker_x86_64/scripts:run_docker.sh"],
    args = [
        "$(location :image)",
    ],
    data = [
        ":image",
    ],
)

rust_coverage_report(
    name = "per_rust_coverage",
    bazel_configs = [
        "ferrocene-coverage",
    ],
    query = 'kind("rust_test", @score_persistency//src/rust/...)',
    visibility = ["//visibility:public"],
)

# alias(
#     name = "rust_coverage",
#     actual = ":run",
#     visibility = ["//visibility:public"],
# )

# test_suite(
#     name = "ut_int",
#     tests = [
#         "@score_persistency//src/rust/rust_kvs:tests",
#     ],
# )
