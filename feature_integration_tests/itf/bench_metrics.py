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
"""Parsing and statistics for the DLT sink benchmark (test_iosock_dlt_bench.py).

Pure Python, no ITF dependency, so it can be unit tested on any host.

Delay variation: guest and host clocks are not synchronised, so absolute latency cannot be measured.
Message i carries the guest timestamp tmsp_i (0.1 ms ticks) and is received at host time rx_i. The value
  dv_i = (rx_i - rx_0) - (tmsp_i - tmsp_0)
is constant for a sink that delivers with a fixed delay and grows when messages queue up. It is reported
relative to its minimum, i.e. as the queuing delay above the best case of the run.
"""

import re
import statistics
from datetime import datetime

# One line printed by `dlt-receive -a`, for example:
#   2026/10/08 08:07:02.745398      79620 114 IOS1 DLTM RN1- log info V 2 [bench 47 xxxx]
_DLT_LINE = re.compile(
    r"(\d{4})/(\d\d)/(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{6})\s+(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+log\s+\S+\s+V\s+\d+\s+\[bench (\d+)"
)
_RESULT_LINE = re.compile(r"BENCH_RESULT (.*)")
# Statistics line of tools/ring_forwarder.cpp (printed on SIGUSR1 and at exit).
_FORWARDER_LINE = re.compile(r"ring_forwarder: (records=\d+\b.*)")
_KEY_VALUE = re.compile(r"(\w+)=(\d+)")
# Forwarder counter -> the sysctl name of the module's equivalent, so that both ring sinks share explain_loss().
_FORWARDER_TO_MODULE = {
    "sent": "packets_sent",
    "datagrams": "datagrams_sent",
    "dropped": "packets_dropped",
    "failures": "send_failures",
    "ring_full": "ring_drops_full",
    "ring_oversize": "ring_drops_oversize",
    "sendmsg_calls": "sendmsg_calls",
}

# Statistics that the DataRouter (application "DR", context "STAT") and the mw::log DataRouter recorder (the
# application's own context "STAT") publish as DLT messages. Arguments are printed separated by spaces.
_DR_BUFFER_FULL = re.compile(r"message drop detected:\W*(\d+)\W*messages,\W*(\d+)\W*bytes lost due to buffer full")
_DR_INVALID_SIZE = re.compile(r"message drop detected:\W*(\d+)\W*messages lost due to invalid size")
_DR_ACCUMULATED = re.compile(r"messages dropped:\W*(\d+)\W*\(accumulated\)")
_DR_SEND_FAILED = re.compile(r"failed to send: total count\W*(\d+)")
_APP_STATS = re.compile(r"no_slot_available_counter=\W*(\d+)\D+message_too_long_counter=\W*(\d+)")

_TMSP_TICKS_PER_SECOND = 10000.0
_UINT32 = 1 << 32


def parse_result(text):
    """Returns the key/value pairs of the BENCH_RESULT line of the application as a dict (numbers as int)."""
    match = _RESULT_LINE.search(text)
    if match is None:
        return None
    result = {}
    for pair in match.group(1).split():
        key, _, value = pair.partition("=")
        result[key] = int(value) if value.lstrip("-").isdigit() else value
    return result


def parse_forwarder_stats(text):
    """Counters of the last statistics line of ring_forwarder in `text` as ints, or None if there is none."""
    stats = None
    for line in text.splitlines():
        match = _FORWARDER_LINE.search(line)
        if match is not None:
            stats = {key: int(value) for key, value in _KEY_VALUE.findall(match.group(1))}
    return stats


def forwarder_counters(stats):
    """The forwarder's counters under the names of the module's sysctl leaves (plus `sendmsg_calls`)."""
    return {module_key: stats[key] for key, module_key in _FORWARDER_TO_MODULE.items() if key in stats}


