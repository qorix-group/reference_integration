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

/*
 * Verified: builds against SDP 8.0.0 and delivers UDP datagrams on QNX 8.0 x86_64 (QEMU).
 *
 * Every kernel call below is taken in form from one of two sources, nothing is invented:
 *   [EXPERIMENT] qorix-group/reference_integration, branch prabakaran_io_sock_module_experiment,
 *                integration/io_sock_module/test/udp_tx_throughput/kso_UDP_tx_throughput.c
 *                (socreate, sobind, sosend, soclose, m_copyback, mbuf length fields, curthread)
 *   [DOCS]       QNX SDP 8.0, "A Hardware-Independent Sample Driver"
 *                (m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR))
 * Whether these are *supported* module APIs is exactly what this code has to establish on a target.
 *
 * Plain C on purpose: the devs headers are FreeBSD kernel headers and are not assumed C++-clean.
 * No <stdint.h>: the types come from the kernel headers and the C boundary header.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/mbuf.h>
#include <netinet/in.h>

#include "qnx_native_udp_transport.h"

static struct socket *g_socket;
static struct sockaddr_in g_destination;
static int g_last_error;

int
dlt_udp_open(dlt_u32 dst_ip_host_order, dlt_u16 dst_port, dlt_u16 src_port)
{
	struct thread *td = curthread;
	struct sockaddr_in local;
	int error;

	if (g_socket != NULL) {
		return (EBUSY);
	}

	error = socreate(PF_INET, &g_socket, SOCK_DGRAM, IPPROTO_UDP, td->td_ucred, td);
	if (error != 0) {
		g_socket = NULL;
		return (error);
	}

	if (src_port != 0) {
		memset(&local, 0, sizeof(local));
		local.sin_len = sizeof(local);
		local.sin_family = AF_INET;
		local.sin_port = htons(src_port);
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		error = sobind(g_socket, (struct sockaddr *)&local, td);
		if (error != 0) {
			soclose(g_socket);
			g_socket = NULL;
			return (error);
		}
	}

	memset(&g_destination, 0, sizeof(g_destination));
	g_destination.sin_len = sizeof(g_destination);
	g_destination.sin_family = AF_INET;
	g_destination.sin_port = htons(dst_port);
	g_destination.sin_addr.s_addr = htonl(dst_ip_host_order);
	return (0);
}

static int
send_datagram(const dlt_u8 *data, dlt_size size)
{
	struct mbuf *packet;

	if (g_socket == NULL) {
		return (ENOTCONN);
	}
	if (data == NULL || size == 0) {
		return (EINVAL);
	}
	if (size > MCLBYTES) {
		return (EMSGSIZE);
	}

	packet = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (packet == NULL) {
		return (ENOBUFS);
	}

	m_copyback(packet, 0, (int)size, (const char *)data);
	packet->m_len = (int)size;
	packet->m_pkthdr.len = (int)size;

	/* sosend() consumes the mbuf chain on success and on failure. */
	return (sosend(g_socket, (struct sockaddr *)&g_destination, NULL, packet, NULL, 0, curthread));
}

int
dlt_udp_send(void *ctx __unused, const dlt_u8 *data, dlt_size size)
{
	int error = send_datagram(data, size);

	if (error != 0) {
		g_last_error = error;
	}
	return (error);
}

int
dlt_udp_last_error(void)
{
	return (g_last_error);
}

void
dlt_udp_close(void)
{
	if (g_socket != NULL) {
		soclose(g_socket);
		g_socket = NULL;
	}
}
