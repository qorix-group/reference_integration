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
"""ITF test for the ring input of the QNX io-sock DLT module (qnx_iosock_dlt).

The module (built with `make qnx-module-ring`) creates a shared-memory ring, /dev/shmem/score_dlt_ring.
Separate producer processes on the target (`make qnx-tools`, tools/ring_producer.c) write log records
into it; the module formats them as DLT inside io-sock and sends them natively. No socket and no IPC
is involved on the producer side. The test checks the DLT stream on the host and the module's sysctl
counters. Paths are passed in environment variables, otherwise the tests are skipped:

    IOSOCK_DLT_RING_MODULE=$PWD/qnx_iosock_dlt/qnx_module/mods-score-dlt-ring.so \\
    IOSOCK_DLT_RING_PRODUCER=$PWD/qnx_iosock_dlt/out/qnx_x86_64/ring_producer \\
    bazel test --config=itf-qnx-x86_64 \\
        --test_env=IOSOCK_DLT_RING_MODULE --test_env=IOSOCK_DLT_RING_PRODUCER \\
        //feature_integration_tests/itf --test_output=streamed

io-sock modules cannot be unloaded, so one target session can host only one variant of the module:
test_iosock_dlt.py (synthetic input) is skipped whenever IOSOCK_DLT_RING_MODULE is set.
"""

import logging
import os
import time

import pytest

from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

MODULE_ENV = "IOSOCK_DLT_RING_MODULE"
PRODUCER_ENV = "IOSOCK_DLT_RING_PRODUCER"

REMOTE_MODULE = "/tmp_ram/mods-score-dlt-ring.so"
REMOTE_PRODUCER = "/tmp_ram/ring_producer"
RING_NAME = "/score_dlt_ring"
SYSCTL_ROOT = "qnx.module.score_dlt"

APP_ID = "DLTM"
CTX_SINGLE = "RNG"
CTX_FIRST = "RN1"
CTX_SECOND = "RN2"

SINGLE_RECORDS = 20
CONCURRENT_RECORDS = 50  # per producer process
PRODUCER_PERIOD_MS = 20

_STATE_RUNNING = "1"
_STATE_FAILED = "3"
_INPUT_RING = "1"
_START_TIMEOUT_SEC = 15
_DRAIN_TIMEOUT_SEC = 15
_POLL_INTERVAL_SEC = 0.25
_LAST_DATAGRAM_GRACE_SEC = 1


@pytest.fixture
def ring_files():
    module = os.environ.get(MODULE_ENV)
    producer = os.environ.get(PRODUCER_ENV)
    if not module or not os.path.isfile(module):
        pytest.skip(f"{MODULE_ENV} does not point to a built mods-score-dlt-ring.so")
    if not producer or not os.path.isfile(producer):
        pytest.skip(f"{PRODUCER_ENV} does not point to a QNX ring_producer binary")
    return module, producer


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


def _wait_for(predicate, timeout):
    """Polls predicate() until it returns a truthy value or the timeout expires; returns the last value."""
    deadline = time.monotonic() + timeout
    value = predicate()
    while not value and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        value = predicate()
    return value


def _run_producer(target, count, ctx, period_ms=PRODUCER_PERIOD_MS):
    exit_code, out = target.execute(f"{REMOTE_PRODUCER} {RING_NAME} {count} {period_ms} ring-dlt {APP_ID} {ctx}")
    text = out.decode(errors="replace")
    logger.info("ring_producer (%s): exit code %s: %s", ctx, exit_code, text.strip())
    assert exit_code == 0, f"ring_producer failed: {text}"


def test_iosock_dlt_module_forwards_ring_records(qnx_target, ring_files, dlt_config):
    """Records written into the shared-memory ring by producer processes arrive as DLT on the host."""
    module_path, producer_path = ring_files
    with qnx_target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)
        sftp.upload(producer_path, REMOTE_PRODUCER)
    exit_code, out = qnx_target.execute(f"chmod +x {REMOTE_PRODUCER}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"

    with DltWindow(
        protocol=Protocol.UDP,
        host_ip=dlt_config.host_ip,
        multicast_ips=dlt_config.multicast_ips,
        print_to_stdout=True,
        binary_path=dlt_config.dlt_receive_path,
    ) as window:
        exit_code, out = qnx_target.execute(f"mount -T io-sock {REMOTE_MODULE}")
        assert exit_code == 0, f"mount -T io-sock failed: {out.decode(errors='replace')}"

        state = _wait_for(lambda: _sysctl(qnx_target, "state") in (_STATE_RUNNING, _STATE_FAILED)
                          and _sysctl(qnx_target, "state"), _START_TIMEOUT_SEC)
        assert state == _STATE_RUNNING, f"worker state is {state!r}, last_error={_sysctl(qnx_target, 'last_error')!r}"
        assert _sysctl(qnx_target, "input") == _INPUT_RING
        assert _sysctl(qnx_target, "ring_slots") not in (None, "0")

        # One producer process.
        _run_producer(qnx_target, SINGLE_RECORDS, CTX_SINGLE)
        # Two producer processes at the same time, writing into the same ring.
        exit_code, out = qnx_target.execute(
            f"{REMOTE_PRODUCER} {RING_NAME} {CONCURRENT_RECORDS} 5 ring-dlt {APP_ID} {CTX_FIRST} > /dev/null & "
            f"{REMOTE_PRODUCER} {RING_NAME} {CONCURRENT_RECORDS} 5 ring-dlt {APP_ID} {CTX_SECOND} > /dev/null & "
            "wait"
        )
        logger.info("concurrent producers: exit code %s %s", exit_code, out.decode(errors="replace").strip())

        expected = SINGLE_RECORDS + 2 * CONCURRENT_RECORDS
        _wait_for(lambda: _sysctl(qnx_target, "packets_sent") == str(expected), _DRAIN_TIMEOUT_SEC)
        time.sleep(_LAST_DATAGRAM_GRACE_SEC)

    assert _sysctl(qnx_target, "records_received") == str(expected)
    assert _sysctl(qnx_target, "packets_sent") == str(expected)
    assert _sysctl(qnx_target, "send_failures") == "0"
    assert _sysctl(qnx_target, "packets_dropped") == "0"
    assert _sysctl(qnx_target, "ring_drops_full") == "0"
    assert _sysctl(qnx_target, "ring_drops_oversize") == "0"
    assert _sysctl(qnx_target, "ring_corrupt") == "0"

    for ctx, wanted in ((CTX_SINGLE, SINGLE_RECORDS), (CTX_FIRST, CONCURRENT_RECORDS), (CTX_SECOND, CONCURRENT_RECORDS)):
        record = window.record(filters=[(APP_ID, ctx)])
        messages = record.find(query=dict(apid=APP_ID, ctid=ctx))
        logger.info("Received %d DLT messages with app_id=%s, context_id=%s", len(messages), APP_ID, ctx)
        assert len(messages) >= wanted, (
            f"Expected {wanted} DLT messages with app_id={APP_ID} and context_id={ctx}, got {len(messages)}"
        )
