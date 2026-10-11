# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
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
# Copyright (c) 2026 Qorix
# SPDX-License-Identifier: Apache-2.0
#
# mw::log recorder that feeds the DLT shared-memory ring of the io-sock module. Plug it into
# score_logging as the custom recorder:
#
#   --@score_logging//score/mw/log/flags:KCustom_Logging=True
#   --@score_logging//score/mw/log/flags:custom_recorder_impl=//qnx_iosock_dlt/mwlog_ring:recorder
#
# and set the application's log mode to "kCustom" (not combined with "kRemote") in its logging configuration.
# Needs patches/logging/003-expose-dlt-content-formatting.patch (visibility of the DLT formatter).
#
#   bazel test --config=linux-x86_64 //qnx_iosock_dlt/mwlog_ring:ring_recorder_test

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

exports_files(
    [
        "mwlog_bench_remote.json",
        "mwlog_bench_remote_large.json",
        "mwlog_bench_ring.json",
        "mwlog_ring_config.json",
    ],
    visibility = ["//visibility:public"],
)

# Load generator for the DataRouter versus ring comparison (config selects kRemote or kCustom).
cc_binary(
    name = "mwlog_bench_app",
    srcs = ["mwlog_bench_app.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@score_baselibs//score/mw/log",
        "@score_logging//score/mw/log/backend:custom",
        "@score_logging//score/mw/log/backend:remote",
    ],
)

# Example mw::log application for the QEMU end-to-end test. Link the custom backend next to the console
# bundle; without KCustom_Logging=True the custom backend is empty and the app logs to the console.
cc_binary(
    name = "mwlog_ring_app",
    srcs = ["mwlog_ring_app.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@score_baselibs//score/mw/log",
        "@score_logging//score/mw/log/backend:custom",
    ],
)

cc_library(
    name = "recorder",
    srcs = [
        "ring_recorder.cpp",
        "ring_recorder_factory.cpp",
        "shm_ring_writer.cpp",
    ],
    hdrs = [
        "custom_recorder.h",
        "ring_recorder.h",
        "ring_recorder_factory.h",
        "shm_ring_writer.h",
    ],
    # custom_registrant.cpp includes "custom_recorder.h" without a path.
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "//qnx_iosock_dlt:shm_ring",
        "@score_baselibs//score/mw/log:recorder",
        "@score_baselibs//score/mw/log/configuration",
        "@score_baselibs//score/mw/log/detail:circular_allocator",
        "@score_baselibs//score/mw/log/detail:dlt_argument_counter",
        "@score_baselibs//score/mw/log/detail:initialization_reporter",
        "@score_baselibs//score/mw/log/detail:log_data_types",
        "@score_baselibs//score/mw/log/detail:log_recorder_factory",
        "@score_baselibs//score/mw/log/detail:types_and_errors",
        "@score_logging//score/mw/log/detail/common:dlt_content_formatting",
    ],
)

cc_test(
    name = "ring_recorder_test",
    size = "small",
    srcs = ["ring_recorder_test.cpp"],
    linkopts = ["-pthread"],
    deps = [":recorder"],
)
