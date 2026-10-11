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
// Host test of the mw::log ring recorder: records written through the Recorder interface must
// arrive in the shared-memory ring with the ids, level, argument count and DLT-encoded payload.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ring_recorder.h"
#include "ring_recorder_factory.h"
#include "score/mw/log/log_level.h"

namespace
{

int g_checks = 0;
int g_failures = 0;

void Check(const bool condition, const char* const what, const int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)

using score::mw::log::LogLevel;
using score::mw::log::SlotHandle;

constexpr std::uint32_t kSlots = 8U;
constexpr std::uint32_t kPayloadCap = 256U;

score::mw::log::detail::Configuration MakeConfig()
{
    score::mw::log::detail::Configuration config;
    config.SetAppId("APPL");
    config.SetDefaultLogLevel(LogLevel::kInfo);
    config.SetNumberOfSlots(4U);
    return config;
}

bool Contains(const std::uint8_t* data, const std::size_t size, const char* const text)
{
    const std::size_t length = std::strlen(text);
    for (std::size_t i = 0; (i + length) <= size; ++i)
    {
        if (std::memcmp(data + i, text, length) == 0)
        {
            return true;
        }
    }
    return false;
}

void TestRecordReachesRing()
{
    std::vector<std::uint8_t> memory(dlt_shm_ring_required_size(kSlots, kPayloadCap));
    CHECK(dlt_shm_ring_init(memory.data(), memory.size(), kSlots, kPayloadCap) == 0);
    dlt_shm_ring ring{};
    CHECK(dlt_shm_ring_attach(&ring, memory.data(), memory.size()) == 0);

    qorix::dlt::RingRecorder recorder{MakeConfig(), std::make_unique<qorix::dlt::ShmRingWriter>(ring), kPayloadCap};

    const auto slot = recorder.StartRecord("CTXT", LogLevel::kWarn);
    CHECK(slot.has_value());
    recorder.Log(*slot, std::string_view{"hello-ring"});
    recorder.Log(*slot, std::int32_t{42});
    recorder.StopRecord(*slot);

    const dlt_shm_ring_slot* record = nullptr;
    const dlt_shm_u8* payload = nullptr;
    CHECK(dlt_shm_ring_peek(&ring, &record, &payload) == 1);
    if (record == nullptr)
    {
        return;
    }
    CHECK(record->level == static_cast<std::uint8_t>(LogLevel::kWarn));
    CHECK(std::memcmp(record->app_id, "APPL", 4) == 0);
    CHECK(std::memcmp(record->ctx_id, "CTXT", 4) == 0);
    CHECK(record->num_args == 2U);
    CHECK(record->timestamp_ns > 0U);
    CHECK(record->payload_len > 0U);
    CHECK(Contains(payload, record->payload_len, "hello-ring"));
    dlt_shm_ring_release(&ring);

    CHECK(dlt_shm_ring_peek(&ring, &record, &payload) == 0);
}

void TestDisabledLevelIsNotRecorded()
{
    std::vector<std::uint8_t> memory(dlt_shm_ring_required_size(kSlots, kPayloadCap));
    dlt_shm_ring_init(memory.data(), memory.size(), kSlots, kPayloadCap);
    dlt_shm_ring ring{};
    dlt_shm_ring_attach(&ring, memory.data(), memory.size());

    qorix::dlt::RingRecorder recorder{MakeConfig(), std::make_unique<qorix::dlt::ShmRingWriter>(ring), kPayloadCap};

    CHECK(!recorder.IsLogEnabled(LogLevel::kDebug, "CTXT"));
    CHECK(!recorder.StartRecord("CTXT", LogLevel::kDebug).has_value());

    const dlt_shm_ring_slot* record = nullptr;
    const dlt_shm_u8* payload = nullptr;
    CHECK(dlt_shm_ring_peek(&ring, &record, &payload) == 0);
}

void TestSlotsAreReusedAndRingFullDrops()
{
    constexpr std::uint32_t kTinySlots = 2U;
    std::vector<std::uint8_t> memory(dlt_shm_ring_required_size(kTinySlots, kPayloadCap));
    dlt_shm_ring_init(memory.data(), memory.size(), kTinySlots, kPayloadCap);
    dlt_shm_ring ring{};
    dlt_shm_ring_attach(&ring, memory.data(), memory.size());

    qorix::dlt::RingRecorder recorder{MakeConfig(), std::make_unique<qorix::dlt::ShmRingWriter>(ring), kPayloadCap};

    // More records than recorder slots and ring slots: nothing may block or crash, extras are dropped.
    for (int i = 0; i < 20; ++i)
    {
        const auto slot = recorder.StartRecord("CTXT", LogLevel::kInfo);
        CHECK(slot.has_value());
        recorder.Log(*slot, std::int32_t{i});
        recorder.StopRecord(*slot);
    }
    CHECK(ring.header->enqueue_pos == kTinySlots);
    CHECK(ring.header->drops_full == (20U - kTinySlots));
}

void TestOversizeIsDroppedAndCounted()
{
    std::vector<std::uint8_t> memory(dlt_shm_ring_required_size(kSlots, 8U));
    dlt_shm_ring_init(memory.data(), memory.size(), kSlots, 8U);
    dlt_shm_ring ring{};
    dlt_shm_ring_attach(&ring, memory.data(), memory.size());

    qorix::dlt::RingRecorder recorder{MakeConfig(), std::make_unique<qorix::dlt::ShmRingWriter>(ring), kPayloadCap};

    const auto slot = recorder.StartRecord("CTXT", LogLevel::kInfo);
    CHECK(slot.has_value());
    recorder.Log(*slot, std::string_view{"this text does not fit into eight bytes"});
    recorder.StopRecord(*slot);
    CHECK(ring.header->drops_oversize == 1U);
}

void TestAttachByName()
{
    const std::string name = "/score_dlt_ring_test_" + std::to_string(::getpid());
    const std::uint32_t size = dlt_shm_ring_required_size(kSlots, kPayloadCap);

    qorix::dlt::ShmRingWriter writer{name};
    const char payload[] = "x";
    // The object does not exist yet.
    CHECK(writer.Write(LogLevel::kInfo,
                       score::mw::log::detail::LoggingIdentifier{"APPL"},
                       score::mw::log::detail::LoggingIdentifier{"CTXT"},
                       0U,
                       payload,
                       1U) == qorix::dlt::ShmRingWriter::Result::kNotAttached);

    const int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    void* const mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(dlt_shm_ring_init(mapping, size, kSlots, kPayloadCap) == 0);
    ::close(fd);

    // The writer retries only once per second, so a fresh writer is needed after the object appeared.
    qorix::dlt::ShmRingWriter second_writer{name};
    CHECK(second_writer.Write(LogLevel::kInfo,
                              score::mw::log::detail::LoggingIdentifier{"APPL"},
                              score::mw::log::detail::LoggingIdentifier{"CTXT"},
                              1U,
                              payload,
                              1U) == qorix::dlt::ShmRingWriter::Result::kWritten);

    dlt_shm_ring ring{};
    CHECK(dlt_shm_ring_attach(&ring, mapping, size) == 0);
    const dlt_shm_ring_slot* record = nullptr;
    const dlt_shm_u8* data = nullptr;
    CHECK(dlt_shm_ring_peek(&ring, &record, &data) == 1);

    ::munmap(mapping, size);
    ::shm_unlink(name.c_str());
}

void TestFactoryRefusesRemoteCombination()
{
    qorix::dlt::RingRecorderFactory factory;

    auto config = MakeConfig();
    config.SetLogMode({score::mw::LogMode::kCustom});
    CHECK(factory.CreateLogRecorder(config, nullptr) != nullptr);

    config.SetLogMode({score::mw::LogMode::kCustom, score::mw::LogMode::kRemote});
    CHECK(factory.CreateLogRecorder(config, nullptr) == nullptr);
}

void TestRingNameFromEnvironment()
{
    using qorix::dlt::kRingNameEnvironmentVariable;
    using qorix::dlt::ResolveRingName;

    ::unsetenv(kRingNameEnvironmentVariable);
    CHECK(ResolveRingName() == qorix::dlt::kDefaultRingName);

    ::setenv(kRingNameEnvironmentVariable, "/score_dlt_ring_fwd", 1);
    CHECK(ResolveRingName() == "/score_dlt_ring_fwd");

    // Not a shared-memory object name: refused instead of falling back to the module's ring.
    for (const char* const invalid : {"", "/", "no_slash", "/a/b"})
    {
        ::setenv(kRingNameEnvironmentVariable, invalid, 1);
        CHECK(ResolveRingName().empty());
    }

    qorix::dlt::RingRecorderFactory factory;
    auto config = MakeConfig();
    config.SetLogMode({score::mw::LogMode::kCustom});
    ::setenv(kRingNameEnvironmentVariable, "/a/b", 1);
    CHECK(factory.CreateLogRecorder(config, nullptr) == nullptr);
    ::setenv(kRingNameEnvironmentVariable, "/score_dlt_ring_fwd", 1);
    CHECK(factory.CreateLogRecorder(config, nullptr) != nullptr);

    ::unsetenv(kRingNameEnvironmentVariable);
}

}  // namespace

int main()
{
    TestRecordReachesRing();
    TestDisabledLevelIsNotRecorded();
    TestSlotsAreReusedAndRingFullDrops();
    TestOversizeIsDroppedAndCounted();
    TestAttachByName();
    TestFactoryRefusesRemoteCombination();
    TestRingNameFromEnvironment();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
