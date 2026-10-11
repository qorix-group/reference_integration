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

#ifndef DLT_QNX_NATIVE_UDP_TRANSPORT_H
#define DLT_QNX_NATIVE_UDP_TRANSPORT_H

#include "dlt/dlt_c_api.h"

/**
 * @file qnx_native_udp_transport.h
 * @brief In-stack UDP transmit path of the io-sock module (kernel socket interface).
 *
 * One transport instance per module; all functions must be called from the module's worker thread.
 */

/**
 * @brief Creates the kernel UDP socket and records the destination.
 *
 * @param dst_ip_host_order Destination IPv4 address in host byte order.
 * @param dst_port          Destination UDP port in host byte order.
 * @param src_port          Local port to bind to (host byte order), or 0 for no explicit bind.
 * @return 0 on success, otherwise an errno-style code (the transport is then not open).
 */
int dlt_udp_open(dlt_u32 dst_ip_host_order, dlt_u16 dst_port, dlt_u16 src_port);

/**
 * @brief Sends one datagram. Signature matches dlt_transport_ops::send.
 *
 * The payload is copied once into a freshly allocated packet mbuf; ownership of that mbuf passes to
 * the stack. The call never sleeps for memory.
 *
 * @param ctx  Unused.
 * @param data Datagram payload (a complete DLT message).
 * @param size Payload size in bytes, 1..MCLBYTES.
 * @return 0 on success, otherwise an errno-style code (ENOTCONN, EINVAL, EMSGSIZE, ENOBUFS or the
 *         error reported by the stack).
 */
int dlt_udp_send(void *ctx, const dlt_u8 *data, dlt_size size);

/**
 * @brief Returns the errno-style code of the most recent failed send, or 0 if no send failed yet.
 */
int dlt_udp_last_error(void);

/**
 * @brief Closes the kernel socket. Safe to call when the transport is not open.
 */
void dlt_udp_close(void);

#endif /* DLT_QNX_NATIVE_UDP_TRANSPORT_H */
