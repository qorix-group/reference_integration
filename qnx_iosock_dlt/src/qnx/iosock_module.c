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
 * S-CORE DLT io-sock module: builds DLT frames inside io-sock and sends them through the in-stack UDP
 * path. Two input kinds, chosen at build time (see qnx_module/common.mk):
 *   - synthetic (default): emits DLT_RECORD_COUNT generated records, one every DLT_PERIOD_MS, then exits
 *   - ring (-DDLT_INPUT_RING): creates a POSIX shared-memory ring (DLT_RING_NAME) and forwards every
 *     record producers write into it, without any socket or IPC on the producer side
 *
 * Verified: builds against SDP 8.0.0, loads with `mount -T io-sock` and sends on QNX 8.0 x86_64 (QEMU).
 * Unloading is not supported by io-sock for modules (`umount` answers "Not supported"), so MOD_UNLOAD
 * has not been exercised.
 *
 * Lifecycle and registration follow the QNX documentation sample ("Writing an io-sock Module"):
 *   [DOCS]       moduledata_t, MODULE_VERSION, DECLARE_MODULE(SI_SUB_PSEUDO), iosock_module_version,
 *                SYSUNINIT(SI_SUB_DUMMY), <qnx/qnx_modload.h>
 * The worker thread and the unload handshake follow the Qorix experiment module:
 *   [EXPERIMENT] kthread_add/kthread_exit, MOD_UNLOAD handler, pause(), hz, nanouptime(), printf()
 *                (printf output is visible with: slog2info | grep dltmod)
 *
 * MOD_LOAD never blocks: it only starts the worker thread. The synthetic worker emits DLT_RECORD_COUNT
 * records, one every DLT_PERIOD_MS, then exits. The ring worker polls the ring every DLT_POLL_MS (at
 * least one scheduler tick) and never exits on its own. MOD_UNLOAD asks the worker to stop and
 * refuses to unload (EBUSY) while it is still running.
 *
 * Plain C: the devs headers are FreeBSD kernel headers and are not assumed C++-clean. The C++ core
 * is reached only through dlt_c_api.h. No <stdint.h>: the types come from the kernel headers.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/time.h>
#include <qnx/qnx_modload.h>

#include "dlt/dlt_c_api.h"
#include "qnx_module_stats.h"
#include "qnx_native_udp_transport.h"
#ifdef DLT_INPUT_RING
#include "qnx_shm_map.h"
#endif

/* Build-time configuration, overridable with -D (see qnx_module/common.mk). */
#ifndef DLT_DST_A
#define DLT_DST_A 239 /* default: DataRouter's multicast group 239.255.42.99 */
#define DLT_DST_B 255
#define DLT_DST_C 42
#define DLT_DST_D 99
#endif
#ifndef DLT_DST_PORT
#define DLT_DST_PORT 3490 /* DLT default port, same as the DataRouter */
#endif
#ifndef DLT_SRC_PORT
#define DLT_SRC_PORT 0 /* 0: no explicit bind, the stack picks an ephemeral port */
#endif
#ifndef DLT_RECORD_COUNT
#define DLT_RECORD_COUNT 10
#endif
#ifndef DLT_PERIOD_MS
#define DLT_PERIOD_MS 100
#endif
#ifndef DLT_RING_NAME
#define DLT_RING_NAME "/score_dlt_ring" /* appears as /dev/shmem/score_dlt_ring */
#endif
#ifndef DLT_RING_SLOTS
#define DLT_RING_SLOTS 256 /* power of two */
#endif
#ifndef DLT_RING_SLOT_PAYLOAD
#define DLT_RING_SLOT_PAYLOAD 512 /* bytes per slot, multiple of 8 */
#endif
#ifndef DLT_POLL_MS
#define DLT_POLL_MS 1 /* idle wait of the ring worker, rounded up to one scheduler tick */
#endif
#define DLT_RING_BATCH 32 /* records handled per worker iteration */
#ifndef DLT_BATCH_BYTES
#define DLT_BATCH_BYTES 0 /* 0: one DLT message per datagram, else the largest datagram payload assembled */
#endif

#define DLT_DST_IP                                                                       \
	((((dlt_u32)DLT_DST_A) << 24) | (((dlt_u32)DLT_DST_B) << 16) |       \
	    (((dlt_u32)DLT_DST_C) << 8) | ((dlt_u32)DLT_DST_D))

/* Unload waits at most this many steps of UNLOAD_STEP_MS for the worker to finish. */
#define DLT_UNLOAD_WAIT_STEPS 50
#define DLT_UNLOAD_STEP_MS 100

