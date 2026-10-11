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
 * @file dlt_wire_formatter_test.cpp
 * @brief Host (Linux) tests for the portable DLT core.
 *
 * Self-contained on purpose (no test framework dependency): each CHECK counts, failures are
 * printed, and the process exit code is non-zero if any check failed.
 */

#include <cstdio>
#include <cstring>
#include <type_traits>

#include "dlt/dlt_c_api.h"
#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/record_source.h"
#include "dlt/stats.h"
#include "dlt/transport.h"

using namespace dlt;

namespace
{

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                          \
    do                                                                            \
    {                                                                             \
        ++g_checks;                                                               \
        if (!(condition))                                                         \
        {                                                                         \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
        }                                                                         \
    } while (false)

SyntheticConfig MakeConfig(u32 count)
{
    return SyntheticConfig{count, 100U, 10U, LogLevel::kInfo, MakeDltId("DLTM"), MakeDltId("SYN")};
}

/* Packet sink recording up to kMaxPackets datagrams. */
class MemoryTransport
{
  public:
    static constexpr usize kMaxPackets = 8U;

    bool Send(const u8* data, usize size) noexcept
    {
        if ((count_ >= kMaxPackets) || (size > kMaxUdpPayload))
        {
            return false;
        }
        std::memcpy(packets_[count_], data, size);
        sizes_[count_] = size;
        ++count_;
        return true;
    }

    usize count() const noexcept
    {
        return count_;
    }
    const u8* packet(usize index) const noexcept
    {
        return packets_[index];
    }
    usize size(usize index) const noexcept
    {
        return sizes_[index];
    }

  private:
    u8 packets_[kMaxPackets][kMaxUdpPayload]{};
    usize sizes_[kMaxPackets]{};
    usize count_{0U};
};

class FailingTransport
{
  public:
    bool Send(const u8*, usize) noexcept
    {
        return false;
    }
};

/* Emits one record whose payload is larger than a datagram can carry. */
class OversizeSource
{
  public:
    bool TryRead(LogRecordView& out) noexcept
    {
        if (done_)
        {
            return false;
        }
        done_ = true;
        out = LogRecordView{payload_, sizeof(payload_), 1U, LogLevel::kInfo, 1U, MakeDltId("A"), MakeDltId("C")};
        return true;
    }

  private:
    u8 payload_[kMaxUdpPayload]{};
    bool done_{false};
};

/* Emits records with the given payload sizes. */
class SizedSource
{
  public:
    SizedSource(const usize* sizes, usize count) noexcept : sizes_{sizes}, count_{count} {}

    bool TryRead(LogRecordView& out) noexcept
    {
        if (next_ >= count_)
        {
            return false;
        }
        out = LogRecordView{payload_, sizes_[next_], 1U, LogLevel::kInfo, 1U, MakeDltId("A"), MakeDltId("C")};
        ++next_;
        return true;
    }

