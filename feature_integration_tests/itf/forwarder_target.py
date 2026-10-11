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
"""Helpers to run qnx_iosock_dlt/tools/ring_forwarder on the QNX ITF target (no tests in here).

The forwarder is the userspace twin of the io-sock module: it drains its own shared-memory ring and sends the DLT
frames with sendmsg(). Used by test_iosock_dlt_forwarder.py (functional test) and test_iosock_dlt_bench.py
(the "Forwarder" sink). `target` is the ITF target fixture.
"""

import time

import bench_metrics

REMOTE_FORWARDER = "/tmp_ram/ring_forwarder"
REMOTE_FORWARDER_LOG = "/tmp_ram/ring_forwarder.log"
# Not the module's /score_dlt_ring: the module cannot be unloaded and stays in the session.
FORWARDER_RING = "/score_dlt_ring_fwd"
# Default destination of the module (DLT_DST_* in qnx_module/common.mk).
FORWARDER_DESTINATION = ("239.255.42.99", 3490)

_PIDIN_CMD = '/proc/boot/pidin -F "%a %u %v %N"'
_STATS_MARK = "ring_forwarder: records="
_READY_MARK = "ring_forwarder: ready"
_START_TIMEOUT_SEC = 10
_REPLY_TIMEOUT_SEC = 5
_STOP_SEC = 1
_POLL_INTERVAL_SEC = 0.25


def forwarder_log(target):
    """Everything the forwarder printed so far (stdout and stderr go to REMOTE_FORWARDER_LOG)."""
    _, out = target.execute(f"cat {REMOTE_FORWARDER_LOG}")
    return out.decode(errors="replace")


def _forwarder_pids(target):
    _, out = target.execute(_PIDIN_CMD)
    return bench_metrics.parse_pidin_pids(out.decode(errors="replace"), "ring_forwarder")


def stop_forwarder(target):
    """Stops running forwarders with SIGTERM; each removes its ring and prints its final counters."""
    pids = _forwarder_pids(target)
    for pid in pids:
        target.execute(f"kill {pid}")
    if pids:
        time.sleep(_STOP_SEC)


def start_forwarder(target, local_path, slots, slot_payload=512, batch_bytes=0):
    """Uploads the forwarder, starts it on FORWARDER_RING and waits until it reports that the ring exists.

    A forwarder that an earlier test of the session left behind is stopped first. Ring size and batching should
    equal the module's build, so that both do the same work per record.
    """
    with target.sftp() as sftp:
        sftp.upload(local_path, REMOTE_FORWARDER)
    exit_code, out = target.execute(f"chmod +x {REMOTE_FORWARDER}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"
    stop_forwarder(target)

    host, port = FORWARDER_DESTINATION
    target.execute(
        f"nohup {REMOTE_FORWARDER} {FORWARDER_RING} {host} {port} {slots} {slot_payload} {batch_bytes} 1 "
        f"> {REMOTE_FORWARDER_LOG} 2>&1 &"
    )
    deadline = time.monotonic() + _START_TIMEOUT_SEC
    ready = False
    while not ready and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        ready = _READY_MARK in forwarder_log(target)
    assert ready, f"ring_forwarder did not start; log:\n{forwarder_log(target)}"


def forwarder_stats(target):
    """Asks the running forwarder for its counters with SIGUSR1 and returns them as a dict of ints."""
    pids = _forwarder_pids(target)
    assert pids, "ring_forwarder is not running on the target"
    seen = forwarder_log(target).count(_STATS_MARK)
    target.execute(f"kill -USR1 {pids[0]}")
    deadline = time.monotonic() + _REPLY_TIMEOUT_SEC
    while time.monotonic() < deadline:
        text = forwarder_log(target)
        if text.count(_STATS_MARK) > seen:
            return bench_metrics.parse_forwarder_stats(text)
        time.sleep(_POLL_INTERVAL_SEC)
    raise AssertionError(f"ring_forwarder did not report its counters; log:\n{forwarder_log(target)[-1500:]}")
