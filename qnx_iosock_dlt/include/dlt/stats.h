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

#ifndef DLT_STATS_H
#define DLT_STATS_H

#include "dlt/types.h"

namespace dlt
{

/**
 * @brief Counters maintained by the record-to-packet pipeline.
 *
 * All counters are monotonic for the lifetime of a pipeline. A record that cannot be turned into
 * a DLT message (invalid, or larger than one UDP datagram) is counted in `packets_dropped`; a
 * message in a datagram that the transport refuses is counted in `send_failures`. The two are
 * mutually exclusive for a given record.
 *
 * Without batching every message is its own datagram, so `packets_sent` equals `datagrams_sent`.
 * With batching several messages share a datagram: `packets_sent` (the name is kept for the sysctl
 * interface) still counts DLT messages, `datagrams_sent` counts the datagrams.
 */
struct TransportStats
{
    u64 records_received{};  ///< Records taken from the record source.
    u64 packets_sent{};      ///< DLT messages accepted by the transport.
    u64 bytes_sent{};        ///< Sum of the sizes of the accepted datagrams.
    u64 packets_dropped{};   ///< Records that could not be formatted into one message.
    u64 send_failures{};     ///< DLT messages in datagrams the transport reported as not sent.
    u64 datagrams_sent{};    ///< Datagrams accepted by the transport.
};

}  // namespace dlt

#endif  // DLT_STATS_H