  private:
    const usize* sizes_;
    usize count_;
    usize next_{0U};
    u8 payload_[kMaxUdpPayload]{};
};

constexpr u8 kGoldenFrame0[] = {
    0x35, 0x00, 0x00, 0x2F,                          // htyp, mcnt 0, len 47
    'I',  'O',  'S',  '1',                           // ECU id
    0x00, 0x00, 0x00, 0x64,                          // timestamp 100 (0.1 ms)
    0x41, 0x02,                                      // msin (info, verbose), 2 arguments
    'D',  'L',  'T',  'M',                           // application id
    'S',  'Y',  'N',  0x00,                          // context id
    0x00, 0x82, 0x00, 0x00,                          // type info: string, UTF-8
    0x0B, 0x00,                                      // string length 11 including NUL
    'i',  'o',  's',  'o',  'c',  'k',  '-',  'd',  'l', 't', 0x00,
    0x43, 0x00, 0x00, 0x00,                          // type info: uint32
    0x00, 0x00, 0x00, 0x00,                          // value 0
};
static_assert(sizeof(kGoldenFrame0) == kDltHeaderSize + kSyntheticPayloadSize, "golden frame size");

void TestGoldenFrame()
{
    SyntheticRecordSource source{MakeConfig(2U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    u8 out[64]{};
    LogRecordView record{};

    CHECK(source.TryRead(record));
    const FormatResult first = formatter.Format(record, out, sizeof(out));
    CHECK(first.status == FormatStatus::kOk);
    CHECK(first.size == sizeof(kGoldenFrame0));
    CHECK(std::memcmp(out, kGoldenFrame0, sizeof(kGoldenFrame0)) == 0);

    CHECK(source.TryRead(record));
    const FormatResult second = formatter.Format(record, out, sizeof(out));
    CHECK(second.status == FormatStatus::kOk);
    CHECK(out[1] == 0x01U);
    CHECK((out[8] == 0x00U) && (out[9] == 0x00U) && (out[10] == 0x00U) && (out[11] == 0x6EU));
    CHECK((out[second.size - 4U] == 0x01U) && (out[second.size - 1U] == 0x00U));

    CHECK(!source.TryRead(record));
}

void TestMessageCounterWraps()
{
    DltWireFormatter formatter{MakeDltId("IOS1")};
    const u8 payload[1] = {0x00U};
    const LogRecordView record{payload, 1U, 0U, LogLevel::kInfo, 0U, MakeDltId("A"), MakeDltId("C")};
    u8 out[64]{};

    for (unsigned i = 0U; i < 256U; ++i)
    {
        CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kOk);
        CHECK(out[1] == static_cast<u8>(i));
    }
    CHECK(formatter.message_counter() == 0U);
    CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kOk);
    CHECK(out[1] == 0U);
}

void TestBufferTooSmallLeavesStateUntouched()
{
    SyntheticRecordSource source{MakeConfig(1U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    LogRecordView record{};
    CHECK(source.TryRead(record));

    u8 out[64];
    std::memset(out, 0xAA, sizeof(out));
    const FormatResult result = formatter.Format(record, out, sizeof(kGoldenFrame0) - 1U);
    CHECK(result.status == FormatStatus::kBufferTooSmall);
    CHECK(result.size == 0U);
    CHECK(formatter.message_counter() == 0U);
    for (const u8 byte : out)
    {
        CHECK(byte == 0xAAU);
    }
}

void TestPayloadLimits()
{
    static u8 payload[kDltMaxPayloadSize + 1U];
    static u8 out[kDltMaxMessageSize];
    DltWireFormatter formatter{MakeDltId("IOS1")};

    LogRecordView record{payload, kDltMaxPayloadSize, 0U, LogLevel::kInfo, 0U, MakeDltId("A"), MakeDltId("C")};
    const FormatResult max_result = formatter.Format(record, out, sizeof(out));
    CHECK(max_result.status == FormatStatus::kOk);
    CHECK(max_result.size == kDltMaxMessageSize);
    CHECK((out[2] == 0xFFU) && (out[3] == 0xFFU));

    record.payload_size = kDltMaxPayloadSize + 1U;
    CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kPayloadTooLarge);

    record.payload = nullptr;
    record.payload_size = 0U;
    const FormatResult empty = formatter.Format(record, out, sizeof(out));
    CHECK(empty.status == FormatStatus::kOk);
    CHECK(empty.size == kDltHeaderSize);
    CHECK((out[2] == 0x00U) && (out[3] == static_cast<u8>(kDltHeaderSize)));
}

void TestInvalidArguments()
{
    DltWireFormatter formatter{MakeDltId("IOS1")};
    const u8 payload[1] = {0U};
    u8 out[64]{};

    LogRecordView record{payload, 1U, 0U, LogLevel::kInfo, 0U, MakeDltId("A"), MakeDltId("C")};
    CHECK(formatter.Format(record, nullptr, sizeof(out)).status == FormatStatus::kInvalidArgument);

    record.payload = nullptr;
    CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kInvalidArgument);

    record.payload = payload;
    record.log_level = static_cast<LogLevel>(0U);
    CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kInvalidArgument);
    record.log_level = static_cast<LogLevel>(7U);
    CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kInvalidArgument);

    CHECK(formatter.message_counter() == 0U);
}

void TestLevelsAndIds()
{
    DltWireFormatter formatter{MakeDltId("EC")};
    const u8 payload[1] = {0U};
    u8 out[64]{};

    for (u8 level = 1U; level <= 6U; ++level)
    {
        const LogRecordView record{payload, 1U, 0U, static_cast<LogLevel>(level), 0U, MakeDltId("AB"), MakeDltId("CDEFG")};
        CHECK(formatter.Format(record, out, sizeof(out)).status == FormatStatus::kOk);
        CHECK(out[12] == static_cast<u8>((level << 4U) | 0x01U));
        CHECK((out[4] == 'E') && (out[5] == 'C') && (out[6] == 0U) && (out[7] == 0U));
        CHECK((out[14] == 'A') && (out[15] == 'B') && (out[16] == 0U) && (out[17] == 0U));
        CHECK((out[18] == 'C') && (out[19] == 'D') && (out[20] == 'E') && (out[21] == 'F'));
    }

    const DltId from_null = MakeDltId(nullptr);
    CHECK((from_null.chars[0] == 0) && (from_null.chars[3] == 0));
}

