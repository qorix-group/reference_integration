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

#ifndef DLT_SHM_RING_H
#define DLT_SHM_RING_H

/**
 * @file shm_ring.h
 * @brief Shared-memory ring that carries log records from producers to the io-sock module.
 *
 * Valid C99 and C++17, no system header: the module side (freestanding C++ core) and the producer
 * side (a plain C program or library) include the very same file. Fixed-width types come from the
 * compiler's predefined macros.
 *
 * Layout of the shared object (all fields in the byte order of the machine, producers and consumer
 * run on the same machine):
 *
 *   offset 0                  control block, 3 cache lines (dlt_shm_ring_header)
 *   offset meta_offset        slot_count metadata slots of 32 bytes (dlt_shm_ring_slot)
 *   offset payload_offset     slot_count payload areas of slot_payload_cap bytes each
 *
 * This mirrors the three regions (control block, metadata, payload) that DR-002 draws for the
 * GTL shared-memory path, but it is a separate and much simpler format than the per-process rings
 * of mw::log / DataRouter.
 *
 * Concurrency: many producers, one consumer. The ring is a bounded queue with one sequence number
 * per slot (Dmitry Vyukov's bounded MPMC scheme, used here with a single consumer). Producers never
 * block: if the ring is full the record is dropped and counted. The consumer never blocks either.
 * A producer that dies between claiming and committing a slot stalls the consumer at that slot;
 * this is a known limit of the PoC (see README.md).
 *
 * The consumer reads a record in place: dlt_shm_ring_peek() does not advance, the slot stays owned
 * by the consumer until dlt_shm_ring_release().
 */

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__UINT8_TYPE__) || !defined(__UINT16_TYPE__) || !defined(__UINT32_TYPE__) || \
    !defined(__UINT64_TYPE__) || !defined(__INT32_TYPE__) || !defined(__SIZE_TYPE__)
#error "GCC or Clang predefined integer type macros are required"
#endif

typedef __UINT8_TYPE__ dlt_shm_u8;
typedef __UINT16_TYPE__ dlt_shm_u16;
typedef __UINT32_TYPE__ dlt_shm_u32;
typedef __UINT64_TYPE__ dlt_shm_u64;
typedef __INT32_TYPE__ dlt_shm_i32;
typedef __SIZE_TYPE__ dlt_shm_size;

/** "SDLT" in little endian byte order; written last by the initializer. */
#define DLT_SHM_RING_MAGIC 0x544C4453U
/** Layout version; bumped on every incompatible change. */
#define DLT_SHM_RING_VERSION 1U
/** Size of the control block. */
#define DLT_SHM_RING_HEADER_SIZE 192U
/** Size of one metadata slot. */
#define DLT_SHM_RING_SLOT_SIZE 32U
/** Smallest and largest number of slots (power of two). */
#define DLT_SHM_RING_MIN_SLOTS 2U
#define DLT_SHM_RING_MAX_SLOTS 65536U
/** Payload capacity per slot: multiple of 8, between these limits. */
#define DLT_SHM_RING_MIN_PAYLOAD 8U
#define DLT_SHM_RING_MAX_PAYLOAD 1472U

/** Result of dlt_shm_ring_write(). */
#define DLT_SHM_RING_WRITE_OK 0
#define DLT_SHM_RING_WRITE_FULL 1     /* dropped: no free slot */
#define DLT_SHM_RING_WRITE_OVERSIZE 2 /* dropped: payload larger than the slot capacity */
#define DLT_SHM_RING_WRITE_INVALID (-1)

/**
 * @brief Control block at offset 0, one cache line per writer group to avoid false sharing.
 */
typedef struct dlt_shm_ring_header {
    /* cache line 0: written once by the initializer */
    dlt_shm_u32 magic;
    dlt_shm_u32 version;
    dlt_shm_u32 slot_count;
    dlt_shm_u32 slot_payload_cap;
    dlt_shm_u32 meta_offset;
    dlt_shm_u32 payload_offset;
    dlt_shm_u32 total_size;
    dlt_shm_u32 reserved0;
    dlt_shm_u8 pad0[32];
    /* cache line 1: producers */
    dlt_shm_u32 enqueue_pos;
    dlt_shm_u32 drops_full;
    dlt_shm_u32 drops_oversize;
    dlt_shm_u32 reserved1;
    dlt_shm_u8 pad1[48];
    /* cache line 2: consumer */
    dlt_shm_u32 dequeue_pos;
    dlt_shm_u32 reserved2;
    dlt_shm_u8 pad2[56];
} dlt_shm_ring_header;

