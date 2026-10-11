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
#include "ring_recorder.h"

#include <tuple>
#include <utility>

#include "score/mw/log/detail/common/dlt_format.h"
#include "score/mw/log/detail/dlt_argument_counter.h"

namespace qorix
{
namespace dlt
{

namespace
{
using score::mw::log::SlotHandle;
using score::mw::log::SlotIndex;
using score::mw::log::detail::LogRecord;
using score::mw::log::detail::LoggingIdentifier;
}  // namespace

RingRecorder::RingRecorder(const score::mw::log::detail::Configuration& config,
                           std::unique_ptr<ShmRingWriter> writer,
                           const std::size_t max_payload_bytes)
    : Recorder{},
      config_{config},
      slots_{static_cast<std::size_t>(config.GetNumberOfSlots()), LogRecord{max_payload_bytes}},
      writer_{std::move(writer)}
{
}

LogRecord& RingRecorder::Record(const SlotHandle& slot) noexcept
{
    return slots_.GetUnderlyingBufferFor(static_cast<std::size_t>(slot.GetSlotOfSelectedRecorder()));
}

score::cpp::optional<SlotHandle> RingRecorder::StartRecord(const std::string_view context_id,
                                                           const score::mw::log::LogLevel log_level) noexcept
{
    if (!IsLogEnabled(log_level, context_id))
    {
        return {};
    }
    const auto index = slots_.AcquireSlotToWrite();
    if (!index.has_value())
    {
        return {};
    }
    const SlotHandle slot{static_cast<SlotIndex>(index.value())};

    auto& entry = Record(slot).GetLogEntry();
    entry.num_of_args = 0U;
    entry.payload.clear();
    entry.app_id = LoggingIdentifier{config_.GetAppId()};
    entry.ctx_id = LoggingIdentifier{context_id};
    entry.log_level = log_level;
    return slot;
}

void RingRecorder::StopRecord(const SlotHandle& slot) noexcept
{
    const auto& entry = Record(slot).GetLogEntry();
    // A full or missing ring drops the record; the ring counts it and the module exposes the counters.
    std::ignore = writer_->Write(
        entry.log_level, entry.app_id, entry.ctx_id, entry.num_of_args, entry.payload.data(), entry.payload.size());
    slots_.ReleaseSlot(static_cast<std::size_t>(slot.GetSlotOfSelectedRecorder()));
}

template <typename T>
void RingRecorder::LogData(const SlotHandle& slot, const T data) noexcept
{
    auto& record = Record(slot);
    score::mw::log::detail::DltArgumentCounter counter{record.GetLogEntry().num_of_args};
    std::ignore = counter.TryAddArgument(
        [data, &record]() noexcept { return score::mw::log::detail::DLTFormat::Log(record.GetVerbosePayload(), data); });
}

void RingRecorder::Log(const SlotHandle& slot, const bool data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::uint8_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::int8_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::uint16_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::int16_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::uint32_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::int32_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::uint64_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::int64_t data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const float data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const double data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const std::string_view data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogHex8 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogHex16 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogHex32 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogHex64 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogBin8 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogBin16 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogBin32 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogBin64 data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogRawBuffer data) noexcept
{
    LogData(slot, data);
}
void RingRecorder::Log(const SlotHandle& slot, const score::mw::log::LogSlog2Message data) noexcept
{
    LogData(slot, data.GetMessage());
}

bool RingRecorder::IsLogEnabled(const score::mw::log::LogLevel& log_level, const std::string_view context) const noexcept
{
    return config_.IsLogLevelEnabled(log_level, context);
}

}  // namespace dlt
}  // namespace qorix