static volatile int g_stop;
static volatile int g_worker_done = 1;

static int
ticks_for_ms(int milliseconds)
{
	int ticks = (milliseconds * hz) / 1000;

	return ((ticks > 0) ? ticks : 1);
}

static void
set_dlt_id(char id[4], const char *text)
{
	int i;

	for (i = 0; i < 4; i++) {
		id[i] = text[i];
	}
}

/*
 * Worker thread (synthetic input): opens the UDP transport, pushes the synthetic stream through the
 * portable core at a fixed pace, publishes the counters and exits.
 */
#ifndef DLT_INPUT_RING
static void
dlt_worker(void *arg __unused)
{
	dlt_config config;
	dlt_transport_ops ops;
	dlt_stats stats;
	struct timespec uptime;
	int remaining;
	int error;

	memset(&config, 0, sizeof(config));
	memset(&stats, 0, sizeof(stats));

	nanouptime(&uptime);
	config.record_count = DLT_RECORD_COUNT;
	config.first_timestamp = dlt_nanos_to_timestamp(
	    ((dlt_u64)uptime.tv_sec * 1000000000ULL) + (dlt_u64)uptime.tv_nsec);
	config.timestamp_step = DLT_PERIOD_MS * 10; /* DLT timestamps tick every 0.1 ms */
	config.log_level = 4;                           /* info */
	set_dlt_id(config.ecu_id, "IOS1");
	set_dlt_id(config.app_id, "DLTM");
	set_dlt_id(config.ctx_id, "SYN\0");

	error = dlt_udp_open(DLT_DST_IP, DLT_DST_PORT, DLT_SRC_PORT);
	if (error != 0) {
		printf("dltmod: UDP socket setup failed, error %d\n", error);
		dlt_stats_publish(DLT_STATE_FAILED, NULL, error);
		goto done;
	}

	ops.ctx = NULL;
	ops.send = dlt_udp_send;
	if (dlt_session_start(&config, &ops) != 0) {
		printf("dltmod: session start failed\n");
		dlt_stats_publish(DLT_STATE_FAILED, NULL, EINVAL);
		goto close_socket;
	}

	printf("dltmod: sending %d synthetic DLT records to %d.%d.%d.%d:%d every %d ms\n",
	    DLT_RECORD_COUNT, DLT_DST_A, DLT_DST_B, DLT_DST_C, DLT_DST_D,
	    DLT_DST_PORT, DLT_PERIOD_MS);
	dlt_stats_publish(DLT_STATE_RUNNING, NULL, 0);

	do {
		remaining = dlt_session_step(&stats);
		dlt_stats_publish(DLT_STATE_RUNNING, &stats, dlt_udp_last_error());
		if (remaining <= 0) {
			break;
		}
		pause("dltmod", ticks_for_ms(DLT_PERIOD_MS));
	} while (g_stop == 0);

	dlt_stats_publish(DLT_STATE_FINISHED, &stats, dlt_udp_last_error());
	printf("dltmod: done, sent %lu packets (%lu bytes), %lu dropped, %lu send failures\n",
	    (unsigned long)stats.packets_sent, (unsigned long)stats.bytes_sent,
	    (unsigned long)stats.packets_dropped, (unsigned long)stats.send_failures);

close_socket:
	dlt_udp_close();
done:
	g_worker_done = 1;
	kthread_exit();
}
#else /* DLT_INPUT_RING */
/*
 * Worker thread (ring input): creates and formats the shared-memory ring, opens the UDP transport and
 * forwards every record the producers write until asked to stop. Idle iterations sleep for
 * DLT_POLL_MS. The ring mapping is kept for the lifetime of the process.
 */
