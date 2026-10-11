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

#ifndef DLT_RECORD_SOURCE_H
#define DLT_RECORD_SOURCE_H

#include "dlt/log_record.h"
#include "dlt/types.h"

/**
 * @file record_source.h
 * @brief Record-source contract and the synthetic source used here.
 *
 * A record source is any type providing
 * @code
 *   bool TryRead(LogRecordView& out) noexcept;
 * @endcode
 * It returns true and fills `out` when a record is available, false when there is none. The
 * payload referenced by `out` must stay valid until the next call to TryRead().
 *
 * The contract is expressed as a template parameter of Pipeline instead of a virtual interface:
 * a pure virtual class makes the compiler reference runtime symbols (for example
 * `__cxa_pure_virtual`) that an io-sock module may not be able to resolve. A later GTL-backed
 * source only has to provide the same member function.
 */

namespace dlt
{

/// Text carried by the first argument of every synthetic record.
inline constexpr char kSyntheticText[] = "iosock-dlt";
/// Every synthetic record carries two verbose arguments: the text and a sequence number.
inline constexpr u8 kSyntheticNumArgs = 2U;
/// Payload size: string arg (4 type info + 2 length + text with NUL) plus uint32 arg (4 + 4).
inline constexpr usize kSyntheticPayloadSize = 4U + 2U + sizeof(kSyntheticText) + 4U + 4U;
/// Capacity of the payload buffer inside SyntheticRecordSource.
inline constexpr usize kSyntheticPayloadCapacity = 32U;

static_assert(kSyntheticPayloadSize <= kSyntheticPayloadCapacity, "synthetic payload does not fit its buffer");

/**
 * @brief Parameters of the synthetic record stream.
 */
struct SyntheticConfig
{
    u32 record_count;     ///< How many records the source produces before it runs dry.
    u32 first_timestamp;  ///< DLT timestamp (0.1 ms ticks) of record 0.
    u32 timestamp_step;   ///< Ticks added per record.
    LogLevel log_level;
    DltId app_id;
    DltId ctx_id;
};

/**
 * @brief Deterministic record source: a finite stream of two-argument verbose records.
 *
 * Record `n` carries the string "iosock-dlt" and the uint32 value `n` (arguments encoded as
 * mw::log encodes them: little endian type info, string length including the NUL, value).
 * Timestamps are `first_timestamp + n * timestamp_step`. Constant-initializable, no allocation.
 */
class SyntheticRecordSource
{
  public:
    /**
     * @brief Creates a source that will produce `config.record_count` records.
     */
    constexpr explicit SyntheticRecordSource(const SyntheticConfig& config) noexcept
        : config_{config}, next_{0U}, payload_{}
    {
    }

    /**
     * @brief Produces the next record, if any.
     *
     * @param out Receives a view of the record; the payload points into this object and is valid
     *            until the next call.
     * @return true if a record was produced, false once `record_count` records have been read.
     */
    bool TryRead(LogRecordView& out) noexcept;

    /**
     * @brief Returns how many records have been produced so far.
     */
    constexpr u32 produced() const noexcept
    {
        return next_;
    }

    /**
     * @brief Returns how many records are still to be produced.
     */
    constexpr u32 remaining() const noexcept
    {
        return config_.record_count - next_;
    }

  private:
    SyntheticConfig config_;
    u32 next_;
    u8 payload_[kSyntheticPayloadCapacity];
};

}  // namespace dlt

#endif  // DLT_RECORD_SOURCE_H
