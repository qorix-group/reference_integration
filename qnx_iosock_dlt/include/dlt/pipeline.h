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

#ifndef DLT_PIPELINE_H
#define DLT_PIPELINE_H

#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/stats.h"
#include "dlt/transport.h"
#include "dlt/types.h"

namespace dlt
{

/**
 * @brief Record source -> DLT formatter -> transport, with statistics.
 *
 * @tparam Source    Type satisfying the record-source contract (record_source.h).
 * @tparam Transport Type satisfying the transport contract (transport.h).
 *
 * The pipeline holds references to its collaborators and a frame buffer of kMaxUdpPayload bytes,
 * so it never allocates and can live in static storage. Not thread safe.
 *
 * By default every DLT message is sent as its own datagram. With a batch limit several messages are
 * packed into one datagram (as the DataRouter does), which cuts the number of datagrams a burst
 * produces. A datagram is sent when the next message would not fit, when the limit is reached, and
 * at the end of every Pump() call, so a message never waits longer than one Pump() call.
 */
template <typename Source, typename Transport>
class Pipeline
{
  public:
    /**
     * @brief Wires the collaborators together. All three must outlive the pipeline.
     *
     * @param batch_limit Largest datagram payload to assemble, 0 disables batching (one message per
     *                    datagram). Values above kMaxUdpPayload are limited to it.
     */
    constexpr Pipeline(Source& source, Transport& transport, DltWireFormatter& formatter, usize batch_limit = 0U) noexcept
        : source_{source}, transport_{transport}, formatter_{formatter}, stats_{}, frame_{}, batch_limit_{ClampLimit(batch_limit)}
    {
    }

    /**
     * @brief Changes the batch limit, see the constructor. Takes effect with the next Pump() call.
     */
    void SetBatchLimit(usize batch_limit) noexcept
    {
        batch_limit_ = ClampLimit(batch_limit);
    }

    /**
     * @brief Moves up to `max_records` records through the pipeline.
     *
     * Per record: it is counted as received and formatted into the datagram being assembled. A record
     * that cannot be formatted into a datagram counts as dropped; a refused send counts every message
     * of that datagram as a send failure. Neither condition stops processing.
     *
     * @param max_records Upper bound of records to process in this call.
     * @return Number of records taken from the source.
     */
    usize Pump(usize max_records) noexcept
    {
        usize processed = 0U;
        usize used = 0U;      // bytes of the datagram being assembled
        u32 messages = 0U;    // DLT messages in it
        LogRecordView record{};
        while ((processed < max_records) && source_.TryRead(record))
        {
            ++processed;
            ++stats_.records_received;

            // The first message of a datagram may use the whole payload, later ones the room left below the limit.
            const usize capacity = (used == 0U) ? kMaxUdpPayload : (batch_limit_ - used);
            FormatResult result = formatter_.Format(record, frame_ + used, capacity);
            if ((result.status == FormatStatus::kBufferTooSmall) && (used > 0U))
            {
                Flush(used, messages);
                used = 0U;
                messages = 0U;
                result = formatter_.Format(record, frame_, kMaxUdpPayload);
            }
            if (result.status != FormatStatus::kOk)
            {
                ++stats_.packets_dropped;
                continue;
            }
            used += result.size;
            ++messages;
            if ((batch_limit_ == 0U) || (used >= batch_limit_))
            {
                Flush(used, messages);
                used = 0U;
                messages = 0U;
            }
        }
        if (used > 0U)
        {
            Flush(used, messages);
        }
        return processed;
    }

    /**
     * @brief Returns the counters accumulated since construction or the last ResetStats().
     */
    constexpr const TransportStats& stats() const noexcept
    {
        return stats_;
    }

    /**
     * @brief Sets all counters back to zero.
     */
    void ResetStats() noexcept
    {
        stats_ = TransportStats{};
    }

  private:
    static constexpr usize ClampLimit(usize limit) noexcept
    {
        return (limit > kMaxUdpPayload) ? kMaxUdpPayload : limit;
    }

    // Sends the assembled datagram and accounts for the messages in it.
    void Flush(usize size, u32 messages) noexcept
    {
        if (transport_.Send(frame_, size))
        {
            stats_.packets_sent += messages;
            stats_.bytes_sent += size;
            ++stats_.datagrams_sent;
        }
        else
        {
            stats_.send_failures += messages;
        }
    }

    Source& source_;
    Transport& transport_;
    DltWireFormatter& formatter_;
    TransportStats stats_;
    u8 frame_[kMaxUdpPayload];
    usize batch_limit_;
};

}  // namespace dlt

#endif  // DLT_PIPELINE_H
