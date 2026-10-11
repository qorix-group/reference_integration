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
 * @file shm_ring_test.cpp
 * @brief Host (Linux) tests for the shared-memory ring and its record source.
 *
 * Self-contained like dlt_wire_formatter_test.cpp: each CHECK counts, failures are printed, and the
 * exit code is non-zero if any check failed. The ring lives in ordinary heap memory here; the
 * multi-threaded test stands in for several producer processes.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "dlt/dlt_c_api.h"
#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/shm_ring.h"
#include "dlt/shm_ring_source.h"
#include "dlt/transport.h"

using namespace dlt;

namespace
{

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        ++g_checks;                                                          \
        if (!(condition))                                                    \
        {                                                                    \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                    \
    } while (false)

/* A formatted ring in heap memory together with a producer view and a consumer source. */
struct TestRing
{
    TestRing(u32 slots, u32 cap) : mem(dlt_shm_ring_required_size(slots, cap), 0U)
    {
        init_result = dlt_shm_ring_init(mem.data(), mem.size(), slots, cap);
        attach_result = dlt_shm_ring_attach(&view, mem.data(), mem.size());
        attached = source.Attach(mem.data(), mem.size());
    }

    int Write(const char* app, const char* ctx, u8 level, u64 ts_ns, const void* payload, u32 length, u8 nargs = 1U)
    {
        return dlt_shm_ring_write(&view, level, app, ctx, nargs, ts_ns, payload, length);
    }

    std::vector<u8> mem;
    int init_result{-1};
    int attach_result{-1};
    bool attached{false};
    dlt_shm_ring view{};
    ShmRingRecordSource source{};
};

void TestGeometry()
{
    CHECK(sizeof(dlt_shm_ring_header) == 192U);
    CHECK(sizeof(dlt_shm_ring_slot) == 32U);
    CHECK(dlt_shm_ring_required_size(2U, 8U) == 192U + 2U * 32U + 2U * 8U);
    CHECK(dlt_shm_ring_required_size(4U, 8U) == 352U);
    CHECK(dlt_shm_ring_required_size(256U, 512U) == 192U + 256U * 32U + 256U * 512U);
    CHECK(dlt_shm_ring_required_size(65536U, 1472U) != 0U);
    CHECK(dlt_shm_ring_required_size(1U, 8U) == 0U);        /* too few slots */
    CHECK(dlt_shm_ring_required_size(3U, 8U) == 0U);        /* not a power of two */
    CHECK(dlt_shm_ring_required_size(131072U, 8U) == 0U);   /* too many slots */
    CHECK(dlt_shm_ring_required_size(4U, 4U) == 0U);        /* payload too small */
    CHECK(dlt_shm_ring_required_size(4U, 12U) == 0U);       /* not a multiple of 8 */
    CHECK(dlt_shm_ring_required_size(4U, 1480U) == 0U);     /* payload too large */
    CHECK(dlt_ring_size_for(8U, 64U) == dlt_shm_ring_required_size(8U, 64U));
}

void TestInitAndAttach()
{
    std::vector<u8> mem(dlt_shm_ring_required_size(4U, 16U), 0U);
    dlt_shm_ring view{};

    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == -1); /* not formatted yet: no magic */
    CHECK(dlt_shm_ring_init(mem.data(), mem.size() - 1U, 4U, 16U) == -1); /* region too small */
    CHECK(dlt_shm_ring_init(mem.data(), mem.size(), 3U, 16U) == -1);      /* bad geometry */
    CHECK(dlt_shm_ring_init(nullptr, mem.size(), 4U, 16U) == -1);
    CHECK(dlt_ring_format(mem.data(), mem.size(), 4U, 16U) == 0);
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == 0);
    CHECK(view.mask == 3U);
    CHECK(view.slot_payload_cap == 16U);

    auto* header = reinterpret_cast<dlt_shm_ring_header*>(mem.data());
    CHECK(header->meta_offset == 192U);
    CHECK(header->payload_offset == 192U + 4U * 32U);
    CHECK(header->total_size == mem.size());

    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size() - 1U) == -1); /* mapping smaller than the ring */

    header->version = 2U;
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == -1);
    header->version = DLT_SHM_RING_VERSION;
    header->slot_count = 5U;
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == -1);
    header->slot_count = 4U;
    header->payload_offset += 8U;
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == -1);
    header->payload_offset -= 8U;
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == 0);

    ShmRingRecordSource source;
    CHECK(!source.attached());
    LogRecordView record{};
    CHECK(!source.TryRead(record)); /* not attached: empty, no crash */
}

