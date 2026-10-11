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
 * Verified: builds against SDP 8.0.0; the sysctl nodes are readable on QNX 8.0 x86_64 (QEMU).
 *
 * Mechanism and macros follow the QNX documentation samples:
 *   [DOCS] sample module:  SYSCTL_INT(_qnx_module, OID_AUTO, sample, CTLFLAG_RD, &mod_ver, 0, "Version")
 *   [DOCS] sample driver:  static SYSCTL_NODE(_hw, OID_AUTO, sam, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "...")
 *                          SYSCTL_INT(_hw_sam, OID_AUTO, debug, ...)
 * The parent node here is _qnx_module (used by the module sample), the child node pattern is the
 * driver sample's. Only int-typed read-only values are exposed.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <qnx/qnx_modload.h>

#include "qnx_module_stats.h"

static int g_abi_version = IOSOCK_VERSION_CUR;
static int g_module_version = DLT_MODULE_VERSION;
static int g_state = DLT_STATE_IDLE;
static int g_records_received;
static int g_packets_sent;
static int g_datagrams_sent;
static int g_bytes_sent;
static int g_packets_dropped;
static int g_send_failures;
static int g_last_error;
static int g_input_kind =
#ifdef DLT_INPUT_RING
    DLT_INPUT_KIND_RING;
#else
    DLT_INPUT_KIND_SYNTHETIC;
#endif
static int g_ring_slots;
static int g_ring_drops_full;
static int g_ring_drops_oversize;
static int g_ring_corrupt;
#ifndef DLT_BATCH_BYTES
#define DLT_BATCH_BYTES 0
#endif
static int g_batch_bytes = DLT_BATCH_BYTES;

static SYSCTL_NODE(_qnx_module, OID_AUTO, score_dlt, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "S-CORE DLT io-sock module");

SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, iosock_abi, CTLFLAG_RD, &g_abi_version, 0,
    "io-sock ABI version the module was built against");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, version, CTLFLAG_RD, &g_module_version, 0,
    "Module version");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, state, CTLFLAG_RD, &g_state, 0,
    "0 idle, 1 running, 2 finished, 3 failed");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, records_received, CTLFLAG_RD, &g_records_received, 0,
    "Records taken from the synthetic source");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, packets_sent, CTLFLAG_RD, &g_packets_sent, 0,
    "DLT messages accepted by the stack (equals datagrams_sent without batching)");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, datagrams_sent, CTLFLAG_RD, &g_datagrams_sent, 0,
    "Datagrams accepted by the stack");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, batch_bytes, CTLFLAG_RD, &g_batch_bytes, 0,
    "Largest datagram payload assembled from several messages, 0: one message per datagram");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, bytes_sent, CTLFLAG_RD, &g_bytes_sent, 0,
    "Bytes accepted by the stack");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, packets_dropped, CTLFLAG_RD, &g_packets_dropped, 0,
    "Records that could not be formatted into one datagram");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, send_failures, CTLFLAG_RD, &g_send_failures, 0,
    "Sends refused by the stack");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, last_error, CTLFLAG_RD, &g_last_error, 0,
    "Most recent errno-style failure code");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, input, CTLFLAG_RD, &g_input_kind, 0,
    "Input kind: 0 synthetic stream, 1 shared-memory ring");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, ring_slots, CTLFLAG_RD, &g_ring_slots, 0,
    "Slots of the shared-memory ring (ring input)");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, ring_drops_full, CTLFLAG_RD, &g_ring_drops_full, 0,
    "Records producers dropped because the ring was full");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, ring_drops_oversize, CTLFLAG_RD, &g_ring_drops_oversize, 0,
    "Records producers dropped because the payload did not fit a slot");
SYSCTL_INT(_qnx_module_score_dlt, OID_AUTO, ring_corrupt, CTLFLAG_RD, &g_ring_corrupt, 0,
    "Ring slots skipped because of an invalid length field");

static int
saturate_to_int(dlt_u64 value)
{
	return ((value > 0x7FFFFFFFULL) ? 0x7FFFFFFF : (int)value);
}

void
dlt_stats_publish(int state, const dlt_stats *stats, int last_error)
{
	if (stats != NULL) {
		g_records_received = saturate_to_int(stats->records_received);
		g_packets_sent = saturate_to_int(stats->packets_sent);
		g_datagrams_sent = saturate_to_int(stats->datagrams_sent);
		g_bytes_sent = saturate_to_int(stats->bytes_sent);
		g_packets_dropped = saturate_to_int(stats->packets_dropped);
		g_send_failures = saturate_to_int(stats->send_failures);
	}
	g_last_error = last_error;
	g_state = state;
}

void
dlt_stats_publish_ring(const dlt_ring_info *info)
{
	g_ring_slots = saturate_to_int(info->slot_count);
	g_ring_drops_full = saturate_to_int(info->drops_full);
	g_ring_drops_oversize = saturate_to_int(info->drops_oversize);
	g_ring_corrupt = saturate_to_int(info->corrupt);
}
