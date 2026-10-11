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
#ifndef QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_H
#define QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_H

#include <memory>

#include "score/mw/log/configuration/configuration.h"
#include "score/mw/log/detail/circular_allocator.h"
#include "score/mw/log/detail/log_record.h"
#include "score/mw/log/recorder.h"
#include "shm_ring_writer.h"

namespace qorix
{
namespace dlt
{

/// \brief mw::log recorder that writes finished records into the DLT shared-memory ring.
///
/// Mirrors the DataRouter recorder: arguments are serialised with the same `DLTFormat` into a
/// pre-allocated slot, so the payload is exactly what the io-sock module's wire formatter expects.
/// The only difference is the sink. StopRecord() copies the slot into the ring instead of handing
/// it to the DataRouter message client. No allocation happens after construction.
class RingRecorder final : public score::mw::log::Recorder
{
  public:
    RingRecorder(const score::mw::log::detail::Configuration& config,
                 std::unique_ptr<ShmRingWriter> writer,
                 std::size_t max_payload_bytes);

    score::cpp::optional<score::mw::log::SlotHandle> StartRecord(const std::string_view context_id,
                                                                 const score::mw::log::LogLevel log_level) noexcept override;
    void StopRecord(const score::mw::log::SlotHandle& slot) noexcept override;

    void Log(const score::mw::log::SlotHandle&, const bool) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::uint8_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::int8_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::uint16_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::int16_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::uint32_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::int32_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::uint64_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::int64_t) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const float) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const double) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const std::string_view) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogHex8) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogHex16) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogHex32) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogHex64) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogBin8) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogBin16) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogBin32) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogBin64) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogRawBuffer) noexcept override;
    void Log(const score::mw::log::SlotHandle&, const score::mw::log::LogSlog2Message) noexcept override;

    bool IsLogEnabled(const score::mw::log::LogLevel&, const std::string_view context) const noexcept override;

  private:
    template <typename T>
    void LogData(const score::mw::log::SlotHandle& slot, const T data) noexcept;

    score::mw::log::detail::LogRecord& Record(const score::mw::log::SlotHandle& slot) noexcept;

    score::mw::log::detail::Configuration config_;
    score::mw::log::detail::CircularAllocator<score::mw::log::detail::LogRecord> slots_;
    std::unique_ptr<ShmRingWriter> writer_;
};

}  // namespace dlt
}  // namespace qorix

#endif  // QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_H
