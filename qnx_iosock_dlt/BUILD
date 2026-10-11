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
# Host-side (Linux) targets of the DLT io-sock module: portable core, unit tests, loopback check.
# The QNX module itself is built with the QNX make framework (see README.md, qnx_module/).
#
#   bazel test --config=linux-x86_64 //qnx_iosock_dlt/...

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_python//python:py_binary.bzl", "py_binary")
load("@rules_python//python:py_test.bzl", "py_test")

# No exceptions/RTTI, no hosted-library assumptions and no loop-to-memcpy rewriting, so the linked
# core references no external symbol (checked by :core_freestanding_test).
CORE_COPTS = [
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-threadsafe-statics",
    "-fno-stack-protector",
    "-ffreestanding",
    "-fno-tree-loop-distribute-patterns",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
]

cc_library(
    name = "dlt_core",
    srcs = glob(["src/core/*.cpp"]),
    hdrs = glob(["include/dlt/*.h"]),
    copts = CORE_COPTS,
    includes = ["include"],
)

cc_test(
    name = "dlt_wire_formatter_test",
    size = "small",
    srcs = ["tests/dlt_wire_formatter_test.cpp"],
    deps = [":dlt_core"],
)

# The ring header alone (valid C and C++), shared by the core, the producer tool and the tests.
cc_library(
    name = "shm_ring",
    hdrs = ["include/dlt/shm_ring.h"],
    includes = ["include"],
    visibility = ["//qnx_iosock_dlt:__subpackages__"],
)

cc_test(
    name = "shm_ring_test",
    size = "small",
    srcs = ["tests/shm_ring_test.cpp"],
    linkopts = ["-pthread"],
    deps = [":dlt_core"],
)

cc_binary(
    name = "host_udp_sender",
    srcs = [
        "tests/host_udp_sender.cpp",
        "tests/host_udp_transport.h",
    ],
    deps = [":dlt_core"],
)

# Plays the role of the io-sock module for the ring input: creates the ring, reads it, sends UDP.
cc_binary(
    name = "host_ring_bridge",
    srcs = [
        "tests/host_ring_bridge.cpp",
        "tests/host_udp_transport.h",
    ],
    deps = [":dlt_core"],
)

# Writes records into the ring; the same source is built for QNX with `make qnx-tools`.
cc_binary(
    name = "ring_producer",
    srcs = ["tools/ring_producer.c"],
    deps = [":shm_ring"],
)

# Userspace twin of the module: same core, ring and batching, but sends with sendmsg() through a BSD socket.
# Built for QNX with `make qnx-tools`; used to measure what the removed socket IPC hop costs.
cc_binary(
    name = "ring_forwarder",
    srcs = [
        "tools/posix_udp_transport.h",
        "tools/ring_forwarder.cpp",
    ],
    deps = [":dlt_core"],
)

py_binary(
    name = "dlt_udp_check",
    srcs = ["tools/dlt_udp_check.py"],
    main = "tools/dlt_udp_check.py",
)

# Formatter -> real UDP loopback -> independent Python decoder.
py_test(
    name = "host_e2e_test",
    size = "small",
    srcs = ["tools/dlt_udp_check.py"],
    args = [
        "--bind",
        "127.0.0.1",
        "--port",
        "39490",
        "--count",
        "5",
        "--timeout",
        "10",
        "--exec",
        "'$(rootpath :host_udp_sender) 127.0.0.1 39490 5'",
    ],
    data = [":host_udp_sender"],
    main = "tools/dlt_udp_check.py",
    tags = ["exclusive"],
)

# Producer -> shared-memory ring -> core (module stand-in) -> real UDP loopback -> Python decoder.
py_test(
    name = "host_ring_e2e_test",
    size = "small",
    srcs = ["tools/dlt_udp_check.py"],
    args = [
        "--bind",
        "127.0.0.1",
        "--port",
        "39491",
        "--count",
        "20",
        "--timeout",
        "20",
        "--expect-text",
        "ring-dlt",
        "--exec",
        "'sh $(rootpath tests/host_ring_e2e.sh) $(rootpath :host_ring_bridge) $(rootpath :ring_producer) 39491 20'",
    ],
    data = [
        "tests/host_ring_e2e.sh",
        ":host_ring_bridge",
        ":ring_producer",
    ],
    main = "tools/dlt_udp_check.py",
    tags = ["exclusive"],
)

# The same with several DLT messages per datagram: the decoder has to split the datagrams.
py_test(
    name = "host_ring_e2e_batch_test",
    size = "small",
    srcs = ["tools/dlt_udp_check.py"],
    args = [
        "--bind",
        "127.0.0.1",
        "--port",
        "39492",
        "--count",
        "20",
        "--timeout",
        "20",
        "--expect-text",
        "ring-dlt",
        "--exec",
        "'sh $(rootpath tests/host_ring_e2e.sh) $(rootpath :host_ring_bridge) $(rootpath :ring_producer) 39492 20 1472 0'",
    ],
    data = [
        "tests/host_ring_e2e.sh",
        ":host_ring_bridge",
        ":ring_producer",
    ],
    main = "tools/dlt_udp_check.py",
    tags = ["exclusive"],
)

# Producer -> ring -> ring_forwarder (sendmsg through a BSD socket) -> UDP loopback -> Python decoder.
py_test(
    name = "host_ring_forwarder_e2e_test",
    size = "small",
    srcs = ["tools/dlt_udp_check.py"],
    args = [
        "--bind",
        "127.0.0.1",
        "--port",
        "39493",
        "--count",
        "20",
        "--timeout",
        "20",
        "--expect-text",
        "ring-dlt",
        "--exec",
        "'sh $(rootpath tests/host_ring_forwarder_e2e.sh) $(rootpath :ring_forwarder) $(rootpath :ring_producer) 39493 20'",
    ],
    data = [
        "tests/host_ring_forwarder_e2e.sh",
        ":ring_forwarder",
        ":ring_producer",
    ],
    main = "tools/dlt_udp_check.py",
    tags = ["exclusive"],
)

# The same with several DLT messages per datagram.
py_test(
    name = "host_ring_forwarder_e2e_batch_test",
    size = "small",
    srcs = ["tools/dlt_udp_check.py"],
    args = [
        "--bind",
        "127.0.0.1",
        "--port",
        "39494",
        "--count",
        "20",
        "--timeout",
        "20",
        "--expect-text",
        "ring-dlt",
        "--exec",
        "'sh $(rootpath tests/host_ring_forwarder_e2e.sh) $(rootpath :ring_forwarder) $(rootpath :ring_producer) 39494 20 1472 0'",
    ],
    data = [
        "tests/host_ring_forwarder_e2e.sh",
        ":ring_forwarder",
        ":ring_producer",
    ],
    main = "tools/dlt_udp_check.py",
    tags = ["exclusive"],
)

# The linked core must have no undefined symbols and no static constructors.
py_test(
    name = "core_freestanding_test",
    size = "small",
    srcs = ["tools/check_freestanding.py"],
    args = ["$(locations :dlt_core)"],
    data = [":dlt_core"],
    main = "tools/check_freestanding.py",
)
