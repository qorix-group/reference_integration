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

#ifndef DLT_SHM_RING_SOURCE_H
#define DLT_SHM_RING_SOURCE_H

#include "dlt/log_record.h"
#include "dlt/shm_ring.h"
#include "dlt/types.h"

namespace dlt
{

/**
 * @brief Record source that takes records out of a shared-memory ring (shm_ring.h).
 *
 * Satisfies the record-source contract of record_source.h. The payload handed out points straight
 * into the shared memory (no copy). The slot stays reserved for the consumer until the next
 * TryRead() call, which is exactly the lifetime the contract promises, so producers can never
 * overwrite a payload that is still being formatted.
 *
 * Constant-initializable, no allocation, single consumer.
 */
class ShmRingRecordSource
{
  public:
    constexpr ShmRingRecordSource() noexcept : ring_{nullptr, nullptr, nullptr, 0U, 0U}, holding_{false}, corrupt_{0U}
    {
    }

    /**
     * @brief Validates a mapped region and starts consuming from it.
     *
     * @param mem  Start of the mapping.
     * @param size Size of the mapping in bytes.
     * @return true if the region is a valid ring of the supported version.
     */
    bool Attach(void* mem, usize size) noexcept
    {
        holding_ = false;
        corrupt_ = 0U;
        return dlt_shm_ring_attach(&ring_, mem, size) == 0;
    }

    /**
     * @brief Returns whether Attach() succeeded.
     */
    constexpr bool attached() const noexcept
    {
        return ring_.header != nullptr;
    }

    /**
     * @brief Hands out the next record, if any.
     *
     * Releases the slot returned by the previous call first. A slot whose recorded payload length
     * exceeds the slot capacity (a misbehaving producer) is skipped and counted in corrupt().
     *
     * @param out Receives a view of the record; valid until the next call.
     * @return true if a record was produced, false if the ring is empty or not attached.
     */
    bool TryRead(LogRecordView& out) noexcept
    {
        if (!attached())
        {
            return false;
        }
        if (holding_)
        {
            dlt_shm_ring_release(&ring_);
            holding_ = false;
        }

        const dlt_shm_ring_slot* slot = nullptr;
        const dlt_shm_u8* payload = nullptr;
        while (dlt_shm_ring_peek(&ring_, &slot, &payload) != 0)
        {
            if (slot->payload_len > ring_.slot_payload_cap)
            {
                ++corrupt_;
                dlt_shm_ring_release(&ring_);
                continue;
            }
            holding_ = true;
            out.payload = payload;
            out.payload_size = slot->payload_len;
            out.timestamp = NanosToDltTimestamp(slot->timestamp_ns);
            out.log_level = static_cast<LogLevel>(slot->level);
            out.num_of_args = slot->num_args;
            for (usize i = 0U; i < 4U; ++i)
            {
                out.app_id.chars[i] = slot->app_id[i];
                out.ctx_id.chars[i] = slot->ctx_id[i];
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Records the producers dropped because the ring was full.
     */
    u32 drops_full() const noexcept
    {
        return attached() ? __atomic_load_n(&ring_.header->drops_full, __ATOMIC_RELAXED) : 0U;
    }

    /**
     * @brief Records the producers dropped because the payload did not fit a slot.
     */
    u32 drops_oversize() const noexcept
    {
        return attached() ? __atomic_load_n(&ring_.header->drops_oversize, __ATOMIC_RELAXED) : 0U;
    }

    /**
     * @brief Slots skipped because their length field was invalid.
     */
    constexpr u32 corrupt() const noexcept
    {
        return corrupt_;
    }

    /**
     * @brief Number of slots of the attached ring (0 if not attached).
     */
    constexpr u32 slot_count() const noexcept
    {
        return attached() ? (ring_.mask + 1U) : 0U;
    }

    /**
     * @brief Payload capacity of one slot (0 if not attached).
     */
    constexpr u32 slot_payload_cap() const noexcept
    {
        return ring_.slot_payload_cap;
    }

  private:
    dlt_shm_ring ring_;
    bool holding_;
    u32 corrupt_;
};

}  // namespace dlt

#endif  // DLT_SHM_RING_SOURCE_H
