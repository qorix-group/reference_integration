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

#include "dlt/record_source.h"

namespace dlt
{

namespace
{

/// DLT type info: string (0x200) with UTF-8 coding (0x8000).
constexpr u32 kTypeInfoStringUtf8 = 0x00008200U;
/// DLT type info: unsigned integer (0x40) of 32 bit (length code 3).
constexpr u32 kTypeInfoUint32 = 0x00000043U;

void PutLe16(u8* dst, u16 value) noexcept
{
    dst[0] = static_cast<u8>(value);
    dst[1] = static_cast<u8>(value >> 8U);
}

void PutLe32(u8* dst, u32 value) noexcept
{
    dst[0] = static_cast<u8>(value);
    dst[1] = static_cast<u8>(value >> 8U);
    dst[2] = static_cast<u8>(value >> 16U);
    dst[3] = static_cast<u8>(value >> 24U);
}

}  // namespace

bool SyntheticRecordSource::TryRead(LogRecordView& out) noexcept
{
    if (next_ >= config_.record_count)
    {
        return false;
    }

    usize pos = 0U;
    PutLe32(&payload_[pos], kTypeInfoStringUtf8);
    pos += 4U;
    PutLe16(&payload_[pos], static_cast<u16>(sizeof(kSyntheticText)));
    pos += 2U;
    for (usize i = 0U; i < sizeof(kSyntheticText); ++i)
    {
        payload_[pos + i] = static_cast<u8>(kSyntheticText[i]);
    }
    pos += sizeof(kSyntheticText);
    PutLe32(&payload_[pos], kTypeInfoUint32);
    pos += 4U;
    PutLe32(&payload_[pos], next_);
    pos += 4U;

    out.payload = payload_;
    out.payload_size = pos;
    out.timestamp = config_.first_timestamp + (next_ * config_.timestamp_step);
    out.log_level = config_.log_level;
    out.num_of_args = kSyntheticNumArgs;
    out.app_id = config_.app_id;
    out.ctx_id = config_.ctx_id;

    ++next_;
    return true;
}

}  // namespace dlt