static void
dlt_worker(void *arg __unused)
{
	dlt_transport_ops ops;
	dlt_stats stats;
	dlt_ring_info info;
	char ecu_id[4];
	void *ring = NULL;
	dlt_size ring_size;
	int processed;
	int error;

	memset(&stats, 0, sizeof(stats));
	memset(&info, 0, sizeof(info));
	set_dlt_id(ecu_id, "IOS1");

	ring_size = dlt_ring_size_for(DLT_RING_SLOTS, DLT_RING_SLOT_PAYLOAD);
	if (ring_size == 0) {
		printf("dltmod: invalid ring geometry %d x %d\n", DLT_RING_SLOTS, DLT_RING_SLOT_PAYLOAD);
		dlt_stats_publish(DLT_STATE_FAILED, NULL, EINVAL);
		goto done;
	}

	error = dlt_shm_map_create(DLT_RING_NAME, ring_size, &ring);
	if (error != 0) {
		printf("dltmod: cannot create shared memory %s, error %d\n", DLT_RING_NAME, error);
		dlt_stats_publish(DLT_STATE_FAILED, NULL, error);
		goto done;
	}
	if (dlt_ring_format(ring, ring_size, DLT_RING_SLOTS, DLT_RING_SLOT_PAYLOAD) != 0) {
		printf("dltmod: cannot format the ring\n");
		dlt_stats_publish(DLT_STATE_FAILED, NULL, EINVAL);
		goto done;
	}

	error = dlt_udp_open(DLT_DST_IP, DLT_DST_PORT, DLT_SRC_PORT);
	if (error != 0) {
		printf("dltmod: UDP socket setup failed, error %d\n", error);
		dlt_stats_publish(DLT_STATE_FAILED, NULL, error);
		goto done;
	}

	ops.ctx = NULL;
	ops.send = dlt_udp_send;
	if (dlt_ring_start(ring, ring_size, ecu_id, &ops) != 0) {
		printf("dltmod: ring session start failed\n");
		dlt_stats_publish(DLT_STATE_FAILED, NULL, EINVAL);
		goto close_socket;
	}

	printf("dltmod: ring %s ready (%d slots of %d bytes), forwarding to %d.%d.%d.%d:%d\n",
	    DLT_RING_NAME, DLT_RING_SLOTS, DLT_RING_SLOT_PAYLOAD, DLT_DST_A, DLT_DST_B, DLT_DST_C,
	    DLT_DST_D, DLT_DST_PORT);
	dlt_stats_publish(DLT_STATE_RUNNING, NULL, 0);

	dlt_ring_set_batching(DLT_BATCH_BYTES);

	while (g_stop == 0) {
		processed = dlt_ring_step(DLT_RING_BATCH, &stats, &info);
		dlt_stats_publish_ring(&info);
		if (processed > 0) {
			dlt_stats_publish(DLT_STATE_RUNNING, &stats, dlt_udp_last_error());
		} else {
			pause("dltmod", ticks_for_ms(DLT_POLL_MS));
		}
	}

	dlt_stats_publish(DLT_STATE_FINISHED, &stats, dlt_udp_last_error());
	printf("dltmod: stopped, sent %lu packets (%lu bytes), %lu dropped, %lu send failures\n",
	    (unsigned long)stats.packets_sent, (unsigned long)stats.bytes_sent,
	    (unsigned long)stats.packets_dropped, (unsigned long)stats.send_failures);

close_socket:
	dlt_udp_close();
done:
	g_worker_done = 1;
	kthread_exit();
}
#endif /* DLT_INPUT_RING */

static int
dlt_module_event(module_t mod __unused, int what, void *arg __unused)
{
	int error;
	int waited;

	switch (what) {
	case MOD_LOAD:
		g_stop = 0;
		g_worker_done = 0;
		error = kthread_add(dlt_worker, NULL, NULL, NULL, 0, 0, "dltmod_worker");
		if (error != 0) {
			g_worker_done = 1;
			printf("dltmod: cannot start worker thread, error %d\n", error);
			return (error);
		}
		break;

	case MOD_UNLOAD:
		g_stop = 1;
		for (waited = 0; (waited < DLT_UNLOAD_WAIT_STEPS) && (g_worker_done == 0); waited++) {
			pause("dltmod_stop", ticks_for_ms(DLT_UNLOAD_STEP_MS));
		}
		if (g_worker_done == 0) {
			printf("dltmod: worker still running, refusing to unload\n");
			return (EBUSY);
		}
		/* Give the worker time to leave the module's code inside kthread_exit(). */
		pause("dltmod_stop", ticks_for_ms(DLT_UNLOAD_STEP_MS));
		break;

	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

static moduledata_t score_dlt_moduledata = {
	"module_score_dlt",
	dlt_module_event,
	NULL
};

MODULE_VERSION(score_dlt, DLT_MODULE_VERSION);
DECLARE_MODULE(score_dlt, score_dlt_moduledata, SI_SUB_PSEUDO, SI_ORDER_ANY);

struct _iosock_module_version iosock_module_version = IOSOCK_MODULE_VER_SYM_INIT;

static void
dlt_uninit(void *arg __unused)
{
}
/*
 * As in the QNX sample: with SI_SUB_DUMMY this function is never called. If code is added to it,
 * the sub-system has to be changed to SI_SUB_DRIVERS.
 */
SYSUNINIT(dlt_uninit, SI_SUB_DUMMY, SI_ORDER_ANY, dlt_uninit, NULL);
