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
"""ITF benchmark: mw::log through DataRouter (kRemote) versus through the DLT ring and io-sock module (kCustom).

The same application (qnx_iosock_dlt/mwlog_ring/mwlog_bench_app, built by Bazel) runs on the QNX QEMU target
once per sink and scenario, repeated IOSOCK_DLT_BENCH_REPEATS times (default 3). Per run it reports the cost of
one log call in the application, and the host checks delivery (loss, reordering), throughput and the delay
variation of the DLT stream. Messages that did not arrive are attributed with the counters of each path: the
DataRouter's own drop statistics (published as DLT messages) and the application's mw::log statistics for the
DataRouter path, the module's sysctl counters for the ring path, and the host's UDP and tap0 drop counters
for both. Results go to the test log and, if Bazel provides TEST_UNDECLARED_OUTPUTS_DIR, to bench_results.json
and bench_results.md.

The module is loaded before the first run (it cannot be unloaded). The sinks are the DataRouter with a 1 MiB
application buffer, the DataRouter with an 8 MiB buffer and the ring. The sink that goes first rotates from
scenario to scenario and from repetition to repetition, after one discarded warm-up run per sink, so that no
sink is always first. All sinks therefore run with the module's worker polling in the background.

Optional fourth sink "Forwarder" (IOSOCK_DLT_RING_FORWARDER = path of the QNX tools/ring_forwarder binary): the same
ring, formatter and batching as the module, but sending with sendmsg() from a normal process, so that the cost of
the socket IPC hop can be read as module against forwarder. It runs on its own ring (/score_dlt_ring_fwd) for the
whole test, with the module's ring size (IOSOCK_DLT_BENCH_RING_SLOTS, default 2048) and the batching given in
IOSOCK_DLT_BENCH_FORWARDER_BATCH (0 or 1472, set it equal to the module build). IOSOCK_DLT_BENCH_SINKS (comma
separated, for example "Ring,Forwarder") restricts the sinks; without a DataRouter sink the DataRouter is not
started. IOSOCK_DLT_BENCH_CPU_HIGH_RATES=1 adds 10 kHz and 20 kHz to the CPU scenarios: the cost of the IPC hop
scales with the datagram rate, so it is best seen unbatched and fast.

A second test, test_bench_cpu_datarouter_versus_module, measures the CPU use of the application, the DataRouter
and io-sock under sustained load. Select a test with `-k bench_datarouter` (latency, delivery, loss) or
`-k bench_cpu`; both share the target session and the module. Setting IOSOCK_DLT_BENCH_DR_PERIOD_MS=10 restarts
the DataRouter with a 10 ms polling and flush period instead of 100 ms (needs patch 004 of patches/logging).

    IOSOCK_DLT_RING_MODULE=$PWD/qnx_iosock_dlt/qnx_module/mods-score-dlt-ring.so \\
    bazel test --config=itf-qnx-x86_64 --test_env=IOSOCK_DLT_RING_MODULE --test_env=IOSOCK_DLT_BENCH_REPEATS=5 \\
        --test_timeout=3600 \\
        --@score_logging//score/mw/log/flags:KCustom_Logging=True \\
        --@score_logging//score/mw/log/flags:custom_recorder_impl=//qnx_iosock_dlt/mwlog_ring:recorder \\
        --test_arg=-k --test_arg=bench_datarouter //feature_integration_tests/itf:itf --test_output=streamed

Run these tests on their own, not together with the other io-sock tests.
"""

import json
import logging
import os
import time

import pytest

import bench_metrics
import forwarder_target
from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

MODULE_ENV = "IOSOCK_DLT_RING_MODULE"
APP_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_bench_app"
RING_CONFIG_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_bench_ring.json"
REMOTE_CONFIG_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_bench_remote.json"
REMOTE_LARGE_CONFIG_RUNFILE = "qnx_iosock_dlt/mwlog_ring/mwlog_bench_remote_large.json"

REMOTE_MODULE = "/tmp_ram/mods-score-dlt-ring.so"
REMOTE_APP = "/tmp_ram/mwlog_bench_app"
REMOTE_RING_CONFIG = "/tmp_ram/mwlog_bench_ring.json"
REMOTE_REMOTE_CONFIG = "/tmp_ram/mwlog_bench_remote.json"
REMOTE_REMOTE_LARGE_CONFIG = "/tmp_ram/mwlog_bench_remote_large.json"
SYSCTL_ROOT = "qnx.module.score_dlt"