void TestRoundTrip()
{
    TestRing ring{4U, 32U};
    CHECK(ring.init_result == 0 && ring.attach_result == 0 && ring.attached);
    CHECK(ring.source.slot_count() == 4U);
    CHECK(ring.source.slot_payload_cap() == 32U);

    LogRecordView record{};
    CHECK(!ring.source.TryRead(record)); /* empty */

    const u8 payload[10] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U};
    CHECK(ring.Write("APP1", "CT\0\0", 4U, 123456789ULL, payload, sizeof(payload), 2U) == DLT_SHM_RING_WRITE_OK);
    CHECK(ring.source.TryRead(record));
    CHECK(record.payload_size == 10U);
    CHECK(std::memcmp(record.payload, payload, sizeof(payload)) == 0);
    CHECK(record.timestamp == 1234U); /* 123456789 ns = 1234.56789 ticks of 0.1 ms */
    CHECK(record.log_level == LogLevel::kInfo);
    CHECK(record.num_of_args == 2U);
    CHECK(std::memcmp(record.app_id.chars, "APP1", 4U) == 0);
    CHECK(std::memcmp(record.ctx_id.chars, "CT\0\0", 4U) == 0);
    CHECK(!ring.source.TryRead(record)); /* consumed */

    CHECK(ring.Write("A\0\0\0", "B\0\0\0", 3U, 0ULL, nullptr, 0U, 0U) == DLT_SHM_RING_WRITE_OK); /* empty payload */
    CHECK(ring.source.TryRead(record));
    CHECK(record.payload_size == 0U);
}

void TestOrderAcrossWraparound()
{
    TestRing ring{4U, 8U};
    LogRecordView record{};
    u32 next_write = 0U;
    u32 next_read = 0U;
    bool in_order = true;

    for (u32 round = 0U; round < 1000U; ++round)
    {
        const u32 burst = 1U + (round % 4U); /* 1..4 records fit the ring */
        for (u32 i = 0U; i < burst; ++i)
        {
            if (ring.Write("W", "R", 4U, next_write, &next_write, sizeof(next_write)) != DLT_SHM_RING_WRITE_OK)
            {
                in_order = false;
            }
            ++next_write;
        }
        while (ring.source.TryRead(record))
        {
            u32 value = 0U;
            std::memcpy(&value, record.payload, sizeof(value));
            in_order = in_order && (value == next_read);
            ++next_read;
        }
    }
    CHECK(in_order);
    CHECK(next_read == next_write);
    CHECK(ring.source.drops_full() == 0U);
}

void TestFullRingDropsAndCounts()
{
    TestRing ring{4U, 8U};
    const u8 byte = 7U;
    for (int i = 0; i < 4; ++i)
    {
        CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_OK);
    }
    CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_FULL);
    CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_FULL);
    CHECK(ring.source.drops_full() == 2U);

    LogRecordView record{};
    CHECK(ring.source.TryRead(record));
    CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_FULL); /* slot still held */
    CHECK(ring.source.TryRead(record));                                          /* releases the first slot */
    CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_OK);
}

void TestPayloadStaysValidUntilNextRead()
{
    TestRing ring{2U, 8U};
    const u8 first = 0xA1U;
    const u8 second = 0xB2U;
    const u8 third = 0xC3U;
    CHECK(ring.Write("A", "B", 4U, 1ULL, &first, 1U) == DLT_SHM_RING_WRITE_OK);
    CHECK(ring.Write("A", "B", 4U, 2ULL, &second, 1U) == DLT_SHM_RING_WRITE_OK);

    LogRecordView record{};
    CHECK(ring.source.TryRead(record));
    const u8* held = record.payload;
    CHECK(*held == first);

    /* The ring is full while the consumer holds the first slot, so the producer cannot overwrite it. */
    CHECK(ring.Write("A", "B", 4U, 3ULL, &third, 1U) == DLT_SHM_RING_WRITE_FULL);
    CHECK(*held == first);

    CHECK(ring.source.TryRead(record));
    CHECK(*record.payload == second);
    CHECK(ring.Write("A", "B", 4U, 3ULL, &third, 1U) == DLT_SHM_RING_WRITE_OK);
    CHECK(ring.source.TryRead(record));
    CHECK(*record.payload == third);
    CHECK(!ring.source.TryRead(record));
}

