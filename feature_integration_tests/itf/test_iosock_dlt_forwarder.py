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
"""ITF test for the userspace forwarder (qnx_iosock_dlt/tools/ring_forwarder.cpp) on the QNX QEMU target.

The forwarder is the sendmsg() twin of the io-sock module: it owns its own shared-memory ring, drains it with the
same core (formatter, batching) and sends the DLT frames through a BSD socket. This test starts it, lets a producer
process write 20 records into its ring and checks the DLT stream on the host and the forwarder's counters. It needs
no module, so it can share a target session with the module tests. Both binaries are built with `make qnx-tools`;
their paths are passed in environment variables, otherwise the test is skipped:

    IOSOCK_DLT_RING_FORWARDER=$PWD/qnx_iosock_dlt/out/qnx_x86_64/ring_forwarder \\
    IOSOCK_DLT_RING_PRODUCER=$PWD/qnx_iosock_dlt/out/qnx_x86_64/ring_producer \\
    bazel test --config=itf-qnx-x86_64 \\
        --test_env=IOSOCK_DLT_RING_FORWARDER --test_env=IOSOCK_DLT_RING_PRODUCER \\
        --test_arg=-k --test_arg=forwarder //feature_integration_tests/itf --test_output=streamed

IOSOCK_DLT_BENCH_FORWARDER_BATCH=1472 runs the forwarder with batching (several DLT messages per datagram).
"""

import logging
import os
import time

import pytest

import forwarder_target
from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

FORWARDER_ENV = "IOSOCK_DLT_RING_FORWARDER"
PRODUCER_ENV = "IOSOCK_DLT_RING_PRODUCER"
BATCH_ENV = "IOSOCK_DLT_BENCH_FORWARDER_BATCH"

REMOTE_PRODUCER = "/tmp_ram/ring_producer"
APP_ID = "DLTM"
CTX_ID = "FWD"
RECORDS = 20
PRODUCER_PERIOD_MS = 20
RING_SLOTS = 256

_DRAIN_TIMEOUT_SEC = 15
_POLL_INTERVAL_SEC = 0.25
_LAST_DATAGRAM_GRACE_SEC = 1


@pytest.fixture
def forwarder_files():
    forwarder = os.environ.get(FORWARDER_ENV)
    producer = os.environ.get(PRODUCER_ENV)
    if not forwarder or not os.path.isfile(forwarder):
        pytest.skip(f"{FORWARDER_ENV} does not point to a QNX ring_forwarder binary")
    if not producer or not os.path.isfile(producer):
        pytest.skip(f"{PRODUCER_ENV} does not point to a QNX ring_producer binary")
    return forwarder, producer


@pytest.fixture
def qnx_target(target):
    _, out = target.execute("uname -s")
    if b"QNX" not in out:
        pytest.skip("forwarder tests run on QNX targets only")
    return target


def _wait_for_records(target, expected):
    """Asks the forwarder for its counters until it has taken `expected` records; returns the last counters."""
    deadline = time.monotonic() + _DRAIN_TIMEOUT_SEC
    stats = forwarder_target.forwarder_stats(target)
    while stats["records"] < expected and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        stats = forwarder_target.forwarder_stats(target)
    return stats


def test_iosock_dlt_forwarder_sends_records(qnx_target, forwarder_files, dlt_config):
    """Records written into the forwarder's ring arrive as DLT on the host; the counters add up."""
    forwarder_path, producer_path = forwarder_files
    batch_bytes = int(os.environ.get(BATCH_ENV, "0"))
    with qnx_target.sftp() as sftp:
        sftp.upload(producer_path, REMOTE_PRODUCER)
    exit_code, out = qnx_target.execute(f"chmod +x {REMOTE_PRODUCER}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"

    forwarder_target.start_forwarder(qnx_target, forwarder_path, slots=RING_SLOTS, batch_bytes=batch_bytes)
    try:
        with DltWindow(
            protocol=Protocol.UDP,
            host_ip=dlt_config.host_ip,
            multicast_ips=dlt_config.multicast_ips,
            print_to_stdout=True,
            binary_path=dlt_config.dlt_receive_path,
        ) as window:
            exit_code, out = qnx_target.execute(
                f"{REMOTE_PRODUCER} {forwarder_target.FORWARDER_RING} {RECORDS} {PRODUCER_PERIOD_MS} "
                f"ring-dlt {APP_ID} {CTX_ID}"
            )
            text = out.decode(errors="replace")
            logger.info("ring_producer: exit code %s: %s", exit_code, text.strip())
            assert exit_code == 0, f"ring_producer failed: {text}"

            stats = _wait_for_records(qnx_target, RECORDS)
            time.sleep(_LAST_DATAGRAM_GRACE_SEC)
    finally:
        forwarder_target.stop_forwarder(qnx_target)

    logger.info("ring_forwarder counters: %s", stats)
    assert stats["records"] == RECORDS
    assert stats["sent"] == RECORDS
    assert stats["failures"] == 0
    assert stats["dropped"] == 0
    assert stats["sendmsg_errors"] == 0
    assert stats["ring_full"] == 0
    assert stats["ring_oversize"] == 0
    assert stats["corrupt"] == 0
    # One sendmsg() per datagram; without batching also one datagram per message.
    assert stats["sendmsg_calls"] == stats["datagrams"]
    if batch_bytes == 0:
        assert stats["datagrams"] == RECORDS
    else:
        assert 0 < stats["datagrams"] <= RECORDS

    record = window.record(filters=[(APP_ID, CTX_ID)])
    messages = record.find(query=dict(apid=APP_ID, ctid=CTX_ID))
    logger.info("Received %d DLT messages with app_id=%s, context_id=%s", len(messages), APP_ID, CTX_ID)
    assert len(messages) >= RECORDS, (
        f"Expected {RECORDS} DLT messages with app_id={APP_ID} and context_id={CTX_ID}, got {len(messages)}"
    )
