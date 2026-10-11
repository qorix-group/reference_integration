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
/*
 * Copyright (c) 2026 Qorix
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file host_udp_sender.cpp
 * @brief Linux-only harness: sends the synthetic DLT stream over a real UDP socket.
 *
 * This is NOT the QNX transport. It exists so the wire format produced by the portable core can be
 * checked end to end (formatter -> UDP -> independent decoder in tools/dlt_udp_check.py) without a
 * QNX target.
 *
 * Usage: host_udp_sender <dst-ip> <dst-port> <record-count>
 */

#include <cstdio>
#include <cstdlib>

#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/record_source.h"
#include "host_udp_transport.h"

using namespace dlt;
using dlt::host::LinuxUdpTransport;
using dlt::host::MonotonicNanos;

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: %s <dst-ip> <dst-port> <record-count>\n", argv[0]);
        return 2;
    }

    LinuxUdpTransport transport;
    if (!transport.Open(argv[1], static_cast<unsigned>(std::atoi(argv[2]))))
    {
        std::fprintf(stderr, "cannot open UDP socket towards %s:%s\n", argv[1], argv[2]);
        return 2;
    }

    const u32 count = static_cast<u32>(std::atoi(argv[3]));
    SyntheticRecordSource source{SyntheticConfig{
        count, NanosToDltTimestamp(MonotonicNanos()), 1000U, LogLevel::kInfo, MakeDltId("DLTM"), MakeDltId("SYN")}};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    Pipeline<SyntheticRecordSource, LinuxUdpTransport> pipeline{source, transport, formatter};

    static_cast<void>(pipeline.Pump(count));

    const TransportStats& stats = pipeline.stats();
    std::printf("records=%llu sent=%llu bytes=%llu dropped=%llu failures=%llu\n",
                static_cast<unsigned long long>(stats.records_received),
                static_cast<unsigned long long>(stats.packets_sent),
                static_cast<unsigned long long>(stats.bytes_sent),
                static_cast<unsigned long long>(stats.packets_dropped),
                static_cast<unsigned long long>(stats.send_failures));
    return ((stats.send_failures == 0U) && (stats.packets_dropped == 0U)) ? 0 : 1;
}