APP_ID = "BNCH"
CTX_REMOTE = "REM"
CTX_RING = "RNG"
CTX_FORWARDER = "FWD"

FORWARDER_ENV = "IOSOCK_DLT_RING_FORWARDER"
FORWARDER_BATCH_ENV = "IOSOCK_DLT_BENCH_FORWARDER_BATCH"
RING_SLOTS_ENV = "IOSOCK_DLT_BENCH_RING_SLOTS"
SINKS_ENV = "IOSOCK_DLT_BENCH_SINKS"
CPU_HIGH_RATES_ENV = "IOSOCK_DLT_BENCH_CPU_HIGH_RATES"
FORWARDER_PATH = os.environ.get(FORWARDER_ENV, "")

# Sink name -> (configuration file on the target, DLT context, environment prefix of the application command).
# The DataRouter variants differ in the size of the application's shared-memory buffer (1 MiB and 8 MiB); the
# DataRouter's own polling and flush period is set for the whole session with IOSOCK_DLT_BENCH_DR_PERIOD_MS (run
# the test twice to compare). The forwarder is fed through the ring recorder like the module, on its own ring.
SINK_SETUP = {
    "DataRouter": (REMOTE_REMOTE_CONFIG, CTX_REMOTE, ""),
    "DataRouter 8 MiB": (REMOTE_REMOTE_LARGE_CONFIG, CTX_REMOTE, ""),
    "Ring": (REMOTE_RING_CONFIG, CTX_RING, ""),
}
if FORWARDER_PATH:
    SINK_SETUP["Forwarder"] = (REMOTE_RING_CONFIG, CTX_FORWARDER, f"DLT_RING_NAME={forwarder_target.FORWARDER_RING} ")
# Sinks that report counters of their own (module sysctl, forwarder statistics) instead of DataRouter statistics.
COUNTED_SINKS = ("Ring", "Forwarder")


def _select(names):
    """Restricts `names` to the sinks listed in IOSOCK_DLT_BENCH_SINKS (all if unset), keeping their order."""
    wanted = {name.strip() for name in os.environ.get(SINKS_ENV, "").split(",") if name.strip()}
    if not wanted:
        return tuple(names)
    unknown = wanted - set(SINK_SETUP)
    if unknown:
        raise ValueError(f"{SINKS_ENV}: unknown or unavailable sinks {sorted(unknown)} (Forwarder needs {FORWARDER_ENV})")
    return tuple(name for name in names if name in wanted)


SINKS = _select(SINK_SETUP)
CPU_SINKS = _select(name for name in ("DataRouter", "Ring", "Forwarder") if name in SINK_SETUP)
FORWARDER_ACTIVE = bool(FORWARDER_PATH) and ("Forwarder" in SINKS or "Forwarder" in CPU_SINKS)
REPEATS_ENV = "IOSOCK_DLT_BENCH_REPEATS"
DR_PERIOD_ENV = "IOSOCK_DLT_BENCH_DR_PERIOD_MS"

# (name, count, messages per second (0 = unthrottled), text bytes per message)
WARMUP = ("warm-up", 500, 1000, 16)
SCENARIOS = [
    ("paced 1k/s, 16 B", 3000, 1000, 16),
    ("paced 1k/s, 400 B", 3000, 1000, 400),
    ("paced 5k/s, 16 B", 10000, 5000, 16),
    ("burst, 16 B", 5000, 0, 16),
    ("burst, 400 B", 5000, 0, 400),
]

_QNX_DATAROUTER_CHECK_CMD = "/proc/boot/pidin | /proc/boot/grep datarouter"
_QNX_DATAROUTER_START_CMD = (
    "cd /usr/bin/datarouter && {env}nohup on -A nonroot,allow,pathspace -u 1051:1091 "
    "./datarouter --no_adaptive_runtime > /dev/null 2>&1 &"
)
_DATAROUTER_STARTUP_TIMEOUT_SEC = 2
_CAPTURE_START_SEC = 1
_SETTLE_SEC = 3  # lets the sink drain and the last datagrams reach the host
# The mw::log DataRouter recorder publishes its counters at most every 5 s and the DataRouter its channel
# statistics every 10 s: bursts on the DataRouter path wait for both so that the drop counters are captured.
_REPORT_WAIT_SEC = 6
_STATISTICS_SETTLE_SEC = 11
_BETWEEN_RUNS_SEC = 2
_MODULE_LEAVES = (
    "packets_sent",
    "datagrams_sent",
    "packets_dropped",
    "send_failures",
    "ring_drops_full",
    "ring_drops_oversize",
)
_STATE_RUNNING = 1
_STATE_FAILED = 3
_START_TIMEOUT_SEC = 15
_POLL_INTERVAL_SEC = 0.25

