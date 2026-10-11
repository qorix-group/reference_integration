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
"""Unit tests for bench_metrics.py (host only, no target): CPU accounting, forwarder counters, tables.

    bazel test //feature_integration_tests/itf:bench_metrics_test
    python3 feature_integration_tests/itf/test_bench_metrics.py
"""

import unittest

import bench_metrics

# pidin -F "%a %u %v %N": pid, user ns, system ns, name; one line per thread repeats the process values.
PIDIN = """\
1 100 200 /proc/boot/procnto-smp-instr
4099 1000000 2000000 sbin/io-sock
4099 1000000 2000000 sbin/io-sock
4099 1000000 2000000 sbin/io-sock
8200 500 1500 /tmp_ram/ring_forwarder
9000 10 20 usr/bin/datarouter
"""

FORWARDER_LOG = """\
ring_forwarder: ready, ring /score_dlt_ring_fwd (2048 slots of 512 bytes), batch 0 bytes, poll 1 ms
ring_forwarder: records=10 sent=10 datagrams=10 bytes=700 dropped=0 failures=0 sendmsg_calls=10 sendmsg_errors=0 ring_full=0 ring_oversize=0 corrupt=0
ring_forwarder: records=20 sent=20 datagrams=18 bytes=1400 dropped=0 failures=1 sendmsg_calls=18 sendmsg_errors=1 ring_full=2 ring_oversize=3 corrupt=0
"""


class PidinTest(unittest.TestCase):
    def test_threads_of_one_process_count_once(self):
        cpu = bench_metrics.parse_pidin_cpu(PIDIN, ("io-sock", "ring_forwarder", "datarouter"))
        self.assertEqual(cpu["io-sock"], 3000000)
        self.assertEqual(cpu["ring_forwarder"], 2000)
        self.assertEqual(cpu["datarouter"], 30)

    def test_missing_process_is_none(self):
        cpu = bench_metrics.parse_pidin_cpu(PIDIN, ("io-sock", "no-such-process"))
        self.assertIsNone(cpu["no-such-process"])
        self.assertEqual(cpu["io-sock"], 3000000)

    def test_default_names_do_not_include_the_forwarder(self):
        self.assertEqual(set(bench_metrics.parse_pidin_cpu(PIDIN)), {"datarouter", "io-sock"})

    def test_pids(self):
        self.assertEqual(bench_metrics.parse_pidin_pids(PIDIN, "ring_forwarder"), [8200])


class ForwarderStatsTest(unittest.TestCase):
    def test_last_statistics_line_wins(self):
        stats = bench_metrics.parse_forwarder_stats(FORWARDER_LOG)
        self.assertEqual(stats["records"], 20)
        self.assertEqual(stats["datagrams"], 18)
        self.assertEqual(stats["failures"], 1)

    def test_ready_line_alone_has_no_statistics(self):
        self.assertIsNone(bench_metrics.parse_forwarder_stats(FORWARDER_LOG.splitlines()[0]))
        self.assertIsNone(bench_metrics.parse_forwarder_stats(""))

    def test_counters_use_the_module_names(self):
        counters = bench_metrics.forwarder_counters(bench_metrics.parse_forwarder_stats(FORWARDER_LOG))
        self.assertEqual(
            counters,
            {
                "packets_sent": 20,
                "datagrams_sent": 18,
                "packets_dropped": 0,
                "send_failures": 1,
                "ring_drops_full": 2,
                "ring_drops_oversize": 3,
                "sendmsg_calls": 18,
            },
        )

    def test_loss_is_explained_by_the_ring_counters(self):
        counters = bench_metrics.forwarder_counters(bench_metrics.parse_forwarder_stats(FORWARDER_LOG))
        loss = bench_metrics.explain_loss("Forwarder", {"lost": 7}, {}, counters)
        self.assertEqual(loss, {"explained": 5, "unaccounted": 2})


