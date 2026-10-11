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

#ifndef DLT_LOG_RECORD_H
#define DLT_LOG_RECORD_H

#include "dlt/types.h"

namespace dlt
{

/**
 * @brief A DLT identifier (ECU id, application id or context id): four characters, NUL padded.
 */
struct DltId
{
    char chars[4];
};

/**
 * @brief Builds a DltId from a C string.
 *
 * Shorter strings are padded with NUL, longer strings are truncated to four characters. This
 * matches how the DataRouter fills the same wire fields.
 *
 * @param text NUL terminated text, may be nullptr (yields an all-NUL id).
 * @return The resulting identifier.
 */
constexpr DltId MakeDltId(const char* text) noexcept
{
    DltId id{{'\0', '\0', '\0', '\0'}};
    if (text != nullptr)
    {
        for (usize i = 0U; (i < 4U) && (text[i] != '\0'); ++i)
        {
            id.chars[i] = text[i];
        }
    }
    return id;
}

/**
 * @brief DLT log levels carried in the message info byte of a verbose log message.
 *
 * Values follow the DLT protocol (and mw::log::LogLevel). Off (0) is not a valid level for a
 * transmitted record.
 */
enum class LogLevel : u8
{
    kFatal = 1U,
    kError = 2U,
    kWarn = 3U,
    kInfo = 4U,
    kDebug = 5U,
    kVerbose = 6U,
};

/**
 * @brief Non-owning view of one already serialized log record.
 *
 * `payload` holds the verbose DLT argument bytes exactly as mw::log produced them (type info plus
 * value per argument, little endian). The formatter adds the DLT headers around it; it never
 * interprets the payload.
 */
struct LogRecordView
{
    const u8* payload;  ///< Serialized arguments, may be nullptr only if payload_size is 0.
    usize payload_size;
    u32 timestamp;  ///< DLT timestamp in 0.1 ms ticks (u32, wraps).
    LogLevel log_level;
    u8 num_of_args;  ///< Number of arguments contained in the payload.
    DltId app_id;
    DltId ctx_id;
};

/**
 * @brief Converts a monotonic time in nanoseconds into the DLT timestamp unit (0.1 ms).
 *
 * Mirrors the DataRouter conversion (`duration<uint32_t, ratio<1, 10000>>` of the record time)
 * including the wrap-around of the 32 bit result.
 *
 * @param nanos Monotonic time in nanoseconds.
 * @return Time in 0.1 ms ticks, truncated to 32 bit.
 */
constexpr u32 NanosToDltTimestamp(u64 nanos) noexcept
{
    return static_cast<u32>(nanos / 100000ULL);
}

}  // namespace dlt

#endif  // DLT_LOG_RECORD_H