void TestOversizeAndInvalid()
{
    TestRing ring{4U, 16U};
    u8 big[32] = {};
    CHECK(ring.Write("A", "B", 4U, 1ULL, big, 17U) == DLT_SHM_RING_WRITE_OVERSIZE);
    CHECK(ring.source.drops_oversize() == 1U);
    LogRecordView record{};
    CHECK(!ring.source.TryRead(record)); /* nothing was queued */
    CHECK(ring.Write("A", "B", 4U, 1ULL, big, 16U) == DLT_SHM_RING_WRITE_OK); /* exactly the capacity */
    CHECK(ring.source.TryRead(record));
    CHECK(record.payload_size == 16U);
    CHECK(ring.Write("A", "B", 4U, 1ULL, nullptr, 3U) == DLT_SHM_RING_WRITE_INVALID);
    CHECK(dlt_shm_ring_write(nullptr, 4U, "A", "B", 0U, 0ULL, nullptr, 0U) == DLT_SHM_RING_WRITE_INVALID);
}

void TestCorruptLengthIsSkipped()
{
    TestRing ring{4U, 16U};
    const u8 byte = 9U;
    CHECK(ring.Write("A", "B", 4U, 1ULL, &byte, 1U) == DLT_SHM_RING_WRITE_OK);
    CHECK(ring.Write("C", "D", 4U, 2ULL, &byte, 1U) == DLT_SHM_RING_WRITE_OK);
    ring.view.slots[0].payload_len = 17U; /* a misbehaving producer */

    LogRecordView record{};
    CHECK(ring.source.TryRead(record));
    CHECK(record.app_id.chars[0] == 'C'); /* the corrupt slot was skipped */
    CHECK(ring.source.corrupt() == 1U);
    CHECK(!ring.source.TryRead(record));
}

/* Packet sink keeping the last frames. */
class FrameStore
{
  public:
    static constexpr usize kMaxFrames = 8U;

    bool Send(const u8* data, usize size) noexcept
    {
        if ((count_ >= kMaxFrames) || (size > kMaxUdpPayload))
        {
            return false;
        }
        std::memcpy(frames_[count_], data, size);
        sizes_[count_] = size;
        ++count_;
        return true;
    }

    usize count() const noexcept
    {
        return count_;
    }
    const u8* frame(usize index) const noexcept
    {
        return frames_[index];
    }
    usize size(usize index) const noexcept
    {
        return sizes_[index];
    }

  private:
    u8 frames_[kMaxFrames][kMaxUdpPayload]{};
    usize sizes_[kMaxFrames]{};
    usize count_{0U};
};

void TestPipelineProducesDltFrames()
{
    TestRing ring{8U, 64U};
    const u8 payload[6] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    CHECK(ring.Write("APPL", "CTXT", 5U, 5000000000ULL, payload, sizeof(payload), 3U) == DLT_SHM_RING_WRITE_OK);
    CHECK(ring.Write("APPM", "CTXU", 2U, 5000100000ULL, payload, 2U, 1U) == DLT_SHM_RING_WRITE_OK);

    FrameStore store;
    DltWireFormatter formatter{MakeDltId("TEST")};
    Pipeline<ShmRingRecordSource, FrameStore> pipeline{ring.source, store, formatter};
    CHECK(pipeline.Pump(10U) == 2U);
    CHECK(pipeline.stats().records_received == 2U);
    CHECK(pipeline.stats().packets_sent == 2U);
    CHECK(pipeline.stats().packets_dropped == 0U);
    CHECK(store.count() == 2U);

    const u8* frame = store.frame(0U);
    CHECK(store.size(0U) == 22U + sizeof(payload));
    CHECK(frame[0] == 0x35U); /* htyp: version 1, UEH, WEID, WTMS */
    CHECK(((static_cast<usize>(frame[2]) << 8U) | frame[3]) == store.size(0U));
    CHECK(std::memcmp(&frame[4], "TEST", 4U) == 0);
    const u32 timestamp = (static_cast<u32>(frame[8]) << 24U) | (static_cast<u32>(frame[9]) << 16U) |
                          (static_cast<u32>(frame[10]) << 8U) | static_cast<u32>(frame[11]);
    CHECK(timestamp == 50000U); /* 5 s in 0.1 ms ticks */
    CHECK(frame[12] == ((5U << 4U) | 1U)); /* verbose log message, level debug */
    CHECK(frame[13] == 3U);
    CHECK(std::memcmp(&frame[14], "APPL", 4U) == 0);
    CHECK(std::memcmp(&frame[18], "CTXT", 4U) == 0);
    CHECK(std::memcmp(&frame[22], payload, sizeof(payload)) == 0);
    CHECK(store.frame(1U)[1] == static_cast<u8>(frame[1] + 1U)); /* message counter advances */
    CHECK(std::memcmp(&store.frame(1U)[14], "APPM", 4U) == 0);
}