/**
 * @brief Metadata of one record. The payload sits in the payload area at the same slot index.
 */
typedef struct dlt_shm_ring_slot {
    dlt_shm_u32 sequence;   /* slot state, see the bounded queue scheme above */
    dlt_shm_u16 payload_len;
    dlt_shm_u8 level;       /* DLT log level 1 (fatal) .. 6 (verbose) */
    dlt_shm_u8 num_args;    /* number of verbose arguments in the payload */
    char app_id[4];         /* DLT application id, NUL padded */
    char ctx_id[4];         /* DLT context id, NUL padded */
    dlt_shm_u64 timestamp_ns; /* producer's CLOCK_MONOTONIC time in nanoseconds */
    dlt_shm_u8 reserved[8];
} dlt_shm_ring_slot;

/* Compile-time layout checks that work in C99 and C++. */
typedef char dlt_shm_ring_assert_header_size[(sizeof(dlt_shm_ring_header) == DLT_SHM_RING_HEADER_SIZE) ? 1 : -1];
typedef char dlt_shm_ring_assert_slot_size[(sizeof(dlt_shm_ring_slot) == DLT_SHM_RING_SLOT_SIZE) ? 1 : -1];

/**
 * @brief A process-local view of a mapped ring. Plain pointers into the mapping.
 */
typedef struct dlt_shm_ring {
    dlt_shm_ring_header *header;
    dlt_shm_ring_slot *slots;
    dlt_shm_u8 *payload;
    dlt_shm_u32 mask;
    dlt_shm_u32 slot_payload_cap;
} dlt_shm_ring;

/**
 * @brief Number of bytes a ring with the given geometry occupies.
 *
 * @param slot_count       Power of two in [DLT_SHM_RING_MIN_SLOTS, DLT_SHM_RING_MAX_SLOTS].
 * @param slot_payload_cap Multiple of 8 in [DLT_SHM_RING_MIN_PAYLOAD, DLT_SHM_RING_MAX_PAYLOAD].
 * @return Size in bytes, or 0 if the geometry is invalid.
 */
static inline dlt_shm_u32 dlt_shm_ring_required_size(dlt_shm_u32 slot_count, dlt_shm_u32 slot_payload_cap)
{
    if ((slot_count < DLT_SHM_RING_MIN_SLOTS) || (slot_count > DLT_SHM_RING_MAX_SLOTS) ||
        ((slot_count & (slot_count - 1U)) != 0U) || (slot_payload_cap < DLT_SHM_RING_MIN_PAYLOAD) ||
        (slot_payload_cap > DLT_SHM_RING_MAX_PAYLOAD) || ((slot_payload_cap % 8U) != 0U)) {
        return 0U;
    }
    return DLT_SHM_RING_HEADER_SIZE + (slot_count * DLT_SHM_RING_SLOT_SIZE) + (slot_count * slot_payload_cap);
}

/**
 * @brief Formats a memory region as an empty ring and publishes it.
 *
 * Meant to be called once by the owner (the module) on freshly created, zero-filled shared memory
 * before any producer attaches. The magic number is stored last with release semantics, so a
 * producer that sees it also sees the rest of the control block.
 *
 * @param mem              Region start, at least 8 byte aligned (a mapping is page aligned).
 * @param size             Region size in bytes.
 * @param slot_count       Number of slots, power of two.
 * @param slot_payload_cap Payload capacity per slot, multiple of 8.
 * @return 0 on success, -1 if the geometry is invalid or the region is too small.
 */
