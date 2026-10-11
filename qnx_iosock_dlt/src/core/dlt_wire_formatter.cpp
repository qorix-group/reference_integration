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

#include "dlt/dlt_wire_formatter.h"

namespace dlt
{

namespace
{

constexpr u8 kMsinLevelShift = 4U;
constexpr u8 kMsinTypeShift = 1U;
constexpr u8 kLowestValidLevel = static_cast<u8>(LogLevel::kFatal);
constexpr u8 kHighestValidLevel = static_cast<u8>(LogLevel::kVerbose);

void PutBe16(u8* dst, u16 value) noexcept
{
    dst[0] = static_cast<u8>(value >> 8U);
    dst[1] = static_cast<u8>(value);
}

void PutBe32(u8* dst, u32 value) noexcept
{
    dst[0] = static_cast<u8>(value >> 24U);
    dst[1] = static_cast<u8>(value >> 16U);
    dst[2] = static_cast<u8>(value >> 8U);
    dst[3] = static_cast<u8>(value);
}

void PutId(u8* dst, const DltId& id) noexcept
{
    for (usize i = 0U; i < 4U; ++i)
    {
        dst[i] = static_cast<u8>(id.chars[i]);
    }
}

}  // namespace

FormatResult DltWireFormatter::Format(const LogRecordView& record, u8* out, usize capacity) noexcept
{
    const u8 level = static_cast<u8>(record.log_level);
    if ((out == nullptr) || ((record.payload == nullptr) && (record.payload_size != 0U)) ||
        (level < kLowestValidLevel) || (level > kHighestValidLevel))
    {
        return FormatResult{FormatStatus::kInvalidArgument, 0U};
    }
    if (record.payload_size > kDltMaxPayloadSize)
    {
        return FormatResult{FormatStatus::kPayloadTooLarge, 0U};
    }
    const usize total_size = kDltHeaderSize + record.payload_size;
    if (capacity < total_size)
    {
        return FormatResult{FormatStatus::kBufferTooSmall, 0U};
    }

    out[0] = static_cast<u8>(kDltHtypUseExtendedHeader | kDltHtypWithEcuId | kDltHtypWithTimestamp | kDltHtypVersion1);
    out[1] = message_counter_;
    PutBe16(&out[2], static_cast<u16>(total_size));

    PutId(&out[4], ecu_id_);
    PutBe32(&out[8], record.timestamp);

    out[12] = static_cast<u8>((kDltMsinTypeLog << kMsinTypeShift) | (level << kMsinLevelShift) | kDltMsinVerbose);
    out[13] = record.num_of_args;
    PutId(&out[14], record.app_id);
    PutId(&out[18], record.ctx_id);

    for (usize i = 0U; i < record.payload_size; ++i)
    {
        out[kDltHeaderSize + i] = record.payload[i];
    }

    ++message_counter_;
    return FormatResult{FormatStatus::kOk, total_size};
}

}  // namespace dlt