CPU_REPEATS_ENV = "IOSOCK_DLT_BENCH_CPU_REPEATS"
CPU_SECONDS_ENV = "IOSOCK_DLT_BENCH_CPU_SECONDS"
# (name, messages per second, text bytes per message); every scenario runs for CPU_SECONDS
CPU_SCENARIOS = [
    ("cpu 1k/s, 16 B", 1000, 16),
    ("cpu 5k/s, 16 B", 5000, 16),
    ("cpu 5k/s, 400 B", 5000, 400),
]
if os.environ.get(CPU_HIGH_RATES_ENV) == "1":
    CPU_SCENARIOS += [("cpu 10k/s, 16 B", 10000, 16), ("cpu 20k/s, 16 B", 20000, 16)]
_PIDIN_CMD = '/proc/boot/pidin -F "%a %u %v %N"'
_CPU_DRAIN_SEC = 1  # lets the DataRouter and the module finish the last messages before the second snapshot


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
    """Returns the value of qnx.module.score_dlt.<leaf> as an int, or None if unreadable."""
    exit_code, out = target.execute(f"sysctl -n {SYSCTL_ROOT}.{leaf}")
    text = out.decode(errors="replace").strip()
    return int(text) if (exit_code == 0 and text.lstrip("-").isdigit()) else None


def _ensure_datarouter(target):
    """Starts the DataRouter if needed. With IOSOCK_DLT_BENCH_DR_PERIOD_MS set it is (re)started with that period.

    DATAROUTER_PERIOD_MS is read by the DataRouter built with patches/logging/004-datarouter-period-env.patch; it
    replaces the 100 ms polling and DLT flush periods.
    """
    period_ms = os.environ.get(DR_PERIOD_ENV, "").strip()
    _, out = target.execute(_QNX_DATAROUTER_CHECK_CMD)
    running = "datarouter" in out.decode(errors="replace")
    if running and not period_ms:
        return
    if running:
        _, out = target.execute(_PIDIN_CMD)
        for pid in bench_metrics.parse_pidin_pids(out.decode(errors="replace"), "datarouter"):
            target.execute(f"kill {pid}")
        time.sleep(_DATAROUTER_STARTUP_TIMEOUT_SEC)
    exit_code, _ = target.execute("test -x /usr/bin/datarouter")
    assert exit_code == 0, "Datarouter binary not found at /usr/bin/datarouter on target"
    env = f"DATAROUTER_PERIOD_MS={int(period_ms)} " if period_ms else ""
    target.execute(_QNX_DATAROUTER_START_CMD.format(env=env))
    time.sleep(_DATAROUTER_STARTUP_TIMEOUT_SEC)
    _, out = target.execute(_QNX_DATAROUTER_CHECK_CMD)
    assert "datarouter" in out.decode(errors="replace"), "Failed to start datarouter on target"


def _sink_counters(target, sink):
    """Counters of a ring sink under the module's sysctl names: the module's, or the forwarder's statistics."""
    if sink == "Ring":
        values = {leaf: _sysctl(target, leaf) for leaf in _MODULE_LEAVES}
        return {leaf: value for leaf, value in values.items() if value is not None}
    if sink == "Forwarder":
        return bench_metrics.forwarder_counters(forwarder_target.forwarder_stats(target))
    return {}