void TestTimestampConversion()
{
    CHECK(NanosToDltTimestamp(0ULL) == 0U);
    CHECK(NanosToDltTimestamp(99999ULL) == 0U);
    CHECK(NanosToDltTimestamp(100000ULL) == 1U);
    CHECK(NanosToDltTimestamp(1000000000ULL) == 10000U);
    CHECK(NanosToDltTimestamp((0x100000000ULL * 100000ULL) + 100000ULL) == 1U);
    CHECK(dlt_nanos_to_timestamp(1000000000ULL) == 10000U);
}

void TestSyntheticSource()
{
    SyntheticRecordSource source{MakeConfig(8U)};
    LogRecordView record{};
    CHECK(source.remaining() == 8U);

    for (u32 i = 0U; i < 8U; ++i)
    {
        CHECK(source.TryRead(record));
        CHECK(record.payload_size == kSyntheticPayloadSize);
        CHECK(record.num_of_args == kSyntheticNumArgs);
        CHECK(record.timestamp == 100U + (10U * i));
        CHECK(record.payload[record.payload_size - 4U] == static_cast<u8>(i));
    }
    CHECK(source.produced() == 8U);
    CHECK(source.remaining() == 0U);
    CHECK(!source.TryRead(record));
}

void TestPipelineCountsAndOrder()
{
    SyntheticRecordSource source{MakeConfig(3U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    MemoryTransport transport;
    Pipeline<SyntheticRecordSource, MemoryTransport> pipeline{source, transport, formatter};

    CHECK(pipeline.Pump(2U) == 2U);
    CHECK(pipeline.stats().records_received == 2U);
    CHECK(pipeline.stats().packets_sent == 2U);
    CHECK(pipeline.stats().bytes_sent == 2U * sizeof(kGoldenFrame0));

    CHECK(pipeline.Pump(10U) == 1U);
    CHECK(pipeline.Pump(10U) == 0U);
    CHECK(pipeline.stats().records_received == 3U);
    CHECK(pipeline.stats().packets_sent == 3U);
    CHECK(pipeline.stats().datagrams_sent == 3U);
    CHECK(pipeline.stats().packets_dropped == 0U);
    CHECK(pipeline.stats().send_failures == 0U);

    CHECK(transport.count() == 3U);
    for (usize i = 0U; i < 3U; ++i)
    {
        CHECK(transport.size(i) == sizeof(kGoldenFrame0));
        CHECK(transport.packet(i)[1] == static_cast<u8>(i));
    }
    CHECK(std::memcmp(transport.packet(0U), kGoldenFrame0, sizeof(kGoldenFrame0)) == 0);

    pipeline.ResetStats();
    CHECK(pipeline.stats().records_received == 0U);
}

void TestPipelineFailureAccounting()
{
    {
        SyntheticRecordSource source{MakeConfig(3U)};
        DltWireFormatter formatter{MakeDltId("IOS1")};
        FailingTransport transport;
        Pipeline<SyntheticRecordSource, FailingTransport> pipeline{source, transport, formatter};
        CHECK(pipeline.Pump(10U) == 3U);
        CHECK(pipeline.stats().send_failures == 3U);
        CHECK(pipeline.stats().packets_sent == 0U);
        CHECK(pipeline.stats().bytes_sent == 0U);
        CHECK(pipeline.stats().packets_dropped == 0U);
    }
    {
        OversizeSource source;
        DltWireFormatter formatter{MakeDltId("IOS1")};
        MemoryTransport transport;
        Pipeline<OversizeSource, MemoryTransport> pipeline{source, transport, formatter};
        CHECK(pipeline.Pump(10U) == 1U);
        CHECK(pipeline.stats().records_received == 1U);
        CHECK(pipeline.stats().packets_dropped == 1U);
        CHECK(pipeline.stats().packets_sent == 0U);
        CHECK(pipeline.stats().send_failures == 0U);
        CHECK(transport.count() == 0U);
    }
}

/* ---- Batching: several DLT messages per datagram ---- */

void TestPipelineBatchesMessages()
{
    constexpr usize kFrame = sizeof(kGoldenFrame0);
    SyntheticRecordSource source{MakeConfig(10U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    MemoryTransport transport;
    Pipeline<SyntheticRecordSource, MemoryTransport> pipeline{source, transport, formatter, kMaxUdpPayload};

    CHECK(pipeline.Pump(10U) == 10U);
    CHECK(transport.count() == 1U);
    CHECK(transport.size(0U) == 10U * kFrame);
    CHECK(pipeline.stats().packets_sent == 10U);
    CHECK(pipeline.stats().datagrams_sent == 1U);
    CHECK(pipeline.stats().bytes_sent == 10U * kFrame);
    CHECK(std::memcmp(transport.packet(0U), kGoldenFrame0, kFrame) == 0);
    for (usize i = 0U; i < 10U; ++i)
    {
        const u8* frame = transport.packet(0U) + (i * kFrame);
        CHECK(frame[1] == static_cast<u8>(i)); /* the message counter advances per message */
        CHECK(((static_cast<usize>(frame[2]) << 8U) | frame[3]) == kFrame);
    }
}

void TestPipelineBatchLimit()
{
    constexpr usize kFrame = sizeof(kGoldenFrame0);
    SyntheticRecordSource source{MakeConfig(10U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    MemoryTransport transport;
    /* 100 bytes hold two 47 byte messages, the third one starts the next datagram. */
    Pipeline<SyntheticRecordSource, MemoryTransport> pipeline{source, transport, formatter, 100U};

    CHECK(pipeline.Pump(10U) == 10U);
    CHECK(transport.count() == 5U);
    for (usize i = 0U; i < 5U; ++i)
    {
        CHECK(transport.size(i) == 2U * kFrame);
        CHECK(transport.packet(i)[1] == static_cast<u8>(2U * i));
    }
    CHECK(pipeline.stats().packets_sent == 10U);
    CHECK(pipeline.stats().datagrams_sent == 5U);
}

void TestPipelineBatchIsSentAtTheEndOfPump()
{
    constexpr usize kFrame = sizeof(kGoldenFrame0);
    SyntheticRecordSource source{MakeConfig(6U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    MemoryTransport transport;
    Pipeline<SyntheticRecordSource, MemoryTransport> pipeline{source, transport, formatter};

    pipeline.SetBatchLimit(kMaxUdpPayload); /* switched on after construction */
    CHECK(pipeline.Pump(3U) == 3U);
    CHECK(transport.count() == 1U); /* a message never waits for the next Pump() call */
    CHECK(transport.size(0U) == 3U * kFrame);
    CHECK(pipeline.Pump(3U) == 3U);
    CHECK(transport.count() == 2U);

    pipeline.SetBatchLimit(0U); /* and off again */
    CHECK(pipeline.Pump(3U) == 0U);
    CHECK(pipeline.stats().packets_sent == 6U);
    CHECK(pipeline.stats().datagrams_sent == 2U);
}

void TestPipelineBatchWithOversizeRecord()
{
    const usize sizes[] = {10U, kMaxUdpPayload, 10U};
    SizedSource source{sizes, 3U};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    MemoryTransport transport;
    Pipeline<SizedSource, MemoryTransport> pipeline{source, transport, formatter, kMaxUdpPayload};

    CHECK(pipeline.Pump(10U) == 3U);
    CHECK(pipeline.stats().records_received == 3U);
    CHECK(pipeline.stats().packets_dropped == 1U);
    CHECK(pipeline.stats().packets_sent == 2U);
    CHECK(pipeline.stats().datagrams_sent == 2U);
    CHECK(transport.count() == 2U);
    CHECK(transport.size(0U) == kDltHeaderSize + 10U);
    CHECK(transport.packet(0U)[1] == 0U);
    CHECK(transport.packet(1U)[1] == 1U); /* the dropped record did not use a message counter value */
}

void TestPipelineBatchFailureAccounting()
{
    SyntheticRecordSource source{MakeConfig(5U)};
    DltWireFormatter formatter{MakeDltId("IOS1")};
    FailingTransport transport;
    Pipeline<SyntheticRecordSource, FailingTransport> pipeline{source, transport, formatter, kMaxUdpPayload};

    CHECK(pipeline.Pump(10U) == 5U);
    CHECK(pipeline.stats().send_failures == 5U); /* every message of the refused datagram */
    CHECK(pipeline.stats().packets_sent == 0U);
    CHECK(pipeline.stats().datagrams_sent == 0U);
    CHECK(pipeline.stats().bytes_sent == 0U);
}

/* ---- C API ---- */

struct CapturedPackets
{
    u8 data[4][kMaxUdpPayload];
    dlt_size sizes[4];
    int count;
    int fail_after;  // return an error once `count` reaches this value (-1: never)
};

int CaptureSend(void* ctx, const dlt_u8* data, dlt_size size)
{
    CapturedPackets* captured = static_cast<CapturedPackets*>(ctx);
    if ((captured->fail_after >= 0) && (captured->count >= captured->fail_after))
    {
        return 5;
    }
    std::memcpy(captured->data[captured->count], data, size);
    captured->sizes[captured->count] = size;
    ++captured->count;
    return 0;
}

dlt_config MakeCConfig(dlt_u32 count)
{
    dlt_config config{};
    config.record_count = count;
    config.first_timestamp = 100U;
    config.timestamp_step = 10U;
    config.log_level = 4U;
    std::memcpy(config.ecu_id, "IOS1", 4U);
    std::memcpy(config.app_id, "DLTM", 4U);
    std::memcpy(config.ctx_id, "SYN\0", 4U);
    return config;
}

void TestCApi()
{
    CapturedPackets captured{};
    captured.fail_after = -1;
    const dlt_transport_ops ops{&captured, &CaptureSend};
    dlt_stats stats{};

    CHECK(dlt_session_step(&stats) < 0);  // no session started yet

    const dlt_config config = MakeCConfig(3U);
    CHECK(dlt_session_start(nullptr, &ops) == -1);
    CHECK(dlt_session_start(&config, nullptr) == -1);
    const dlt_transport_ops no_send{nullptr, nullptr};
    CHECK(dlt_session_start(&config, &no_send) == -1);

    CHECK(dlt_session_start(&config, &ops) == 0);
    CHECK(dlt_session_step(&stats) == 2);
    CHECK(dlt_session_step(&stats) == 1);
    CHECK(dlt_session_step(nullptr) == 0);
    CHECK(dlt_session_step(&stats) == 0);  // finished: nothing more to send
    CHECK(stats.records_received == 3U);
    CHECK(stats.packets_sent == 3U);
    CHECK(stats.bytes_sent == 3U * sizeof(kGoldenFrame0));
    CHECK(captured.count == 3);
    CHECK(std::memcmp(captured.data[0], kGoldenFrame0, sizeof(kGoldenFrame0)) == 0);
    CHECK(captured.data[2][1] == 2U);

    // Restart: counters and message counter start from zero again; transport errors are counted.
    captured.count = 0;
    captured.fail_after = 1;
    const dlt_config again = MakeCConfig(2U);
    CHECK(dlt_session_start(&again, &ops) == 0);
    CHECK(dlt_session_step(&stats) == 1);
    CHECK(dlt_session_step(&stats) == 0);
    CHECK(stats.records_received == 2U);
    CHECK(stats.packets_sent == 1U);
    CHECK(stats.send_failures == 1U);
    CHECK(captured.data[0][1] == 0U);
}

/* ---- compile-time properties: a static instance needs no constructor at load time ---- */

constexpr DltWireFormatter kStaticFormatter{MakeDltId("TST1")};
static_assert(kStaticFormatter.message_counter() == 0U, "formatter is constant-initializable");
constexpr SyntheticRecordSource kStaticSource{SyntheticConfig{3U, 0U, 1U, LogLevel::kInfo, DltId{}, DltId{}}};
static_assert(kStaticSource.remaining() == 3U, "source is constant-initializable");
static_assert(std::is_trivially_destructible<DltWireFormatter>::value, "formatter needs no destructor");
static_assert(std::is_trivially_destructible<SyntheticRecordSource>::value, "source needs no destructor");
static_assert(std::is_trivially_destructible<TransportStats>::value, "stats need no destructor");
static_assert(!std::is_polymorphic<DltWireFormatter>::value, "no vtable in the formatter");
static_assert(!std::is_polymorphic<SyntheticRecordSource>::value, "no vtable in the source");

}  // namespace

int main()
{
    TestGoldenFrame();
    TestMessageCounterWraps();
    TestBufferTooSmallLeavesStateUntouched();
    TestPayloadLimits();
    TestInvalidArguments();
    TestLevelsAndIds();
    TestTimestampConversion();
    TestSyntheticSource();
    TestPipelineCountsAndOrder();
    TestPipelineFailureAccounting();
    TestPipelineBatchesMessages();
    TestPipelineBatchLimit();
    TestPipelineBatchIsSentAtTheEndOfPump();
    TestPipelineBatchWithOversizeRecord();
    TestPipelineBatchFailureAccounting();
    TestCApi();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
