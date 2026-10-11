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

#ifndef DLT_C_API_H
#define DLT_C_API_H

/**
 * @file dlt_c_api.h
 * @brief C boundary between the portable C++ core and the plain-C io-sock module.
 *
 * The header includes no system header on purpose: the kernel-side C files include the FreeBSD
 * style devs headers and must not be mixed with <stdint.h>. Fixed-width types come from the
 * compiler's predefined macros and are layout-compatible with the kernel's uint8_t etc.
 *
 * The API drives one process-wide session per input kind and is not thread safe: exactly one
 * worker context may call start/step. Two kinds exist:
 *   - synthetic: dlt_session_*, a finite generated stream (the first PoC step)
 *   - ring:      dlt_ring_*, records taken from a shared-memory ring (include/dlt/shm_ring.h)
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef __UINT8_TYPE__ dlt_u8;
typedef __UINT16_TYPE__ dlt_u16;
typedef __UINT32_TYPE__ dlt_u32;
typedef __UINT64_TYPE__ dlt_u64;
typedef __SIZE_TYPE__ dlt_size;

/**
 * @brief Packet sink supplied by the caller (the QNX native UDP transport in the module).
 *
 * `send` must transmit exactly one datagram and return 0 on success or an errno-style non-zero
 * code on failure. It must not keep `data` after returning.
 */
typedef struct dlt_transport_ops {
    void *ctx;
    int (*send)(void *ctx, const dlt_u8 *data, dlt_size size);
} dlt_transport_ops;

/**
 * @brief Parameters of one synthetic session.
 *
 * The three ids are four characters, NUL padded (not necessarily NUL terminated).
 */
typedef struct dlt_config {
    dlt_u32 record_count;
    dlt_u32 first_timestamp; /* DLT timestamp, 0.1 ms ticks */
    dlt_u32 timestamp_step;  /* ticks added per record */
    dlt_u8 log_level;        /* 1 (fatal) .. 6 (verbose) */
    char ecu_id[4];
    char app_id[4];
    char ctx_id[4];
} dlt_config;

/**
 * @brief Snapshot of the pipeline counters.
 */
typedef struct dlt_stats {
    dlt_u64 records_received;
    dlt_u64 packets_sent;     /* DLT messages accepted by the sink (equals datagrams_sent without batching) */
    dlt_u64 bytes_sent;
    dlt_u64 packets_dropped;
    dlt_u64 send_failures;    /* DLT messages in datagrams the sink refused */
    dlt_u64 datagrams_sent;   /* datagrams accepted by the sink */
} dlt_stats;

/**
 * @brief Converts monotonic nanoseconds to the DLT timestamp unit (0.1 ms, 32 bit, wrapping).
 */
dlt_u32 dlt_nanos_to_timestamp(dlt_u64 nanos);

/**
 * @brief Starts (or restarts) the synthetic session; counters and message counter are reset.
 *
 * @param config Session parameters, must not be NULL.
 * @param ops    Packet sink, must not be NULL and must have a non-NULL `send`.
 * @return 0 on success, -1 on invalid arguments.
 */
int dlt_session_start(const dlt_config *config, const dlt_transport_ops *ops);

/**
 * @brief Processes one synthetic record: format it and hand it to the transport.
 *
 * @param stats_out Optional (may be NULL); receives the counters after this step.
 * @return Number of records still to be produced (> 0: call again), 0 when finished,
 *         negative if no session was started.
 */
int dlt_session_step(dlt_stats *stats_out);

/**
 * @brief State of the shared-memory ring, as seen by the consumer.
 */
typedef struct dlt_ring_info {
    dlt_u32 slot_count;
    dlt_u32 slot_payload_cap;
    dlt_u32 drops_full;     /* records producers dropped because the ring was full */
    dlt_u32 drops_oversize; /* records producers dropped because the payload did not fit a slot */
    dlt_u32 corrupt;        /* slots skipped because of an invalid length field */
} dlt_ring_info;

/**
 * @brief Number of bytes a ring of the given geometry occupies (0 if the geometry is invalid).
 *
 * @param slot_count       Power of two, 2 .. 65536.
 * @param slot_payload_cap Multiple of 8, 8 .. 1472.
 */
dlt_size dlt_ring_size_for(dlt_u32 slot_count, dlt_u32 slot_payload_cap);

/**
 * @brief Formats a zero-filled shared-memory region as an empty ring (owner side, once).
 *
 * @return 0 on success, -1 if the geometry is invalid or the region too small.
 */
int dlt_ring_format(void *mem, dlt_size size, dlt_u32 slot_count, dlt_u32 slot_payload_cap);

/**
 * @brief Starts (or restarts) the ring session: attaches to the ring and binds the packet sink.
 *
 * Counters and the message counter are reset.
 *
 * @param mem     Mapping of a formatted ring.
 * @param size    Size of the mapping.
 * @param ecu_id  Four characters, NUL padded, written into every DLT standard header.
 * @param ops     Packet sink, must not be NULL and must have a non-NULL `send`.
 * @return 0 on success, -1 on invalid arguments or if the region is not a valid ring.
 */
int dlt_ring_start(void *mem, dlt_size size, const char ecu_id[4], const dlt_transport_ops *ops);

/**
 * @brief Packs several DLT messages of the ring session into one datagram.
 *
 * Without it (the default, and after a restart with 0) every message is its own datagram. A datagram
 * is sent when the next message does not fit, when it reaches `max_datagram_bytes`, and at the end of
 * every dlt_ring_step() call. Takes effect with the next dlt_ring_step() call; dlt_ring_start() does
 * not reset it.
 *
 * @param max_datagram_bytes Largest datagram payload to assemble, 0 disables batching, values above
 *                           1472 (one UDP datagram at MTU 1500) are limited to 1472.
 */
void dlt_ring_set_batching(dlt_u32 max_datagram_bytes);

/**
 * @brief Moves up to `max_records` records from the ring through formatter and transport.
 *
 * @param max_records Upper bound of records to process in this call.
 * @param stats_out   Optional; receives the counters after this call.
 * @param ring_out    Optional; receives the ring state after this call.
 * @return Number of records taken from the ring (0: ring empty), negative if no session was started.
 */
int dlt_ring_step(dlt_u32 max_records, dlt_stats *stats_out, dlt_ring_info *ring_out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* DLT_C_API_H */