static inline int dlt_shm_ring_init(void *mem, dlt_shm_size size, dlt_shm_u32 slot_count,
    dlt_shm_u32 slot_payload_cap)
{
    dlt_shm_u8 *bytes = (dlt_shm_u8 *)mem;
    dlt_shm_ring_header *header = (dlt_shm_ring_header *)mem;
    dlt_shm_ring_slot *slots;
    const dlt_shm_u32 total = dlt_shm_ring_required_size(slot_count, slot_payload_cap);
    dlt_shm_u32 i;

    if ((mem == 0) || (total == 0U) || (size < total)) {
        return -1;
    }

    for (i = 0U; i < DLT_SHM_RING_HEADER_SIZE; ++i) {
        bytes[i] = 0U;
    }
    slots = (dlt_shm_ring_slot *)(void *)(bytes + DLT_SHM_RING_HEADER_SIZE);
    for (i = 0U; i < slot_count; ++i) {
        dlt_shm_u32 byte;
        dlt_shm_u8 *raw = (dlt_shm_u8 *)&slots[i];
        for (byte = 0U; byte < DLT_SHM_RING_SLOT_SIZE; ++byte) {
            raw[byte] = 0U;
        }
        slots[i].sequence = i;
    }

    header->version = DLT_SHM_RING_VERSION;
    header->slot_count = slot_count;
    header->slot_payload_cap = slot_payload_cap;
    header->meta_offset = DLT_SHM_RING_HEADER_SIZE;
    header->payload_offset = DLT_SHM_RING_HEADER_SIZE + (slot_count * DLT_SHM_RING_SLOT_SIZE);
    header->total_size = total;
    __atomic_store_n(&header->magic, DLT_SHM_RING_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

/**
 * @brief Validates a mapped region and builds a local view of it.
 *
 * Checks magic, version, geometry and that everything fits into `size`. Safe to call on memory a
 * misbehaving peer wrote.
 *
 * @param ring Receives the view on success.
 * @param mem  Region start.
 * @param size Region size in bytes.
 * @return 0 on success, -1 if the region is not a valid ring of this version.
 */
static inline int dlt_shm_ring_attach(dlt_shm_ring *ring, void *mem, dlt_shm_size size)
{
    dlt_shm_u8 *bytes = (dlt_shm_u8 *)mem;
    dlt_shm_ring_header *header = (dlt_shm_ring_header *)mem;
    dlt_shm_u32 slot_count;
    dlt_shm_u32 cap;

    if ((ring == 0) || (mem == 0) || (size < DLT_SHM_RING_HEADER_SIZE)) {
        return -1;
    }
    if (__atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) != DLT_SHM_RING_MAGIC) {
        return -1;
    }
    slot_count = header->slot_count;
    cap = header->slot_payload_cap;
    if ((header->version != DLT_SHM_RING_VERSION) ||
        (dlt_shm_ring_required_size(slot_count, cap) == 0U) ||
        (header->total_size != dlt_shm_ring_required_size(slot_count, cap)) ||
        (header->total_size > size) || (header->meta_offset != DLT_SHM_RING_HEADER_SIZE) ||
        (header->payload_offset != (DLT_SHM_RING_HEADER_SIZE + (slot_count * DLT_SHM_RING_SLOT_SIZE)))) {
        return -1;
    }

    ring->header = header;
    ring->slots = (dlt_shm_ring_slot *)(void *)(bytes + header->meta_offset);
    ring->payload = bytes + header->payload_offset;
    ring->mask = slot_count - 1U;
    ring->slot_payload_cap = cap;
    return 0;
}

/**
 * @brief Producer side: appends one record. Lock free, never blocks, safe from many threads and
 *        processes at once.
 *
 * @param ring         Attached view.
 * @param level        DLT log level 1..6.
 * @param app_id       Four characters (NUL padded), not necessarily NUL terminated.
 * @param ctx_id       Four characters (NUL padded).
 * @param num_args     Number of verbose arguments encoded in the payload.
 * @param timestamp_ns Producer's CLOCK_MONOTONIC time in nanoseconds.
 * @param payload      Serialized verbose arguments, `length` bytes (may be NULL only if length is 0).
 * @param length       Payload size in bytes.
 * @return DLT_SHM_RING_WRITE_OK, _FULL or _OVERSIZE (the record is dropped and counted in the
 *         control block), or _INVALID for bad arguments.
 */
static inline int dlt_shm_ring_write(const dlt_shm_ring *ring, dlt_shm_u8 level, const char app_id[4],
    const char ctx_id[4], dlt_shm_u8 num_args, dlt_shm_u64 timestamp_ns, const void *payload,
    dlt_shm_u32 length)
{
    dlt_shm_ring_header *header;
    dlt_shm_u32 pos;
    dlt_shm_u32 i;

    if ((ring == 0) || (ring->header == 0) || ((payload == 0) && (length != 0U))) {
        return DLT_SHM_RING_WRITE_INVALID;
    }
    header = ring->header;

    if (length > ring->slot_payload_cap) {
        __atomic_fetch_add(&header->drops_oversize, 1U, __ATOMIC_RELAXED);
        return DLT_SHM_RING_WRITE_OVERSIZE;
    }

    pos = __atomic_load_n(&header->enqueue_pos, __ATOMIC_RELAXED);
    for (;;) {
        dlt_shm_ring_slot *slot = &ring->slots[pos & ring->mask];
        const dlt_shm_u32 sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
        const dlt_shm_i32 diff = (dlt_shm_i32)(sequence - pos);

        if (diff == 0) {
            if (__atomic_compare_exchange_n(&header->enqueue_pos, &pos, pos + 1U, 1, __ATOMIC_RELAXED,
                    __ATOMIC_RELAXED) != 0) {
                break;
            }
        } else if (diff < 0) {
            __atomic_fetch_add(&header->drops_full, 1U, __ATOMIC_RELAXED);
            return DLT_SHM_RING_WRITE_FULL;
        } else {
            pos = __atomic_load_n(&header->enqueue_pos, __ATOMIC_RELAXED);
        }
    }

    {
        dlt_shm_ring_slot *slot = &ring->slots[pos & ring->mask];
        dlt_shm_u8 *dst = ring->payload + ((dlt_shm_size)(pos & ring->mask) * ring->slot_payload_cap);
        const dlt_shm_u8 *src = (const dlt_shm_u8 *)payload;

        for (i = 0U; i < length; ++i) {
            dst[i] = src[i];
        }
        slot->payload_len = (dlt_shm_u16)length;
        slot->level = level;
        slot->num_args = num_args;
        for (i = 0U; i < 4U; ++i) {
            slot->app_id[i] = app_id[i];
            slot->ctx_id[i] = ctx_id[i];
        }
        slot->timestamp_ns = timestamp_ns;
        __atomic_store_n(&slot->sequence, pos + 1U, __ATOMIC_RELEASE);
    }
    return DLT_SHM_RING_WRITE_OK;
}

/**
 * @brief Consumer side: looks at the oldest committed record without consuming it.
 *
 * Only one consumer may use a ring. The slot returned stays owned by the consumer (producers cannot
 * overwrite it) until dlt_shm_ring_release() is called.
 *
 * @param ring    Attached view.
 * @param slot    Receives the slot metadata.
 * @param payload Receives a pointer to the slot's payload bytes (slot->payload_len valid bytes,
 *                trust it only after checking against ring->slot_payload_cap).
 * @return 1 if a record is available, 0 if the ring is empty.
 */
static inline int dlt_shm_ring_peek(const dlt_shm_ring *ring, const dlt_shm_ring_slot **slot,
    const dlt_shm_u8 **payload)
{
    dlt_shm_ring_header *header = ring->header;
    const dlt_shm_u32 pos = __atomic_load_n(&header->dequeue_pos, __ATOMIC_RELAXED);
    const dlt_shm_ring_slot *candidate = &ring->slots[pos & ring->mask];
    const dlt_shm_u32 sequence = __atomic_load_n(&candidate->sequence, __ATOMIC_ACQUIRE);

    if ((dlt_shm_i32)(sequence - (pos + 1U)) != 0) {
        return 0;
    }
    *slot = candidate;
    *payload = ring->payload + ((dlt_shm_size)(pos & ring->mask) * ring->slot_payload_cap);
    return 1;
}

/**
 * @brief Consumer side: hands the slot returned by the last successful peek back to the producers.
 */
static inline void dlt_shm_ring_release(const dlt_shm_ring *ring)
{
    dlt_shm_ring_header *header = ring->header;
    const dlt_shm_u32 pos = __atomic_load_n(&header->dequeue_pos, __ATOMIC_RELAXED);

    __atomic_store_n(&header->dequeue_pos, pos + 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->slots[pos & ring->mask].sequence, pos + ring->mask + 1U, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* DLT_SHM_RING_H */
