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

#ifndef DLT_WIRE_FORMATTER_H
#define DLT_WIRE_FORMATTER_H

#include "dlt/log_record.h"
#include "dlt/types.h"

namespace dlt
{

/// Size of the DLT standard header (htyp, mcnt, len).
constexpr usize kDltStandardHeaderSize = 4U;
/// Size of the DLT extra header (ECU id, timestamp) as selected by htyp below.
constexpr usize kDltExtraHeaderSize = 8U;
/// Size of the DLT extended header (msin, noar, apid, ctid).
constexpr usize kDltExtendedHeaderSize = 10U;
/// Total header size of a verbose message in front of the payload.
constexpr usize kDltHeaderSize = kDltStandardHeaderSize + kDltExtraHeaderSize + kDltExtendedHeaderSize;
/// The DLT length field is 16 bit and covers header plus payload.
constexpr usize kDltMaxMessageSize = 0xFFFFU;
/// Largest payload that still fits into one DLT message.
constexpr usize kDltMaxPayloadSize = kDltMaxMessageSize - kDltHeaderSize;

/// htyp: an extended header follows.
constexpr u8 kDltHtypUseExtendedHeader = 0x01U;
/// htyp: the ECU id is part of the extra header.
constexpr u8 kDltHtypWithEcuId = 0x04U;
/// htyp: the timestamp is part of the extra header.
constexpr u8 kDltHtypWithTimestamp = 0x10U;
/// htyp: protocol version 1 in bits 5..7.
constexpr u8 kDltHtypVersion1 = 0x20U;
/// msin: the payload is verbose (type info per argument).
constexpr u8 kDltMsinVerbose = 0x01U;
/// msin: message type "log" (0) shifted into bits 1..3.
constexpr u8 kDltMsinTypeLog = 0x00U;

/**
 * @brief Outcome of DltWireFormatter::Format().
 */
enum class FormatStatus : u8
{
    kOk = 0U,
    kInvalidArgument,  ///< Null output, null payload with non-zero size, or log level out of range.
    kBufferTooSmall,   ///< The output buffer cannot hold header plus payload.
    kPayloadTooLarge,  ///< Header plus payload would exceed the 16 bit DLT length field.
};

/**
 * @brief Result of a formatting call: status plus the number of bytes written.
 */
struct FormatResult
{
    FormatStatus status;
    usize size;  ///< Total frame size in bytes; 0 unless status is kOk.
};

/**
 * @brief Builds verbose DLT v1 messages around an already serialized payload.
 *
 * Layout produced (all multi-byte header fields big endian, as the DataRouter does):
 * standard header (htyp 0x35, mcnt, len), extra header (ECU id, timestamp in 0.1 ms), extended
 * header (msin, noar, apid, ctid), then the payload bytes unchanged.
 *
 * The class owns only the ECU id and the 8 bit message counter, never allocates, never throws and
 * is constant-initializable, so a single static instance needs no runtime constructor. It is not
 * thread safe; give each sending context its own instance.
 */
class DltWireFormatter
{
  public:
    /**
     * @brief Creates a formatter that stamps every message with the given ECU id.
     *
     * @param ecu_id ECU identifier written into the extra header.
     */
    constexpr explicit DltWireFormatter(DltId ecu_id) noexcept : ecu_id_{ecu_id}, message_counter_{0U} {}

    /**
     * @brief Writes one DLT message for `record` into `out`.
     *
     * On success the message counter advances by one (wrapping at 256). On any failure nothing
     * is written, and the counter is left unchanged.
     *
     * @param record Record to wrap; its payload must stay valid for the duration of the call.
     * @param out Destination buffer.
     * @param capacity Size of `out` in bytes.
     * @return Status and, on success, the frame size (kDltHeaderSize + payload size).
     */
    FormatResult Format(const LogRecordView& record, u8* out, usize capacity) noexcept;

    /**
     * @brief Returns the counter value the next successfully formatted message will carry.
     */
    constexpr u8 message_counter() const noexcept
    {
        return message_counter_;
    }

  private:
    DltId ecu_id_;
    u8 message_counter_;
};

}  // namespace dlt

#endif  // DLT_WIRE_FORMATTER_H
