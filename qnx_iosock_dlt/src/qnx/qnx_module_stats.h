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

#ifndef DLT_QNX_MODULE_STATS_H
#define DLT_QNX_MODULE_STATS_H

#include "dlt/dlt_c_api.h"

/**
 * @file qnx_module_stats.h
 * @brief Publishes module version, state and counters through sysctl.
 */

/** Version of this module as shown in sysctl. */
#define DLT_MODULE_VERSION 2

/** Value of the `input` sysctl: the module generates a finite synthetic stream. */
#define DLT_INPUT_KIND_SYNTHETIC 0
/** Value of the `input` sysctl: the module reads records from the shared-memory ring. */
#define DLT_INPUT_KIND_RING 1

/** Worker has not started yet (or the module just loaded). */
#define DLT_STATE_IDLE 0
/** Worker is emitting the synthetic stream. */
#define DLT_STATE_RUNNING 1
/** Worker emitted all records (or was asked to stop) and exited cleanly. */
#define DLT_STATE_FINISHED 2
/** Worker could not start (socket or session setup failed); see last_error. */
#define DLT_STATE_FAILED 3

/**
 * @brief Copies state and counters into the sysctl-visible variables.
 *
 * Counters are published as 32 bit ints (saturating), the same type the documented sample module
 * exposes with SYSCTL_INT. Readers see eventually consistent values; there is a single writer.
 *
 * @param state      One of the DLT_STATE_* values.
 * @param stats      Counter snapshot, may be NULL to leave the counters unchanged.
 * @param last_error errno-style code of the most recent failure, 0 if none.
 */
void dlt_stats_publish(int state, const dlt_stats *stats, int last_error);

/**
 * @brief Publishes the state of the shared-memory ring (ring input only).
 *
 * @param info Ring state from dlt_ring_step(), must not be NULL.
 */
void dlt_stats_publish_ring(const dlt_ring_info *info);

#endif /* DLT_QNX_MODULE_STATS_H */