class CpuSummaryTest(unittest.TestCase):
    BEFORE = {"datarouter": 0, "io-sock": 0, "ring_forwarder": 0}
    AFTER = {"datarouter": 100_000_000, "io-sock": 200_000_000, "ring_forwarder": 300_000_000}

    def test_processes_present_are_summed_and_transport_excludes_the_application(self):
        summary = bench_metrics.cpu_summary(self.BEFORE, self.AFTER, 40_000, 10.0, 1000, {})
        self.assertAlmostEqual(summary["app_pct"], 0.4)
        self.assertAlmostEqual(summary["ring_forwarder_pct"], 3.0)
        self.assertAlmostEqual(summary["total_pct"], 6.4)
        self.assertAlmostEqual(summary["transport_pct"], 6.0)
        self.assertAlmostEqual(summary["transport_us_per_msg"], 600.0)
        self.assertNotIn("transport_us_per_datagram", summary)

    def test_idle_share_is_removed_from_per_message_figures_only(self):
        idle = {"ring_forwarder": 10_000_000.0}  # ns per second, 100 ms over a 10 s window
        summary = bench_metrics.cpu_summary(self.BEFORE, self.AFTER, 0, 10.0, 1000, idle)
        self.assertAlmostEqual(summary["ring_forwarder_pct"], 3.0)
        self.assertAlmostEqual(summary["ring_forwarder_us_per_msg"], 200.0)

    def test_cost_per_datagram(self):
        summary = bench_metrics.cpu_summary(self.BEFORE, self.AFTER, 0, 10.0, 1000, {}, datagrams=400)
        self.assertEqual(summary["datagrams"], 400)
        self.assertAlmostEqual(summary["transport_us_per_datagram"], 1500.0)

    def test_zero_datagrams_do_not_divide_by_zero(self):
        summary = bench_metrics.cpu_summary(self.BEFORE, self.AFTER, 0, 10.0, 1000, {}, datagrams=0)
        self.assertEqual(summary["transport_us_per_datagram"], 0.0)

    def test_without_forwarder_the_old_processes_only(self):
        before = {"datarouter": 0, "io-sock": 0}
        after = {"datarouter": 100_000_000, "io-sock": 200_000_000}
        summary = bench_metrics.cpu_summary(before, after, 0, 10.0, 1000, {})
        self.assertNotIn("ring_forwarder_pct", summary)
        self.assertAlmostEqual(summary["total_pct"], 3.0)


class CpuTableTest(unittest.TestCase):
    @staticmethod
    def _group(sink, names, datagrams=None):
        before = {name: 0 for name in names}
        after = {name: 100_000_000 for name in names}
        run = bench_metrics.cpu_summary(before, after, 100_000, 10.0, 1000, {}, datagrams=datagrams)
        return ("cpu 1k/s, 16 B", sink, bench_metrics.aggregate_cpu([run, run]))

    def test_table_without_forwarder_keeps_its_columns(self):
        table = bench_metrics.format_cpu_table([self._group("DataRouter", ["datarouter", "io-sock"])])
        header = table.splitlines()[0]
        self.assertNotIn("forwarder", header)
        self.assertNotIn("per datagram", header)
        self.assertEqual(table.splitlines()[1], "|" + "---|" * 8)

    def test_table_with_forwarder_and_datagrams(self):
        groups = [
            self._group("DataRouter", ["datarouter", "io-sock", "ring_forwarder"]),
            self._group("Ring", ["io-sock", "ring_forwarder"], datagrams=500),
            self._group("Forwarder", ["io-sock", "ring_forwarder"], datagrams=500),
        ]
        lines = bench_metrics.format_cpu_table(groups).splitlines()
        self.assertIn("forwarder %", lines[0])
        self.assertIn("transport us per datagram", lines[0])
        self.assertEqual(lines[1], "|" + "---|" * 10)
        self.assertEqual(len(lines), 5)
        # A sink without the DataRouter process shows '-' there; a sink without datagram counts too.
        self.assertIn("| - |", lines[3])
        self.assertTrue(lines[2].rstrip().endswith("| - |"))


class DropTableTest(unittest.TestCase):
    def test_forwarder_calls_have_a_column(self):
        titles = [title for _key, title in bench_metrics._DROP_COLUMNS]
        self.assertIn("forwarder sendmsg calls", titles)


if __name__ == "__main__":
    unittest.main()
