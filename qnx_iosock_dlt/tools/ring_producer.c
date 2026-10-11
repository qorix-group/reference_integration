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

/**
 * @file ring_producer.c
 * @brief Test producer: writes DLT log records into the shared-memory ring of the io-sock module.
 *
 * Plain C, builds for Linux (host tests) and for QNX (target), and uses the very same header the
 * module side uses (include/dlt/shm_ring.h). Every record carries the two verbose arguments the
 * synthetic source uses: a string and an incrementing uint32, so tools/dlt_udp_check.py can check
 * the stream on the receiving side.
 *
 * Usage: ring_producer <shm-name> <count> <period-ms> [text] [app-id] [ctx-id]
 *   shm-name   shared memory object, e.g. /score_dlt_ring (QNX: /dev/shmem/score_dlt_ring)
 *   count      number of records to write
 *   period-ms  pause between records (0: write as fast as possible)
 *   text       first argument of every record (default "ring-dlt")
 *
 * The program waits up to 15 s for the ring to appear and be formatted, so it may be started before
 * the owner (the module) has created it.
 *
 * Exit status: 0 if every record was accepted by the ring, 1 if records were dropped, 2 on setup errors.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dlt/shm_ring.h"

#define ATTACH_TIMEOUT_MS 15000
#define ATTACH_POLL_MS 100
#define TYPE_INFO_STRING_UTF8 0x00008200U
#define TYPE_INFO_UINT32 0x00000043U

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec pause;

    pause.tv_sec = (time_t)(milliseconds / 1000U);
    pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&pause, NULL);
}

static dlt_shm_u64 monotonic_nanos(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((dlt_shm_u64)now.tv_sec * 1000000000ULL) + (dlt_shm_u64)now.tv_nsec;
}

static void put_le16(dlt_shm_u8 *dst, dlt_shm_u16 value)
{
    dst[0] = (dlt_shm_u8)value;
    dst[1] = (dlt_shm_u8)(value >> 8);
}

static void put_le32(dlt_shm_u8 *dst, dlt_shm_u32 value)
{
    dst[0] = (dlt_shm_u8)value;
    dst[1] = (dlt_shm_u8)(value >> 8);
    dst[2] = (dlt_shm_u8)(value >> 16);
    dst[3] = (dlt_shm_u8)(value >> 24);
}

/* Maps the ring, waiting for the owner to create and format it. Returns 0 on success. */
static int map_ring(const char *name, dlt_shm_ring *ring)
{
    int waited_ms;

    for (waited_ms = 0; waited_ms <= ATTACH_TIMEOUT_MS; waited_ms += ATTACH_POLL_MS) {
        int fd = shm_open(name, O_RDWR, 0);

        if (fd >= 0) {
            struct stat info;

            if ((fstat(fd, &info) == 0) && (info.st_size >= (off_t)DLT_SHM_RING_HEADER_SIZE)) {
                void *mem = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

                if (mem != MAP_FAILED) {
                    if (dlt_shm_ring_attach(ring, mem, (dlt_shm_size)info.st_size) == 0) {
                        close(fd);
                        return 0;
                    }
                    munmap(mem, (size_t)info.st_size);
                }
            }
            close(fd);
        }
        sleep_ms(ATTACH_POLL_MS);
    }
    return -1;
}

int main(int argc, char **argv)
{
    dlt_shm_ring ring;
    const char *text = "ring-dlt";
    const char *app = "DLTM";
    const char *ctx = "RNG";
    char app_id[4] = {0, 0, 0, 0};
    char ctx_id[4] = {0, 0, 0, 0};
    unsigned long count;
    unsigned long period_ms;
    unsigned long written = 0;
    unsigned long dropped_full = 0;
    unsigned long dropped_oversize = 0;
    unsigned long i;
    size_t text_len;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <shm-name> <count> <period-ms> [text] [app-id] [ctx-id]\n", argv[0]);
        return 2;
    }
    count = strtoul(argv[2], NULL, 10);
    period_ms = strtoul(argv[3], NULL, 10);
    if (argc > 4) {
        text = argv[4];
    }
    if (argc > 5) {
        app = argv[5];
    }
    if (argc > 6) {
        ctx = argv[6];
    }
    memcpy(app_id, app, strnlen(app, 4));
    memcpy(ctx_id, ctx, strnlen(ctx, 4));

    if (map_ring(argv[1], &ring) != 0) {
        fprintf(stderr, "ring_producer: no valid ring at %s after %d ms\n", argv[1], ATTACH_TIMEOUT_MS);
        return 2;
    }
    printf("ring_producer: attached to %s (%u slots of %u bytes)\n", argv[1], (unsigned int)(ring.mask + 1U),
        (unsigned int)ring.slot_payload_cap);

    text_len = strlen(text) + 1U; /* the argument carries the terminating NUL */
    for (i = 0; i < count; ++i) {
        dlt_shm_u8 payload[DLT_SHM_RING_MAX_PAYLOAD];
        size_t pos = 0;
        int result;

        if ((4U + 2U + text_len + 4U + 4U) > sizeof(payload)) {
            fprintf(stderr, "ring_producer: text too long\n");
            return 2;
        }
        put_le32(&payload[pos], TYPE_INFO_STRING_UTF8);
        pos += 4U;
        put_le16(&payload[pos], (dlt_shm_u16)text_len);
        pos += 2U;
        memcpy(&payload[pos], text, text_len);
        pos += text_len;
        put_le32(&payload[pos], TYPE_INFO_UINT32);
        pos += 4U;
        put_le32(&payload[pos], (dlt_shm_u32)i);
        pos += 4U;

        result = dlt_shm_ring_write(&ring, 4U, app_id, ctx_id, 2U, monotonic_nanos(), payload, (dlt_shm_u32)pos);
        if (result == DLT_SHM_RING_WRITE_OK) {
            ++written;
        } else if (result == DLT_SHM_RING_WRITE_FULL) {
            ++dropped_full;
        } else {
            ++dropped_oversize;
        }
        if (period_ms > 0) {
            sleep_ms((unsigned int)period_ms);
        }
    }

    printf("ring_producer: written=%lu dropped_full=%lu dropped_oversize=%lu\n", written, dropped_full,
        dropped_oversize);
    return (written == count) ? 0 : 1;
}
