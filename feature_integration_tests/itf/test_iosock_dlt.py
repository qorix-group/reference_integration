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
"""ITF test for the QNX io-sock DLT module (qnx_iosock_dlt).

Uploads the module to the QEMU target, loads it into the running io-sock, captures the DLT stream on
the host and checks the module's sysctl counters. The module is built separately with the QNX SDP
(`make qnx-module` in qnx_iosock_dlt/); its path is passed in IOSOCK_DLT_MODULE, otherwise the tests
are skipped:

    IOSOCK_DLT_MODULE=$PWD/qnx_iosock_dlt/qnx_module/mods-score-dlt.so \\
    bazel test --config=itf-qnx-x86_64 --test_env=IOSOCK_DLT_MODULE \\
        //feature_integration_tests/itf --test_output=streamed

Verified on the QNX 8.0 x86_64 QEMU image (KVM): passes; umount of a mods-* module answers "Not supported".

io-sock modules cannot be unloaded, so one target session can host only one variant of the module. This
test (synthetic input) is skipped whenever IOSOCK_DLT_RING_MODULE is set; see test_iosock_dlt_ring.py.
"""

import logging
import os
import time

import pytest

from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

MODULE_ENV = "IOSOCK_DLT_MODULE"
RING_MODULE_ENV = "IOSOCK_DLT_RING_MODULE"
EXPECTED_RECORDS = int(os.environ.get("IOSOCK_DLT_RECORDS", "10"))

REMOTE_MODULE = "/tmp_ram/mods-score-dlt.so"
UNLOAD_PATH = "/dev/io-sock/mods-score-dlt.so"
SYSCTL_ROOT = "qnx.module.score_dlt"

# DLT identifiers written by the module
APP_ID = "DLTM"
CTX_ID = "SYN"

_STATE_FINISHED = "2"
_STATE_FAILED = "3"
_FINISH_TIMEOUT_SEC = 20
_POLL_INTERVAL_SEC = 0.5
_LAST_DATAGRAM_GRACE_SEC = 1


@pytest.fixture
def module_path():
    if os.environ.get(RING_MODULE_ENV):
        pytest.skip(f"{RING_MODULE_ENV} is set: only one module variant can be loaded per target session")
    path = os.environ.get(MODULE_ENV)
    if not path or not os.path.isfile(path):
        pytest.skip(f"{MODULE_ENV} does not point to a built mods-score-dlt.so")
    return path


@pytest.fixture
def qnx_target(target):
    _, out = target.execute("uname -s")
    if b"QNX" not in out:
        pytest.skip("io-sock module tests run on QNX targets only")
    return target


def _sysctl(target, leaf):
    """Returns the value of qnx.module.score_dlt.<leaf> as a string, or None if unreadable."""
    exit_code, out = target.execute(f"sysctl -n {SYSCTL_ROOT}.{leaf}")
    return out.decode(errors="replace").strip() if exit_code == 0 else None


def _wait_for_worker(target):
    """Polls the worker state until it finished or failed; returns the last state seen."""
    deadline = time.monotonic() + _FINISH_TIMEOUT_SEC
    state = None
    while time.monotonic() < deadline:
        state = _sysctl(target, "state")
        if state in (_STATE_FINISHED, _STATE_FAILED):
            break
        time.sleep(_POLL_INTERVAL_SEC)
    return state


def test_iosock_dlt_module_sends_synthetic_stream(qnx_target, module_path, dlt_config):
    """Loads the module, expects the synthetic DLT records on the host and the counters to match."""
    with qnx_target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)

    with DltWindow(
        protocol=Protocol.UDP,
        host_ip=dlt_config.host_ip,
        multicast_ips=dlt_config.multicast_ips,
        print_to_stdout=True,
        binary_path=dlt_config.dlt_receive_path,
    ) as window:
        exit_code, out = qnx_target.execute(f"mount -T io-sock {REMOTE_MODULE}")
        assert exit_code == 0, f"mount -T io-sock failed: {out.decode(errors='replace')}"

        state = _wait_for_worker(qnx_target)
        time.sleep(_LAST_DATAGRAM_GRACE_SEC)

    assert _sysctl(qnx_target, "version") is not None, "module version is not visible through sysctl"
    assert state == _STATE_FINISHED, f"worker state is {state!r}, last_error={_sysctl(qnx_target, 'last_error')!r}"
    assert _sysctl(qnx_target, "records_received") == str(EXPECTED_RECORDS)
    assert _sysctl(qnx_target, "packets_sent") == str(EXPECTED_RECORDS)
    assert _sysctl(qnx_target, "send_failures") == "0"
    assert _sysctl(qnx_target, "packets_dropped") == "0"

    record = window.record(filters=[(APP_ID, CTX_ID)])
    messages = record.find(query=dict(apid=APP_ID, ctid=CTX_ID))
    logger.info("Received %d DLT messages with app_id=%s, context_id=%s", len(messages), APP_ID, CTX_ID)
    assert len(messages) >= EXPECTED_RECORDS, (
        f"Expected {EXPECTED_RECORDS} DLT messages with app_id={APP_ID} and context_id={CTX_ID}, got {len(messages)}"
    )

    # Modules cannot be unloaded ("Not supported"): report the result, do not fail the test on it.
    exit_code, out = qnx_target.execute(f"umount {UNLOAD_PATH}")
    logger.info("umount %s -> exit code %s %s", UNLOAD_PATH, exit_code, out.decode(errors="replace"))
