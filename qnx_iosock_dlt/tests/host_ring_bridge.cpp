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
 * @file host_ring_bridge.cpp
 * @brief Linux-only harness that plays the role of the io-sock module for the ring input.
 *
 * It creates and formats the shared-memory ring, reads the records producers write into it with the
 * portable core (ShmRingRecordSource -> DLT formatter) and sends the frames over a real UDP socket.
 * Together with tools/ring_producer.c and tools/dlt_udp_check.py this checks the whole path
 * producer -> ring -> DLT frame -> UDP without a QNX target.
 *
 * Usage: host_ring_bridge <shm-name> <dst-ip> <dst-port> <records> [slots] [slot-payload] [timeout-s] [batch-bytes]
 *   Forwards `records` records, then removes the shared memory object and exits. A non-zero
 *   `batch-bytes` packs several DLT messages into one datagram (up to that many bytes).
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "dlt/dlt_c_api.h"
#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/shm_ring_source.h"
#include "host_udp_transport.h"

using namespace dlt;
using dlt::host::LinuxUdpTransport;
using dlt::host::MonotonicNanos;

namespace
{

constexpr u32 kBatch = 32U;

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr,
                     "usage: %s <shm-name> <dst-ip> <dst-port> <records> [slots] [slot-payload] [timeout-s] [batch-bytes]\n",
                     argv[0]);
        return 2;
    }
    const char* name = argv[1];
    const u32 records = static_cast<u32>(std::atoi(argv[4]));
    const u32 slots = (argc > 5) ? static_cast<u32>(std::atoi(argv[5])) : 256U;
    const u32 slot_payload = (argc > 6) ? static_cast<u32>(std::atoi(argv[6])) : 512U;
    const u64 timeout_ns = ((argc > 7) ? static_cast<u64>(std::atoi(argv[7])) : 20ULL) * 1000000000ULL;
    const u32 batch_bytes = (argc > 8) ? static_cast<u32>(std::atoi(argv[8])) : 0U;

    const dlt_size size = dlt_ring_size_for(slots, slot_payload);
    if (size == 0U)
    {
        std::fprintf(stderr, "invalid ring geometry %u x %u\n", slots, slot_payload);
        return 2;
    }

    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if ((fd < 0) || (ftruncate(fd, static_cast<off_t>(size)) != 0))
    {
        std::perror("cannot create the shared memory ring");
        return 2;
    }
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if ((mem == MAP_FAILED) || (dlt_ring_format(mem, size, slots, slot_payload) != 0))
    {
        std::perror("cannot map and format the ring");
        shm_unlink(name);
        return 2;
    }

    LinuxUdpTransport transport;
    ShmRingRecordSource source;
    DltWireFormatter formatter{MakeDltId("IOS1")};
    if (!transport.Open(argv[2], static_cast<unsigned>(std::atoi(argv[3]))) || !source.Attach(mem, size))
    {
        std::fprintf(stderr, "cannot open UDP socket towards %s:%s\n", argv[2], argv[3]);
        shm_unlink(name);
        return 2;
    }
    Pipeline<ShmRingRecordSource, LinuxUdpTransport> pipeline{source, transport, formatter, batch_bytes};

    std::printf("host_ring_bridge: ring %s ready (%u slots of %u bytes), waiting for %u records\n", name, slots,
                slot_payload, records);
    std::fflush(stdout);

    const u64 deadline = MonotonicNanos() + timeout_ns;
    u32 forwarded = 0U;
    while ((forwarded < records) && (MonotonicNanos() < deadline))
    {
        const usize processed = pipeline.Pump(kBatch);
        forwarded += static_cast<u32>(processed);
        if (processed == 0U)
        {
            timespec pause{0, 1000000L};
            nanosleep(&pause, nullptr);
        }
    }

    const TransportStats& stats = pipeline.stats();
    std::printf("host_ring_bridge: forwarded=%u sent=%llu datagrams=%llu bytes=%llu dropped=%llu failures=%llu "
                "ring_full=%u ring_oversize=%u corrupt=%u\n",
                forwarded, static_cast<unsigned long long>(stats.packets_sent),
                static_cast<unsigned long long>(stats.datagrams_sent),
                static_cast<unsigned long long>(stats.bytes_sent),
                static_cast<unsigned long long>(stats.packets_dropped),
                static_cast<unsigned long long>(stats.send_failures), source.drops_full(), source.drops_oversize(),
                source.corrupt());
    munmap(mem, size);
    shm_unlink(name);
    return ((forwarded == records) && (stats.send_failures == 0U) && (stats.packets_dropped == 0U)) ? 0 : 1;
}