@pytest.fixture
def forwarder(qnx_target):
    """Runs tools/ring_forwarder on the target for the whole test if the Forwarder sink is active.

    It owns its own ring, so it can run next to the module. Like the module's worker it polls while idle, so it
    is also present (idle) during the runs of the other sinks. Yields whether it is running.
    """
    if not FORWARDER_ACTIVE:
        yield False
        return
    if not os.path.isfile(FORWARDER_PATH):
        pytest.skip(f"{FORWARDER_ENV} does not point to a QNX ring_forwarder binary")
    forwarder_target.start_forwarder(
        qnx_target,
        FORWARDER_PATH,
        slots=int(os.environ.get(RING_SLOTS_ENV, "2048")),
        batch_bytes=int(os.environ.get(FORWARDER_BATCH_ENV, "0")),
    )
    logger.info("ring_forwarder running: %s", forwarder_target.forwarder_log(qnx_target).strip())
    yield True
    forwarder_target.stop_forwarder(qnx_target)


def _datarouter_needed(sinks):
    return any(sink.startswith("DataRouter") for sink in sinks)


def _run(target, dlt_config, sink, scenario):
    """Runs the load generator once on one sink while capturing the DLT stream; returns the measurements."""
    _name, count, rate, size = scenario
    config_path, ctx, env_prefix = SINK_SETUP[sink]
    counted = sink in COUNTED_SINKS
    # Only a burst on the DataRouter path can overflow its buffer, so only there the counters are worth the wait.
    report_wait = _REPORT_WAIT_SEC if (not counted and rate == 0) else 0
    settle = _STATISTICS_SETTLE_SEC if report_wait else _SETTLE_SEC
    counters_before = _sink_counters(target, sink)
    net_before = bench_metrics.read_host_net_counters()

    with DltWindow(
        protocol=Protocol.UDP,
        host_ip=dlt_config.host_ip,
        multicast_ips=dlt_config.multicast_ips,
        print_to_stdout=True,
        binary_path=dlt_config.dlt_receive_path,
    ) as window:
        time.sleep(_CAPTURE_START_SEC)
        exit_code, out = target.execute(
            f"{env_prefix}MW_LOG_CONFIG_FILE={config_path} {REMOTE_APP} {ctx} {count} {rate} {size} {report_wait}"
        )
        text = out.decode(errors="replace")
        assert exit_code == 0, f"mwlog_bench_app failed: {text}"
        time.sleep(settle)
        captured = list(window.get_captured_logs())

    net = bench_metrics.counter_delta(net_before, bench_metrics.read_host_net_counters())
    module = bench_metrics.counter_delta(counters_before, _sink_counters(target, sink)) if counted else {}
    result = bench_metrics.parse_result(text)
    assert result is not None, f"no BENCH_RESULT line in the application output: {text}"
    summary = bench_metrics.summarize(bench_metrics.parse_dlt_records(captured, APP_ID, ctx), count)
    drops = {} if counted else bench_metrics.parse_sink_counters(captured, APP_ID)
    loss = bench_metrics.explain_loss(sink, summary, drops, module)
    return {"result": result, "summary": summary, "drops": drops, "module": module, "net": net, "loss": loss}


def _mount_module(target):
    if _sysctl(target, "state") is not None:
        return  # loaded by an earlier test of this session, a module cannot be loaded twice
    exit_code, out = target.execute(f"mount -T io-sock {REMOTE_MODULE}")
    assert exit_code == 0, f"mount -T io-sock failed: {out.decode(errors='replace')}"
    deadline = time.monotonic() + _START_TIMEOUT_SEC
    state = _sysctl(target, "state")
    while state not in (_STATE_RUNNING, _STATE_FAILED) and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        state = _sysctl(target, "state")
    assert state == _STATE_RUNNING, f"worker state is {state!r}"


