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

#ifndef DLT_QNX_SHM_MAP_H
#define DLT_QNX_SHM_MAP_H

#include "dlt/dlt_c_api.h"

/**
 * @file qnx_shm_map.h
 * @brief Creates and maps the POSIX shared-memory object that holds the log ring.
 *
 * io-sock is an ordinary user-space process on QNX, so the module can use the libc shared-memory
 * calls. They live in their own translation unit that includes no header at all, because the
 * module's include path puts the FreeBSD-style kernel headers (devs/) in front of the QNX ones and
 * the two sets conflict (sys/mman.h, fcntl.h, ...).
 */

/**
 * @brief Creates the shared-memory object `name`, sizes it and maps it read/write.
 *
 * An object of the same name is removed first, so the mapping always starts zero-filled. The object
 * is made world read/write (0666) so unprivileged producers can attach.
 *
 * @param name     Object name, e.g. "/score_dlt_ring" (QNX: appears as /dev/shmem/score_dlt_ring).
 * @param size     Size in bytes.
 * @param base_out Receives the start of the mapping.
 * @return 0 on success, otherwise an errno-style code.
 */
int dlt_shm_map_create(const char *name, dlt_size size, void **base_out);

#endif /* DLT_QNX_SHM_MAP_H */
