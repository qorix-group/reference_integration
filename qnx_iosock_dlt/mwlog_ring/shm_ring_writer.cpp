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
#include "shm_ring_writer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <utility>

namespace qorix
{
namespace dlt
{

namespace
{

std::uint64_t MonotonicNanoseconds() noexcept
{
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL) + static_cast<std::uint64_t>(now.tv_nsec);
}

}  // namespace

ShmRingWriter::ShmRingWriter(std::string ring_name) : ring_name_{std::move(ring_name)} {}

ShmRingWriter::ShmRingWriter(const dlt_shm_ring& attached_ring) noexcept : ring_{attached_ring}, attached_{true} {}

ShmRingWriter::~ShmRingWriter()
{
    if (mapping_ != nullptr)
    {
        ::munmap(mapping_, mapping_size_);
    }
}

bool ShmRingWriter::EnsureAttached() noexcept
{
    if (attached_.load(std::memory_order_acquire))
    {
        return true;
    }

    // Losing the race or being inside the retry interval simply drops this record.
    std::unique_lock<std::mutex> lock{attach_mutex_, std::try_to_lock};
    if (!lock.owns_lock() || (std::chrono::steady_clock::now() < next_attempt_))
    {
        return false;
    }
    next_attempt_ = std::chrono::steady_clock::now() + kRetryInterval;

    const int fd = ::shm_open(ring_name_.c_str(), O_RDWR, 0);
    if (fd < 0)
    {
        return false;
    }
    struct stat info{};
    void* mapping{MAP_FAILED};
    if ((::fstat(fd, &info) == 0) && (info.st_size > 0))
    {
        mapping = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    ::close(fd);
    if (mapping == MAP_FAILED)
    {
        return false;
    }

    const auto size = static_cast<std::size_t>(info.st_size);
    if (dlt_shm_ring_attach(&ring_, mapping, size) != 0)
    {
        ::munmap(mapping, size);
        return false;
    }
    mapping_ = mapping;
    mapping_size_ = size;
    attached_.store(true, std::memory_order_release);
    return true;
}

ShmRingWriter::Result ShmRingWriter::Write(const score::mw::log::LogLevel level,
                                           const score::mw::log::detail::LoggingIdentifier& app_id,
                                           const score::mw::log::detail::LoggingIdentifier& ctx_id,
                                           const std::uint8_t num_of_args,
                                           const char* const payload,
                                           const std::size_t payload_size) noexcept
{
    if (!EnsureAttached())
    {
        return Result::kNotAttached;
    }

    // LogLevel values 1..6 equal the DLT levels the ring and the module's formatter use.
    const int status = dlt_shm_ring_write(&ring_,
                                          static_cast<dlt_shm_u8>(level),
                                          app_id.data.data(),
                                          ctx_id.data.data(),
                                          num_of_args,
                                          MonotonicNanoseconds(),
                                          payload,
                                          static_cast<dlt_shm_u32>(payload_size));
    switch (status)
    {
        case DLT_SHM_RING_WRITE_OK:
            return Result::kWritten;
        case DLT_SHM_RING_WRITE_FULL:
            return Result::kFull;
        case DLT_SHM_RING_WRITE_OVERSIZE:
            return Result::kOversize;
        default:
            return Result::kInvalid;
    }
}

}  // namespace dlt
}  // namespace qorix
