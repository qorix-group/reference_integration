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
 * @file ring_forwarder.cpp
 * @brief Userspace counterpart of the io-sock module: ring -> DLT frames -> sendmsg() over a BSD socket.
 *
 * Runs the very same portable core as the module (ShmRingRecordSource, DltWireFormatter, Pipeline with
 * the same batching, ECU id and round size of 32 records). Only the transport differs: every datagram
 * is handed to io-sock with sendmsg() from a normal process instead of sosend() inside io-sock. Compared
 * with the module on the same QNX target it shows what the removed socket IPC hop is worth.
 *
 * The forwarder creates and formats its own ring (default size as the module: 256 slots of 512 bytes,
 * world read/write) and removes it on exit. Use a name different from the module's /score_dlt_ring so
 * both can run in one target session, and point the producers at it with DLT_RING_NAME.
 *
 * Usage:
 *   ring_forwarder <shm-name> <dst-ip> <dst-port> [slots] [slot-payload] [batch-bytes] [poll-ms] [duration-s]
 *     batch-bytes  0: one DLT message per datagram, else the largest datagram payload assembled (1472)
 *     poll-ms      idle wait between rounds, 1..999 (default 1; the QNX scheduler rounds it up to one tick)
 *     duration-s   0 (default) runs until SIGTERM or SIGINT
 *   SIGUSR1 prints the counters (same line as at exit) without stopping.
 *
 * Output (stdout, one line each): "ring_forwarder: ready ..." once the ring exists, then
 * "ring_forwarder: records=... sent=... datagrams=... sendmsg_calls=..." on SIGUSR1 and at exit.
 * Exit code: 0 if no send failed, 1 if a send failed, 2 on a usage or setup error.
 */

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/shm_ring.h"
#include "dlt/shm_ring_source.h"
#include "posix_udp_transport.h"

using namespace dlt;
using dlt::tools::PosixUdpTransport;

namespace
{

/// Records handled per round, equal to DLT_RING_BATCH in src/qnx/iosock_module.c.
constexpr u32 kRoundRecords = 32U;
constexpr unsigned long kNanosPerMilli = 1000000UL;
constexpr unsigned long long kNanosPerSecond = 1000000000ULL;

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_report = 0;

/**
 * @brief Signal handler: asks the main loop to stop (SIGTERM, SIGINT).
 */
void OnStopSignal(int /*signal_number*/)
{
    g_stop = 1;
}

/**
 * @brief Signal handler: asks the main loop to print the counters (SIGUSR1).
 */
void OnReportSignal(int /*signal_number*/)
{
    g_report = 1;
}

/**
 * @brief Installs `handler` for `signal_number` without SA_RESTART, so that a sleeping loop wakes up.
 */
void Install(int signal_number, void (*handler)(int))
{
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(signal_number, &action, nullptr);
}

/**
 * @brief CLOCK_MONOTONIC in nanoseconds.
 */
u64 MonotonicNanos() noexcept
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<u64>(now.tv_sec) * kNanosPerSecond) + static_cast<u64>(now.tv_nsec);
}

/**
 * @brief Parses an optional unsigned command-line argument.
 *
 * @param argc          Argument count of main().
 * @param argv          Arguments of main().
 * @param index         Position of the argument.
 * @param default_value Result if the argument is absent.
 * @param value         Receives the parsed value.
 * @return false if the argument is present but not a plain unsigned number.
 */
