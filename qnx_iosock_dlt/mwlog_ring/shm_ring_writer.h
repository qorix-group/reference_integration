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
#ifndef QNX_IOSOCK_DLT_MWLOG_RING_SHM_RING_WRITER_H
#define QNX_IOSOCK_DLT_MWLOG_RING_SHM_RING_WRITER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "dlt/shm_ring.h"
#include "score/mw/log/detail/logging_identifier.h"
#include "score/mw/log/log_level.h"

namespace qorix
{
namespace dlt
{

/// \brief Producer side of the DLT shared-memory ring (see include/dlt/shm_ring.h).
///
/// Attaches to the ring that the io-sock module owns on the first write and retries at most once
/// per `kRetryInterval` while the module has not created it yet. Writing never blocks: a missing,
/// full or too small ring drops the record. Safe to call from many threads.
class ShmRingWriter final
{
  public:
    /// Outcome of a Write() call.
    enum class Result : std::uint8_t
    {
        kWritten,
        kNotAttached,  ///< The ring does not exist (yet) or failed validation.
        kFull,         ///< No free slot, counted in the ring's `drops_full`.
        kOversize,     ///< Payload larger than a slot, counted in `drops_oversize`.
        kInvalid
    };

    /// \brief Attaches lazily to the POSIX shared-memory object `ring_name` (for example "/score_dlt_ring").
    explicit ShmRingWriter(std::string ring_name);

    /// \brief Uses an already attached ring view. The memory must outlive the writer (for tests).
    explicit ShmRingWriter(const dlt_shm_ring& attached_ring) noexcept;

    ~ShmRingWriter();
    ShmRingWriter(const ShmRingWriter&) = delete;
    ShmRingWriter& operator=(const ShmRingWriter&) = delete;

    /// \brief Appends one record, stamped with the caller's CLOCK_MONOTONIC time.
    Result Write(const score::mw::log::LogLevel level,
                 const score::mw::log::detail::LoggingIdentifier& app_id,
                 const score::mw::log::detail::LoggingIdentifier& ctx_id,
                 const std::uint8_t num_of_args,
                 const char* const payload,
                 const std::size_t payload_size) noexcept;

  private:
    bool EnsureAttached() noexcept;

    static constexpr std::chrono::seconds kRetryInterval{1};

    std::string ring_name_;
    dlt_shm_ring ring_{};
    void* mapping_{nullptr};
    std::size_t mapping_size_{0U};
    std::atomic<bool> attached_{false};
    std::mutex attach_mutex_;
    std::chrono::steady_clock::time_point next_attempt_{};
};

}  // namespace dlt
}  // namespace qorix

#endif  // QNX_IOSOCK_DLT_MWLOG_RING_SHM_RING_WRITER_H