def test_bench_datarouter_versus_module(qnx_target, module_path, dlt_config, forwarder):
    """Measures the DataRouter path and the ring path with the same application and scenarios."""
    repeats = max(int(os.environ.get(REPEATS_ENV, "3")), 1)
    app_path = _runfile(APP_RUNFILE)
    ring_config = _runfile(RING_CONFIG_RUNFILE)
    remote_config = _runfile(REMOTE_CONFIG_RUNFILE)
    remote_large_config = _runfile(REMOTE_LARGE_CONFIG_RUNFILE)
    with qnx_target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)
        sftp.upload(app_path, REMOTE_APP)
        sftp.upload(ring_config, REMOTE_RING_CONFIG)
        sftp.upload(remote_config, REMOTE_REMOTE_CONFIG)
        sftp.upload(remote_large_config, REMOTE_REMOTE_LARGE_CONFIG)
    exit_code, out = qnx_target.execute(f"chmod +x {REMOTE_APP}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"

    if _datarouter_needed(SINKS):
        _ensure_datarouter(qnx_target)
    _mount_module(qnx_target)

    # Warm-up (discarded): first use of each path pays for page faults, connection set-up and ARP.
    for sink in SINKS:
        _run(qnx_target, dlt_config, sink, WARMUP)
        time.sleep(_BETWEEN_RUNS_SEC)

    runs = {}
    for repetition in range(repeats):
        for index, scenario in enumerate(SCENARIOS):
            # The sink that goes first rotates from scenario to scenario and from repetition to repetition.
            shift = (repetition + index) % len(SINKS)
            for sink in SINKS[shift:] + SINKS[:shift]:
                run = _run(qnx_target, dlt_config, sink, scenario)
                logger.info("[repeat %d] %s, %s: %s", repetition + 1, sink, scenario[0], run)
                runs.setdefault((scenario[0], sink), []).append(run)
                time.sleep(_BETWEEN_RUNS_SEC)

    groups = [
        (scenario[0], sink, bench_metrics.aggregate(runs[(scenario[0], sink)]))
        for scenario in SCENARIOS
        for sink in SINKS
    ]
    table = bench_metrics.format_aggregate_table(groups)
    drop_table = bench_metrics.format_drop_table(groups)
    report = f"Repetitions: {repeats}, values are median (min..max)\n\n{table}\nWhere messages were lost:\n\n{drop_table}"
    logger.info("Benchmark results:\n%s", report)
    print(report)

    outputs_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if outputs_dir:
        with open(os.path.join(outputs_dir, "bench_results.json"), "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "repetitions": repeats,
                    "runs": [
                        {"scenario": scenario, "sink": sink, "repetition": number + 1, **run}
                        for (scenario, sink), sink_runs in runs.items()
                        for number, run in enumerate(sink_runs)
                    ],
                    "aggregate": [
                        {"scenario": scenario, "sink": sink, **aggregated} for scenario, sink, aggregated in groups
                    ],
                },
                handle,
                indent=2,
            )
        with open(os.path.join(outputs_dir, "bench_results.md"), "w", encoding="utf-8") as handle:
            handle.write(report)

    for scenario, sink, aggregated in groups:
        assert aggregated["delivered"]["max"] > 0, f"{sink}: no message of scenario '{scenario}' reached the host"


def _cpu_process_names():
    """Processes whose CPU time is read: the DataRouter if a DataRouter sink runs, io-sock, the forwarder if active."""
    names = ["datarouter"] if _datarouter_needed(CPU_SINKS) else []
    names.append("io-sock")
    if FORWARDER_ACTIVE:
        names.append("ring_forwarder")
    return tuple(names)


def _pidin_cpu(target):
    """CPU time (ns) of the DataRouter, of io-sock (the module's worker, and for the DataRouter path the BSD socket
    send) and of the forwarder, from pidin."""
    _, out = target.execute(_PIDIN_CMD)
    text = out.decode(errors="replace")
    cpu = bench_metrics.parse_pidin_cpu(text, _cpu_process_names())
    missing = [name for name, value in cpu.items() if value is None]
    assert not missing, f"pidin output has no line for {missing}; output starts with:\n{text[:1500]}"
    return cpu


def _run_cpu(target, sink, scenario, seconds, idle_ns_per_s):
    """Runs one sustained load on one sink and returns the CPU summary of the application and the sink's processes.

    For the ring sinks the datagrams sent during the run are added (module sysctl, forwarder statistics), so the
    transport cost can be given per datagram. The counters are read outside of the measured window.
    """
    _name, rate, size = scenario
    count = rate * seconds
    config_path, ctx, env_prefix = SINK_SETUP[sink]
    counters_before = _sink_counters(target, sink)
    before = _pidin_cpu(target)
    started = time.monotonic()
    # No report wait and no spinning: the application's own CPU time must not be dominated by pacing.
    exit_code, out = target.execute(
        f"{env_prefix}MW_LOG_CONFIG_FILE={config_path} {REMOTE_APP} {ctx} {count} {rate} {size} 0 0"
    )
    text = out.decode(errors="replace")
    assert exit_code == 0, f"mwlog_bench_app failed: {text}"
    time.sleep(_CPU_DRAIN_SEC)
    window = time.monotonic() - started
    after = _pidin_cpu(target)
    datagrams = None
    if sink in COUNTED_SINKS:
        counters = bench_metrics.counter_delta(counters_before, _sink_counters(target, sink))
        datagrams = counters.get("datagrams_sent")
    result = bench_metrics.parse_result(text)
    assert result is not None, f"no BENCH_RESULT line in the application output: {text}"
    return bench_metrics.cpu_summary(
        before, after, result.get("cpu_us", 0), window, count, idle_ns_per_s, datagrams=datagrams
    )