void TestCApi()
{
    std::vector<u8> mem(dlt_ring_size_for(8U, 64U), 0U);
    dlt_transport_ops ops{nullptr, nullptr};
    CHECK(dlt_ring_step(1U, nullptr, nullptr) < 0); /* no session yet */
    CHECK(dlt_ring_start(mem.data(), mem.size(), "TEST", &ops) == -1); /* ops.send missing */
    ops.send = [](void*, const dlt_u8*, dlt_size) -> int { return 0; };
    CHECK(dlt_ring_start(mem.data(), mem.size(), "TEST", &ops) == -1); /* not formatted */
    CHECK(dlt_ring_format(mem.data(), mem.size(), 8U, 64U) == 0);
    CHECK(dlt_ring_start(mem.data(), mem.size(), "TEST", &ops) == 0);

    dlt_shm_ring view{};
    CHECK(dlt_shm_ring_attach(&view, mem.data(), mem.size()) == 0);
    const u8 payload[2] = {1U, 2U};
    CHECK(dlt_shm_ring_write(&view, 4U, "APPL", "CTXT", 1U, 1000ULL, payload, 2U) == DLT_SHM_RING_WRITE_OK);

    dlt_stats stats{};
    dlt_ring_info info{};
    CHECK(dlt_ring_step(8U, &stats, &info) == 1);
    CHECK(stats.records_received == 1U && stats.packets_sent == 1U);
    CHECK(info.slot_count == 8U && info.slot_payload_cap == 64U && info.drops_full == 0U);
    CHECK(dlt_ring_step(8U, &stats, &info) == 0); /* empty ring */

    /* With batching several messages share a datagram: packets_sent counts messages, datagrams_sent datagrams. */
    dlt_ring_set_batching(1472U);
    for (u32 i = 0U; i < 3U; ++i)
    {
        CHECK(dlt_shm_ring_write(&view, 4U, "APPL", "CTXT", 1U, 2000ULL + i, payload, 2U) == DLT_SHM_RING_WRITE_OK);
    }
    CHECK(dlt_ring_step(8U, &stats, &info) == 3);
    CHECK(stats.packets_sent == 4U && stats.datagrams_sent == 2U);
    dlt_ring_set_batching(0U);
}

/* Several producers (stand-ins for processes) against one consumer. */
void TestMultiProducerStress()
{
    constexpr u32 kProducers = 4U;
    constexpr u32 kPerProducer = 20000U;

    TestRing ring{64U, 16U};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;

    for (u32 id = 0U; id < kProducers; ++id)
    {
        producers.emplace_back(
            [&ring, &go, id]()
            {
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                for (u32 sequence = 0U; sequence < kPerProducer; ++sequence)
                {
                    u8 payload[8];
                    std::memcpy(&payload[0], &id, 4U);
                    std::memcpy(&payload[4], &sequence, 4U);
                    while (ring.Write("STR", "ESS", 4U, sequence, payload, sizeof(payload)) != DLT_SHM_RING_WRITE_OK)
                    {
                        std::this_thread::yield(); /* full: retry, so no record may be lost */
                    }
                }
            });
    }

    u32 next_sequence[kProducers] = {};
    u32 received = 0U;
    bool order_ok = true;
    bool content_ok = true;
    LogRecordView record{};
    go.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    while ((received < (kProducers * kPerProducer)) && (std::chrono::steady_clock::now() < deadline))
    {
        if (!ring.source.TryRead(record))
        {
            std::this_thread::yield();
            continue;
        }
        u32 id = 0U;
        u32 sequence = 0U;
        content_ok = content_ok && (record.payload_size == 8U);
        std::memcpy(&id, &record.payload[0], 4U);
        std::memcpy(&sequence, &record.payload[4], 4U);
        if (id >= kProducers)
        {
            content_ok = false;
            continue;
        }
        order_ok = order_ok && (sequence == next_sequence[id]);
        next_sequence[id] = sequence + 1U;
        ++received;
    }
    for (std::thread& producer : producers)
    {
        producer.join();
    }

    CHECK(received == kProducers * kPerProducer);
    CHECK(order_ok);
    CHECK(content_ok);
    CHECK(!ring.source.TryRead(record));
    CHECK(ring.source.corrupt() == 0U);
    std::printf("stress: %u records from %u producers, ring-full retries: %u\n", received, kProducers,
                ring.source.drops_full());
}

}  // namespace

int main()
{
    TestGeometry();
    TestInitAndAttach();
    TestRoundTrip();
    TestOrderAcrossWraparound();
    TestFullRingDropsAndCounts();
    TestPayloadStaysValidUntilNextRead();
    TestOversizeAndInvalid();
    TestCorruptLengthIsSkipped();
    TestPipelineProducesDltFrames();
    TestCApi();
    TestMultiProducerStress();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