bool ParseOptional(int argc, char** argv, int index, u32 default_value, u32& value)
{
    if (index >= argc)
    {
        value = default_value;
        return true;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(argv[index], &end, 10);
    if ((errno != 0) || (end == argv[index]) || (*end != '\0') || (parsed > 0xFFFFFFFFUL))
    {
        return false;
    }
    value = static_cast<u32>(parsed);
    return true;
}

/**
 * @brief Creates, sizes, maps and formats the ring (world read/write, a stale object is replaced).
 *
 * @param name         Shared-memory object name, for example "/score_dlt_ring_fwd".
 * @param slots        Number of slots (power of two).
 * @param slot_payload Payload capacity per slot (multiple of 8).
 * @param size         Receives the size of the mapping.
 * @return Start of the mapping, or nullptr on failure (a message was printed).
 */
void* CreateRing(const char* name, u32 slots, u32 slot_payload, usize& size)
{
    size = dlt_shm_ring_required_size(slots, slot_payload);
    if (size == 0U)
    {
        std::fprintf(stderr, "invalid ring geometry %u x %u\n", slots, slot_payload);
        return nullptr;
    }

    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if ((fd < 0) || (fchmod(fd, 0666) != 0) || (ftruncate(fd, static_cast<off_t>(size)) != 0))
    {
        std::perror("cannot create the shared memory ring");
        if (fd >= 0)
        {
            close(fd);
            shm_unlink(name);
        }
        return nullptr;
    }
    void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if ((mem == MAP_FAILED) || (dlt_shm_ring_init(mem, size, slots, slot_payload) != 0))
    {
        std::perror("cannot map and format the ring");
        shm_unlink(name);
        return nullptr;
    }
    return mem;
}

/**
 * @brief Prints one statistics line and flushes, so that a redirected log can be read while running.
 */
void PrintStats(const TransportStats& stats, const ShmRingRecordSource& source, const PosixUdpTransport& transport)
{
    std::printf(
        "ring_forwarder: records=%llu sent=%llu datagrams=%llu bytes=%llu dropped=%llu failures=%llu "
        "sendmsg_calls=%llu sendmsg_errors=%llu ring_full=%u ring_oversize=%u corrupt=%u\n",
        static_cast<unsigned long long>(stats.records_received), static_cast<unsigned long long>(stats.packets_sent),
        static_cast<unsigned long long>(stats.datagrams_sent), static_cast<unsigned long long>(stats.bytes_sent),
        static_cast<unsigned long long>(stats.packets_dropped), static_cast<unsigned long long>(stats.send_failures),
        static_cast<unsigned long long>(transport.calls()), static_cast<unsigned long long>(transport.errors()),
        source.drops_full(), source.drops_oversize(), source.corrupt());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr,
                     "usage: %s <shm-name> <dst-ip> <dst-port> [slots] [slot-payload] [batch-bytes] [poll-ms] "
                     "[duration-s]\n",
                     argv[0]);
        return 2;
    }
    const char* const name = argv[1];
    u32 port = 0U;
    u32 slots = 0U;
    u32 slot_payload = 0U;
    u32 batch_bytes = 0U;
    u32 poll_ms = 0U;
    u32 duration_s = 0U;
    if (!ParseOptional(argc, argv, 3, 0U, port) || (port == 0U) || (port > 65535U) ||
        !ParseOptional(argc, argv, 4, 256U, slots) || !ParseOptional(argc, argv, 5, 512U, slot_payload) ||
        !ParseOptional(argc, argv, 6, 0U, batch_bytes) || !ParseOptional(argc, argv, 7, 1U, poll_ms) ||
        !ParseOptional(argc, argv, 8, 0U, duration_s) || (poll_ms == 0U) || (poll_ms > 999U))
    {
        std::fprintf(stderr, "invalid numeric argument (poll-ms must be 1..999)\n");
        return 2;
    }

    usize size = 0U;
    void* const mem = CreateRing(name, slots, slot_payload, size);
    if (mem == nullptr)
    {
        return 2;
    }

    PosixUdpTransport transport;
    ShmRingRecordSource source;
    DltWireFormatter formatter{MakeDltId("IOS1")};
    if (!transport.Open(argv[2], port) || !source.Attach(mem, size))
    {
        std::fprintf(stderr, "cannot open UDP socket towards %s:%s\n", argv[2], argv[3]);
        shm_unlink(name);
        return 2;
    }
    Pipeline<ShmRingRecordSource, PosixUdpTransport> pipeline{source, transport, formatter, batch_bytes};

    Install(SIGTERM, OnStopSignal);
    Install(SIGINT, OnStopSignal);
    Install(SIGUSR1, OnReportSignal);

    std::printf("ring_forwarder: ready, ring %s (%u slots of %u bytes), batch %u bytes, poll %u ms\n", name, slots,
                slot_payload, batch_bytes, poll_ms);
    std::fflush(stdout);

    const u64 deadline = (duration_s == 0U) ? 0U : (MonotonicNanos() + (duration_s * kNanosPerSecond));
    while ((g_stop == 0) && ((deadline == 0U) || (MonotonicNanos() < deadline)))
    {
        if (g_report != 0)
        {
            g_report = 0;
            PrintStats(pipeline.stats(), source, transport);
        }
        if (pipeline.Pump(kRoundRecords) == 0U)
        {
            timespec idle{0, static_cast<long>(poll_ms * kNanosPerMilli)};
            nanosleep(&idle, nullptr);
        }
    }

    const TransportStats& stats = pipeline.stats();
    PrintStats(stats, source, transport);
    munmap(mem, size);
    shm_unlink(name);
    return (stats.send_failures == 0U) ? 0 : 1;
}
