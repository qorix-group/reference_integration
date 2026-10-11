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
"""ITF test: an unmodified mw::log application feeds the io-sock DLT module through the ring recorder.

The application (qnx_iosock_dlt/mwlog_ring/mwlog_ring_app) is built by Bazel with the custom recorder
flags and logs with log mode "kCustom". Its records go into the shared-memory ring that the ring variant
of the module (built with `make qnx-module-ring`) owns, and the module sends them as DLT. The app, its
configuration and the image are Bazel data of the ITF target; only the module path comes from the
environment, otherwise the test is skipped:

    IOSOCK_DLT_RING_MODULE=$PWD/qnx_iosock_dlt/qnx_module/mods-score-dlt-ring.so \\
    bazel test --config=itf-qnx-x86_64 --test_env=IOSOCK_DLT_RING_MODULE \\
        --@score_logging//score/mw/log/flags:KCustom_Logging=True \\
        --@score_logging//score/mw/log/flags:custom_recorder_impl=//qnx_iosock_dlt/mwlog_ring:recorder \\
        --test_arg=-k --test_arg=mwlog //feature_integration_tests/itf:itf --test_output=streamed

A module can only be loaded once per target session: run this test on its own (`-k mwlog`), not together
with test_iosock_dlt_ring.py.
"""

import logging
import os
import time

import pytest

from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

MODULE_ENV = "IOSOCK_DLT_RING_MODULE"
APP_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_ring_app"
CONFIG_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_ring_config.json"

REMOTE_MODULE = "/tmp_ram/mods-score-dlt-ring.so"
REMOTE_APP = "/tmp_ram/mwlog_ring_app"
REMOTE_CONFIG = "/tmp_ram/mwlog_ring_config.json"
SYSCTL_ROOT = "qnx.module.score_dlt"

APP_ID = "MWLG"
CTX_INFO = "MWC1"
CTX_MIXED = "MWC2"
CTX_FILTERED = "MWD"

RECORDS = 20  # iterations; each sends one record to MWC1 and one to MWC2
PERIOD_MS = 20

_STATE_RUNNING = "1"
_STATE_FAILED = "3"
_START_TIMEOUT_SEC = 15
_DRAIN_TIMEOUT_SEC = 15
_POLL_INTERVAL_SEC = 0.25
_LAST_DATAGRAM_GRACE_SEC = 1


def _runfile(relative_path):
    root = os.environ.get("RUNFILES_DIR")
    workspace = os.environ.get("TEST_WORKSPACE", "_main")
    path = os.path.join(root, workspace, relative_path) if root else None
    if not path or not os.path.isfile(path):
        pytest.skip(f"{relative_path} is not part of this test's runfiles (QNX ITF target only)")
    return path


@pytest.fixture
def module_path():
    path = os.environ.get(MODULE_ENV)
    if not path or not os.path.isfile(path):
        pytest.skip(f"{MODULE_ENV} does not point to a built mods-score-dlt-ring.so")
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


def _wait_for(predicate, timeout):
    """Polls predicate() until it returns a truthy value or the timeout expires; returns the last value."""
    deadline = time.monotonic() + timeout
    value = predicate()
    while not value and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        value = predicate()
    return value


def test_mwlog_application_records_reach_dlt(qnx_target, module_path, dlt_config):
    """Records of an mw::log application in kCustom mode arrive as DLT on the host, filtered by its level."""
    app_path = _runfile(APP_RUNFILE)
    config_path = _runfile(CONFIG_RUNFILE)
    with qnx_target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)
        sftp.upload(app_path, REMOTE_APP)
        sftp.upload(config_path, REMOTE_CONFIG)
    exit_code, out = qnx_target.execute(f"chmod +x {REMOTE_APP}")
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

        state = _wait_for(
            lambda: _sysctl(qnx_target, "state") in (_STATE_RUNNING, _STATE_FAILED) and _sysctl(qnx_target, "state"),
            _START_TIMEOUT_SEC,
        )
        assert state == _STATE_RUNNING, f"worker state is {state!r}, last_error={_sysctl(qnx_target, 'last_error')!r}"

        exit_code, out = qnx_target.execute(
            f"MW_LOG_CONFIG_FILE={REMOTE_CONFIG} {REMOTE_APP} {RECORDS} {PERIOD_MS}"
        )
        text = out.decode(errors="replace")
        logger.info("mwlog_ring_app: exit code %s: %s", exit_code, text.strip())
        assert exit_code == 0, f"mwlog_ring_app failed: {text}"

        expected = 2 * RECORDS
        _wait_for(lambda: _sysctl(qnx_target, "packets_sent") == str(expected), _DRAIN_TIMEOUT_SEC)
        time.sleep(_LAST_DATAGRAM_GRACE_SEC)

    assert _sysctl(qnx_target, "records_received") == str(expected)
    assert _sysctl(qnx_target, "packets_sent") == str(expected)
    assert _sysctl(qnx_target, "send_failures") == "0"
    assert _sysctl(qnx_target, "packets_dropped") == "0"
    assert _sysctl(qnx_target, "ring_drops_full") == "0"
    assert _sysctl(qnx_target, "ring_drops_oversize") == "0"
    assert _sysctl(qnx_target, "ring_corrupt") == "0"

    for ctx, wanted in ((CTX_INFO, RECORDS), (CTX_MIXED, RECORDS)):
        record = window.record(filters=[(APP_ID, ctx)])
        messages = record.find(query=dict(apid=APP_ID, ctid=ctx))
        logger.info("Received %d DLT messages with app_id=%s, context_id=%s", len(messages), APP_ID, ctx)
        assert len(messages) >= wanted, (
            f"Expected {wanted} DLT messages with app_id={APP_ID} and context_id={ctx}, got {len(messages)}"
        )

    # The debug records are below the configured level (kInfo) and must never leave the application.
    record = window.record(filters=[(APP_ID, CTX_FILTERED)])
    assert record.find(query=dict(apid=APP_ID, ctid=CTX_FILTERED)) == []