def test_bench_cpu_datarouter_versus_module(qnx_target, module_path, forwarder):
    """Measures the CPU that the DataRouter path, the ring path and (if active) the forwarder use for sustained loads.

    The CPU time of the application comes from the application itself, that of the DataRouter, of io-sock
    (which contains the module's worker and, for the DataRouter path, the BSD socket send) and of the forwarder
    from pidin. The kernel samples these times at the timer tick, so each load runs for
    IOSOCK_DLT_BENCH_CPU_SECONDS (default 10) and the idle polling cost, measured without load, is removed from
    the per-message figures.
    """
    repeats = max(int(os.environ.get(CPU_REPEATS_ENV, "3")), 1)
    seconds = max(int(os.environ.get(CPU_SECONDS_ENV, "10")), 1)
    app_path = _runfile(APP_RUNFILE)
    ring_config = _runfile(RING_CONFIG_RUNFILE)
    remote_config = _runfile(REMOTE_CONFIG_RUNFILE)
    with qnx_target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)
        sftp.upload(app_path, REMOTE_APP)
        sftp.upload(ring_config, REMOTE_RING_CONFIG)
        sftp.upload(remote_config, REMOTE_REMOTE_CONFIG)
    exit_code, out = qnx_target.execute(f"chmod +x {REMOTE_APP}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"

    if _datarouter_needed(CPU_SINKS):
        _ensure_datarouter(qnx_target)
    _mount_module(qnx_target)
    for sink in CPU_SINKS:  # warm-up, discarded
        _run_cpu(qnx_target, sink, CPU_SCENARIOS[0], 2, {})
        time.sleep(_BETWEEN_RUNS_SEC)

    runs = {}
    idle_runs = []
    for repetition in range(repeats):
        # Idle cost of the DataRouter and of io-sock with the module loaded and no application running.
        before = _pidin_cpu(qnx_target)
        time.sleep(seconds)
        after = _pidin_cpu(qnx_target)
        idle = {name: max(after[name] - before[name], 0) / seconds for name in after}
        idle_runs.append(idle)
        logger.info("[repeat %d] idle CPU (ns per second): %s", repetition + 1, idle)

        for index, scenario in enumerate(CPU_SCENARIOS):
            order = CPU_SINKS if ((repetition + index) % 2 == 0) else tuple(reversed(CPU_SINKS))
            for sink in order:
                run = _run_cpu(qnx_target, sink, scenario, seconds, idle)
                logger.info("[repeat %d] %s, %s: %s", repetition + 1, sink, scenario[0], run)
                runs.setdefault((scenario[0], sink), []).append(run)
                time.sleep(_BETWEEN_RUNS_SEC)

    groups = [
        (scenario[0], sink, bench_metrics.aggregate_cpu(runs[(scenario[0], sink)]))
        for scenario in CPU_SCENARIOS
        for sink in CPU_SINKS
    ]
    idle_line = bench_metrics.format_idle(idle_runs)
    report = (
        f"Repetitions: {repeats}, {seconds} s per load, values are median (min..max)\n\n"
        f"{bench_metrics.format_cpu_table(groups)}\n{idle_line}\n"
    )
    logger.info("CPU results:\n%s", report)
    print(report)

    outputs_dir = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
    if outputs_dir:
        with open(os.path.join(outputs_dir, "bench_cpu_results.json"), "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "repetitions": repeats,
                    "seconds": seconds,
                    "idle_ns_per_s": idle_runs,
                    "runs": [
                        {"scenario": scenario, "sink": sink, "repetition": number + 1, **run}
                        for (scenario, sink), sink_runs in runs.items()
                        for number, run in enumerate(sink_runs)
                    ],
                },
                handle,
                indent=2,
            )
        with open(os.path.join(outputs_dir, "bench_cpu_results.md"), "w", encoding="utf-8") as handle:
            handle.write(report)
