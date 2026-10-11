#!/bin/bash
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
# Bazel --run_under wrapper for ITF QEMU tests with two network interfaces. Same as
# score_itf/scripts/run_under_qemu.sh (private network namespace, tap0 for SSH and the first io-sock stack),
# plus tap1 for the dedicated logging stack of the second io-sock instance:
#
#   tap0  169.254.21.88/16   target 169.254.158.190 (vtnet0, default io-sock stack)
#   tap1  192.168.77.1/24    target 192.168.77.2    (vtnet1, io-sock stack with prefix /alt)
#
# Use it together with feature_integration_tests/configs/qemu_bridge_2nic_config.json:
#   bazel test --config=itf-qnx-x86_64 --run_under=$PWD/qnx_iosock_dlt/tools/run_under_qemu_2nic.sh \
#       --test_arg=--qemu-config=$PWD/feature_integration_tests/configs/qemu_bridge_2nic_config.json ...
# (the experiment "S" of tools/run_experiments.sh does this).

set -euo pipefail

CMD_UNDECLARED_OUTPUTS_DIR="true"
if [[ -n "${TEST_UNDECLARED_OUTPUTS_DIR:-}" ]]; then
    CMD_UNDECLARED_OUTPUTS_DIR="export TEST_UNDECLARED_OUTPUTS_DIR=$TEST_UNDECLARED_OUTPUTS_DIR"
else
    TEST_UNDECLARED_OUTPUTS_DIR="/tmp"
fi

# Temp directory mounted as /var/run inside the unshared user namespace
TMP_VAR_RUN_DIR=${TEST_TMPDIR-$(mktemp -d)}/tmp_var_run
mkdir -p "${TMP_VAR_RUN_DIR}"

CMD_CREATE_VAR_DIR="mount --bind $TMP_VAR_RUN_DIR /var/run"

CMD_LO_INTERFACE="\
ip link set lo up"

CMD_TAP_INTERFACES="\
ip tuntap add mode tap tap0 &&
ip addr add 169.254.21.88/16 broadcast 160.48.199.255 dev tap0 &&
ip link set dev tap0 up &&
ip tuntap add mode tap tap1 &&
ip addr add 192.168.77.1/24 dev tap1 &&
ip link set dev tap1 up"

unshare -m -U -n --map-root-user /bin/bash -c \
    "${CMD_UNDECLARED_OUTPUTS_DIR} &&
  ${CMD_CREATE_VAR_DIR} &&
  ${CMD_LO_INTERFACE} &&
  ${CMD_TAP_INTERFACES} &&
  ${*}"
