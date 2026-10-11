/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
// Copyright (c) 2026 Qorix
// SPDX-License-Identifier: Apache-2.0
// Load generator for comparing mw::log sinks (DataRouter "kRemote" versus the DLT ring "kCustom").
//
// Usage: mwlog_bench_app <context> <count> <rate_per_s> <text_bytes> [report_wait_s] [spin]
//   rate_per_s = 0 sends as fast as possible. Every message carries its sequence number, so the
//   receiver can count loss and reordering: "bench <seq> <text of text_bytes bytes>".
//   report_wait_s > 0 waits that long after the run and then logs one more message. The mw::log DataRouter
//   recorder publishes its drop counters (context STAT) at most every 5 s, with the next log call.
//   spin = 0 paces by sleeping only (late messages are sent back to back), so the application's own CPU time
//   is not dominated by waiting; the default 1 spins for the last stretch for accurate kHz timing.
//
// Prints one line to stdout, parsed by test_iosock_dlt_bench.py:
//   BENCH_RESULT ctx=<c> count=<n> rate=<r> size=<s> total_us=<t> cpu_us=<u> min_ns=.. mean_ns=.. p50_ns=..
//   p90_ns=.. p99_ns=.. p999_ns=.. max_ns=..
// The times are the duration of one complete log statement (stream creation to destruction), the cost the
// application thread pays per message. cpu_us is the CPU time of the process during the run (0 if the
// platform does not provide it).

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "score/mw/log/logging.h"

namespace
{

std::uint64_t NowNs()
{
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL) + static_cast<std::uint64_t>(now.tv_nsec);
}

// Sleeps for long gaps and, with `spin`, spins for the last stretch because the scheduler tick is too coarse
// for kHz rates. Without `spin` it only sleeps (the sleep is rounded up to the tick, late messages catch up).
void WaitUntil(const std::uint64_t deadline_ns, const bool spin)
{
    constexpr std::uint64_t kSleepThresholdNs = 2000000ULL;
    constexpr std::uint64_t kSleepMarginNs = 1000000ULL;
    for (;;)
    {
        const std::uint64_t now = NowNs();
        if (now >= deadline_ns)
        {
            return;
        }
        const std::uint64_t gap = deadline_ns - now;
        if (!spin || (gap > kSleepThresholdNs))
        {
            const std::uint64_t sleep_ns = spin ? (gap - kSleepMarginNs) : gap;
            const timespec request{static_cast<time_t>(sleep_ns / 1000000000ULL),
                                   static_cast<long>(sleep_ns % 1000000000ULL)};
            ::nanosleep(&request, nullptr);
        }
    }
}

// CPU time of this process in microseconds, 0 if the clock is not available.
std::uint64_t ProcessCpuUs()
{
    timespec now{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0)
    {
        return 0ULL;
    }
    return (static_cast<std::uint64_t>(now.tv_sec) * 1000000ULL) + (static_cast<std::uint64_t>(now.tv_nsec) / 1000ULL);
}

std::uint32_t Percentile(const std::vector<std::uint32_t>& sorted, const double fraction)
{
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1U));
    return sorted[index];
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <context> <count> <rate_per_s> <text_bytes> [report_wait_s] [spin]\n", argv[0]);
        return 2;
    }
    const char* const context = argv[1];
    const long count = std::strtol(argv[2], nullptr, 10);
    const long rate = std::strtol(argv[3], nullptr, 10);
    const long text_bytes = std::strtol(argv[4], nullptr, 10);
    const long report_wait_s = (argc > 5) ? std::strtol(argv[5], nullptr, 10) : 0;
    const bool spin = (argc > 6) ? (std::strtol(argv[6], nullptr, 10) != 0) : true;
    if ((count <= 0) || (rate < 0) || (text_bytes < 0) || (text_bytes > 1024) || (report_wait_s < 0))
    {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    const std::string padding(1024U, 'x');
    const std::string_view text{padding.data(), static_cast<std::size_t>(text_bytes)};

    // Initialises the mw::log runtime (config, recorder, DataRouter connection) outside the measurement.
    score::mw::log::LogInfo("INIT") << "bench-init";

    std::vector<std::uint32_t> call_ns(static_cast<std::size_t>(count));
    const std::uint64_t interval_ns = (rate > 0) ? (1000000000ULL / static_cast<std::uint64_t>(rate)) : 0ULL;
    const std::uint64_t cpu_start_us = ProcessCpuUs();
    const std::uint64_t start_ns = NowNs();

    for (long i = 0; i < count; ++i)
    {
        if (interval_ns != 0U)
        {
            WaitUntil(start_ns + (static_cast<std::uint64_t>(i) * interval_ns), spin);
        }
        const std::uint64_t before = NowNs();
        score::mw::log::LogInfo(context) << "bench" << static_cast<std::uint32_t>(i) << text;
        const std::uint64_t elapsed = NowNs() - before;
        call_ns[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed, 0xFFFFFFFFULL));
    }
    const std::uint64_t total_us = (NowNs() - start_ns) / 1000ULL;
    const std::uint64_t cpu_us = ProcessCpuUs() - cpu_start_us;

    std::uint64_t sum = 0;
    for (const std::uint32_t value : call_ns)
    {
        sum += value;
    }
    std::sort(call_ns.begin(), call_ns.end());
    std::printf(
        "BENCH_RESULT ctx=%s count=%ld rate=%ld size=%ld total_us=%llu cpu_us=%llu min_ns=%u mean_ns=%llu p50_ns=%u "
        "p90_ns=%u p99_ns=%u p999_ns=%u max_ns=%u\n",
        context,
        count,
        rate,
        text_bytes,
        static_cast<unsigned long long>(total_us),
        static_cast<unsigned long long>(cpu_us),
        call_ns.front(),
        static_cast<unsigned long long>(sum / static_cast<std::uint64_t>(count)),
        Percentile(call_ns, 0.50),
        Percentile(call_ns, 0.90),
        Percentile(call_ns, 0.99),
        Percentile(call_ns, 0.999),
        call_ns.back());
    std::fflush(stdout);

    if (report_wait_s > 0)
    {
        const timespec request{static_cast<time_t>(report_wait_s), 0L};
        ::nanosleep(&request, nullptr);
        score::mw::log::LogInfo("INIT") << "bench-final";
    }
    return 0;
}