def parse_dlt_records(lines, app_id, ctx_id):
    """Extracts the benchmark messages of one application and context from captured dlt-receive lines.

    Returns a list of dicts with the host receive time `rx` (seconds), the DLT timestamp `tmsp` (ticks) and
    the sequence number `seq`, in arrival order.
    """
    records = []
    for line in lines:
        match = _DLT_LINE.search(line)
        if match is None:
            continue
        year, month, day, hour, minute, second, micro, tmsp, _mcnt, _ecu, apid, ctid, seq = match.groups()
        if (apid.rstrip("-") != app_id) or (ctid.rstrip("-") != ctx_id):
            continue
        rx = datetime(int(year), int(month), int(day), int(hour), int(minute), int(second)).timestamp()
        records.append({"rx": rx + int(micro) / 1e6, "tmsp": int(tmsp), "seq": int(seq)})
    return records


def _percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    return sorted_values[int(fraction * (len(sorted_values) - 1))]


def _tmsp_delta(current, reference):
    delta = (current - reference) % _UINT32
    return delta - _UINT32 if delta >= (_UINT32 // 2) else delta


def summarize(records, sent):
    """Delivery, ordering, throughput and delay variation of one run; `sent` is the number of messages sent."""
    sequences = [record["seq"] for record in records]
    unique = set(sequences)
    delivered = len(unique)
    summary = {
        "sent": sent,
        "received": len(sequences),
        "delivered": delivered,
        "lost": max(sent - delivered, 0),
        "loss_pct": 100.0 * max(sent - delivered, 0) / sent if sent else 0.0,
        "duplicates": len(sequences) - delivered,
        "reordered": sum(1 for earlier, later in zip(sequences, sequences[1:]) if later < earlier),
        "span_s": 0.0,
        "msgs_per_s": 0.0,
        "dv_p50_ms": 0.0,
        "dv_p99_ms": 0.0,
        "dv_max_ms": 0.0,
    }
    if len(records) < 2:
        return summary

    span = records[-1]["rx"] - records[0]["rx"]
    summary["span_s"] = span
    summary["msgs_per_s"] = (delivered / span) if span > 0 else 0.0

    first = records[0]
    variation = [
        (record["rx"] - first["rx"]) - (_tmsp_delta(record["tmsp"], first["tmsp"]) / _TMSP_TICKS_PER_SECOND)
        for record in records
    ]
    smallest = min(variation)
    queuing_ms = sorted((value - smallest) * 1000.0 for value in variation)
    summary["dv_p50_ms"] = _percentile(queuing_ms, 0.50)
    summary["dv_p99_ms"] = _percentile(queuing_ms, 0.99)
    summary["dv_max_ms"] = queuing_ms[-1]
    return summary


def parse_sink_counters(lines, app_id):
    """Drop counters that the DataRouter path reports about itself, from captured dlt-receive lines of one run.

    - dr_buffer_full_msgs / _bytes, dr_invalid_size_msgs: "message drop detected" events of the DataRouter for
      the application's shared-memory buffer, summed over the run.
    - dr_dropped_accumulated: highest "messages dropped (accumulated)" value of the periodic statistics.
    - dr_send_failures: "failed to send" counts of the DataRouter's UDP channels (periodic, every 10 s).
    - app_no_slot / app_too_long: counters of the application's mw::log DataRouter recorder (published every 5 s
      with the next log call, see mwlog_bench_app's final message).
    All are 0 when the corresponding line was not captured; the application statistics only exist on the
    DataRouter path.
    """
    counters = {
        "dr_buffer_full_msgs": 0,
        "dr_buffer_full_bytes": 0,
        "dr_invalid_size_msgs": 0,
        "dr_dropped_accumulated": 0,
        "dr_send_failures": 0,
        "app_no_slot": 0,
        "app_too_long": 0,
    }
    for line in lines:
        if "STAT" not in line:
            continue
        if f" {app_id} STAT" in line:
            match = _APP_STATS.search(line)
            if match is not None:
                counters["app_no_slot"] = max(counters["app_no_slot"], int(match.group(1)))
                counters["app_too_long"] = max(counters["app_too_long"], int(match.group(2)))
            continue
        match = _DR_SEND_FAILED.search(line)
        if match is not None:
            counters["dr_send_failures"] += int(match.group(1))
        if app_id not in line:
            continue
        match = _DR_BUFFER_FULL.search(line)
        if match is not None:
            counters["dr_buffer_full_msgs"] += int(match.group(1))
            counters["dr_buffer_full_bytes"] += int(match.group(2))
        match = _DR_INVALID_SIZE.search(line)
        if match is not None:
            counters["dr_invalid_size_msgs"] += int(match.group(1))
        match = _DR_ACCUMULATED.search(line)
        if match is not None:
            counters["dr_dropped_accumulated"] = max(counters["dr_dropped_accumulated"], int(match.group(1)))
    return counters


def read_host_net_counters(interface="tap0"):
    """Datagram drop counters of the host that receives the DLT stream; keys that cannot be read are omitted."""
    counters = {}
    try:
        with open("/proc/net/snmp", encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        for header, values in zip(lines, lines[1:]):
            if header.startswith("Udp:") and values.startswith("Udp:") and "InDatagrams" in header:
                for name, value in zip(header.split()[1:], values.split()[1:]):
                    if name in ("InErrors", "RcvbufErrors"):
                        counters[f"host_udp_{name}"] = int(value)
    except (OSError, ValueError):
        pass
    for name in ("rx_dropped", "tx_dropped", "rx_errors", "rx_fifo_errors"):
        try:
            with open(f"/sys/class/net/{interface}/statistics/{name}", encoding="utf-8") as handle:
                counters[f"{interface}_{name}"] = int(handle.read().strip())
        except (OSError, ValueError):
            pass
    return counters


def counter_delta(before, after):
    """Difference of two counter snapshots for the keys present in both."""
    return {key: after[key] - before[key] for key in before if key in after}


def explain_loss(sink, summary, drops, module):
    """Splits the messages that did not arrive into those a sink counter explains and the rest.

    `drops` are the DataRouter-path counters (parse_sink_counters), `module` the counter deltas of the ring
    sinks: the module's sysctl leaves, or the forwarder's counters under the same names (forwarder_counters).
    Datagram counters of the network are not messages (the DataRouter packs several per datagram) and
    are therefore not included here.
    """
    if sink.startswith("DataRouter"):
        explained = (
            drops.get("dr_buffer_full_msgs", 0)
            + drops.get("dr_invalid_size_msgs", 0)
            + drops.get("app_no_slot", 0)
            + drops.get("app_too_long", 0)
        )
    else:
        explained = module.get("ring_drops_full", 0) + module.get("ring_drops_oversize", 0)
    return {"explained": explained, "unaccounted": max(summary["lost"] - explained, 0)}


def _stat(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def aggregate(runs):
    """Median and range over repetitions of one scenario and sink.

    Each run is a dict with `result` (application), `summary` (host), `drops`, `module`, `net` and `loss`.
    """
    keys = {
        "result": ("mean_ns", "p50_ns", "p99_ns", "max_ns"),
        "summary": ("sent", "delivered", "loss_pct", "reordered", "msgs_per_s", "dv_p50_ms", "dv_p99_ms", "dv_max_ms"),
        "loss": ("explained", "unaccounted"),
    }
    combined = {"runs": len(runs)}
    for group, names in keys.items():
        for name in names:
            combined[name] = _stat([run[group][name] for run in runs])
    for group in ("drops", "module", "net"):
        for name in sorted({name for run in runs for name in run[group]}):
            combined[name] = _stat([run[group].get(name, 0) for run in runs])
    return combined


def _cell(stat, precision=0):
    if stat["min"] == stat["max"]:
        return f"{stat['median']:.{precision}f}"
    return f"{stat['median']:.{precision}f} ({stat['min']:.{precision}f}..{stat['max']:.{precision}f})"


def format_aggregate_table(groups):
    """Markdown table of median (min..max) per scenario and sink; `groups` is a list of (scenario, sink, aggregate)."""
    header = (
        "| Scenario | Sink | runs | call mean ns | call p99 ns | delivered / sent | loss % | msgs/s (host) | "
        "queuing p50 ms | queuing p99 ms |\n"
        "|---|---|---|---|---|---|---|---|---|---|\n"
    )
    lines = [
        f"| {scenario} | {sink} | {agg['runs']} | {_cell(agg['mean_ns'])} | {_cell(agg['p99_ns'])} | "
        f"{_cell(agg['delivered'])} / {agg['sent']['median']:.0f} | {_cell(agg['loss_pct'], 1)} | "
        f"{_cell(agg['msgs_per_s'])} | {_cell(agg['dv_p50_ms'], 1)} | {_cell(agg['dv_p99_ms'], 1)} |"
        for scenario, sink, agg in groups
    ]
    return header + "\n".join(lines) + "\n"


_DROP_COLUMNS = (
    ("app_no_slot", "app no slot"),
    ("app_too_long", "app too long"),
    ("dr_buffer_full_msgs", "DR buffer full"),
    ("dr_invalid_size_msgs", "DR invalid size"),
    ("dr_send_failures", "DR send failed"),
    ("ring_drops_full", "ring full"),
    ("ring_drops_oversize", "ring oversize"),
    ("send_failures", "module send failed"),
    ("packets_sent", "module messages"),
    ("datagrams_sent", "module datagrams"),
    ("sendmsg_calls", "forwarder sendmsg calls"),
    ("host_udp_RcvbufErrors", "host UDP rcvbuf"),
    ("tap0_rx_dropped", "tap0 rx dropped"),
)


def format_drop_table(groups):
    """Markdown table of where messages were lost; counters not available for a sink show as '-'."""
    header = (
        "| Scenario | Sink | lost (median) | explained by sink counters | unaccounted | "
        + " | ".join(title for _key, title in _DROP_COLUMNS)
        + " |\n|"
        + "---|" * (5 + len(_DROP_COLUMNS))
        + "\n"
    )
    lines = []
    for scenario, sink, agg in groups:
        lost = agg["sent"]["median"] - agg["delivered"]["median"]
        cells = [_cell(agg[key]) if key in agg else "-" for key, _title in _DROP_COLUMNS]
        lines.append(
            f"| {scenario} | {sink} | {lost:.0f} | {_cell(agg['explained'])} | {_cell(agg['unaccounted'])} | "
            + " | ".join(cells)
            + " |"
        )
    return header + "\n".join(lines) + "\n"


_PIDIN_NAMES = ("datarouter", "io-sock")


def _cpu_token_to_ns(token):
    """CPU time as printed by pidin: nanoseconds (documented for %u and %v), or seconds as 'S.fff' or 'H:MM:SS.fff'."""
    if token.isdigit():
        return int(token)
    seconds = 0.0
    for part in token.split(":"):
        seconds = seconds * 60.0 + float(part)
    return int(seconds * 1e9)


def parse_pidin_cpu(text, names=_PIDIN_NAMES):
    """Total user + system CPU time in ns of the processes whose name contains one of `names`.

    `text` is the output of `pidin -F "%a %u %v %N"` (pid, user time, system time, name). Returns None for a name
    that no line matched, so that an unexpected output format is noticed instead of reading as 0.
    """
    per_process = {}
    for line in text.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 4 or not fields[0].isdigit():
            continue
        try:
            cpu_ns = _cpu_token_to_ns(fields[1]) + _cpu_token_to_ns(fields[2])
        except ValueError:
            continue
        per_process[fields[0]] = (fields[3], cpu_ns)  # one line per thread repeats the process values
    totals = {name: None for name in names}
    for process_name, cpu_ns in per_process.values():
        for name in names:
            if name in process_name:
                totals[name] = (totals[name] or 0) + cpu_ns
    return totals


def parse_pidin_pids(text, name):
    """Process ids of the processes whose name contains `name`, from `pidin -F "%a %u %v %N"` output."""
    pids = set()
    for line in text.splitlines():
        fields = line.split(None, 3)
        if len(fields) == 4 and fields[0].isdigit() and name in fields[3]:
            pids.add(int(fields[0]))
    return sorted(pids)


def cpu_summary(before, after, app_cpu_us, window_s, messages, idle_ns_per_s, datagrams=None):
    """CPU consumed by the application and by the processes in `before` (DataRouter, io-sock, ring_forwarder).

    `before` and `after` are parse_pidin_cpu() results for the same names, `idle_ns_per_s` the same processes
    measured without load (polling and housekeeping). Percentages are of one core over the window; the
    `*_us_per_msg` figures subtract the idle share. `transport_*` is everything except the application, i.e.
    the cost of getting the messages out. With `datagrams` (datagrams the sink sent in the run) the transport
    cost per datagram is added: the figure in which the module and the forwarder differ by the IPC hop.
    """
    components = {"app": app_cpu_us * 1000}
    for name in before:
        components[name] = max(after[name] - before[name], 0)
    summary = {"window_s": window_s, "messages": messages}
    total = 0.0
    total_net = 0.0
    transport = 0.0
    transport_net = 0.0
    for name, cpu_ns in components.items():
        net = max(cpu_ns - idle_ns_per_s.get(name, 0.0) * window_s, 0.0)
        summary[f"{name}_pct"] = 100.0 * cpu_ns / (window_s * 1e9)
        summary[f"{name}_us_per_msg"] = net / 1000.0 / messages if messages else 0.0
        total += cpu_ns
        total_net += net
        if name != "app":
            transport += cpu_ns
            transport_net += net
    summary["total_pct"] = 100.0 * total / (window_s * 1e9)
    summary["total_us_per_msg"] = total_net / 1000.0 / messages if messages else 0.0
    summary["transport_pct"] = 100.0 * transport / (window_s * 1e9)
    summary["transport_us_per_msg"] = transport_net / 1000.0 / messages if messages else 0.0
    if datagrams is not None:
        summary["datagrams"] = datagrams
        summary["transport_us_per_datagram"] = transport_net / 1000.0 / datagrams if datagrams else 0.0
    return summary


def aggregate_cpu(runs):
    """Median and range over repetitions of one CPU scenario and sink; each run is a cpu_summary() dict."""
    return {key: _stat([run[key] for run in runs]) for key in runs[0]} | {"runs": len(runs)}


def format_idle(idle_runs):
    """One line with the idle CPU share (percent of one core) of the measured processes over the repetitions."""
    parts = [
        f"{name} {_cell(_stat([100.0 * run[name] / 1e9 for run in idle_runs]), 2)} %" for name in idle_runs[0]
    ]
    return "Idle (module loaded, no application): " + ", ".join(parts)


def _optional_cell(agg, key, precision):
    return _cell(agg[key], precision) if key in agg else "-"


def format_cpu_table(groups):
    """Markdown table of CPU use; `groups` is a list of (scenario, sink, aggregate_cpu result).

    The forwarder and per-datagram columns appear only if some group has them; a process that was not measured
    for a sink shows as '-'.
    """
    with_forwarder = any("ring_forwarder_pct" in agg for _scenario, _sink, agg in groups)
    with_datagrams = any("transport_us_per_datagram" in agg for _scenario, _sink, agg in groups)
    titles = ["app %", "DataRouter %", "io-sock %"]
    if with_forwarder:
        titles.append("forwarder %")
    titles += ["total % of one core", "total us per message (idle share removed)"]
    if with_datagrams:
        titles.append("transport us per datagram (idle share removed)")
    header = "| Scenario | Sink | runs | " + " | ".join(titles) + " |\n|" + "---|" * (3 + len(titles)) + "\n"
    lines = []
    for scenario, sink, agg in groups:
        cells = [
            _optional_cell(agg, "app_pct", 1),
            _optional_cell(agg, "datarouter_pct", 1),
            _optional_cell(agg, "io-sock_pct", 1),
        ]
        if with_forwarder:
            cells.append(_optional_cell(agg, "ring_forwarder_pct", 1))
        cells += [_optional_cell(agg, "total_pct", 1), _optional_cell(agg, "total_us_per_msg", 1)]
        if with_datagrams:
            cells.append(_optional_cell(agg, "transport_us_per_datagram", 2))
        lines.append(f"| {scenario} | {sink} | {agg['runs']} | " + " | ".join(cells) + " |")
    return header + "\n".join(lines) + "\n"
