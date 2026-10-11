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

#ifndef DLT_TRANSPORT_H
#define DLT_TRANSPORT_H

#include "dlt/types.h"

/**
 * @file transport.h
 * @brief Transport contract and packet-size limit.
 *
 * A transport is any type providing
 * @code
 *   bool Send(const u8* data, usize size) noexcept;
 * @endcode
 * It transmits exactly one datagram carrying `data[0..size)` and returns true if the packet was
 * handed to the network stack. It must not retain `data` after returning (the transport accepts one
 * bounded copy inside the transport; no zero-copy is claimed).
 *
 * Like the record source, the contract is a template parameter of Pipeline rather than a virtual
 * interface (see record_source.h for the reason).
 */

namespace dlt
{

/**
 * @brief Largest DLT message sent as a single datagram.
 *
 * Same value as the DataRouter's kUdpMaxPayload: MTU 1500 minus IPv4 header (20) minus UDP header
 * (8). The pipeline does not batch several DLT messages per datagram and does not fragment.
 */
inline constexpr usize kMaxUdpPayload = 1472U;

}  // namespace dlt

#endif  // DLT_TRANSPORT_H
