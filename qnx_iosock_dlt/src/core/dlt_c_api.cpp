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

#include "dlt/dlt_c_api.h"

#include "dlt/dlt_wire_formatter.h"
#include "dlt/log_record.h"
#include "dlt/pipeline.h"
#include "dlt/record_source.h"
#include "dlt/shm_ring.h"
#include "dlt/shm_ring_source.h"

namespace
{

using namespace dlt;

/**
 * @brief Adapts the C packet sink to the C++ transport contract.
 */
class CTransport
{
  public:
    constexpr CTransport() noexcept : ops_{nullptr, nullptr} {}

    /**
     * @brief Stores the packet sink used by subsequent Send() calls.
     */
    void Bind(const dlt_transport_ops& ops) noexcept
    {
        ops_ = ops;
    }

    /**
     * @brief Forwards one datagram to the C sink.
     *
     * @return true if the sink returned 0.
     */
    bool Send(const u8* data, usize size) noexcept
    {
        return (ops_.send != nullptr) && (ops_.send(ops_.ctx, data, size) == 0);
    }

  private:
    dlt_transport_ops ops_;
};

constexpr DltId kNulId{{'\0', '\0', '\0', '\0'}};
constexpr SyntheticConfig kIdleConfig{0U, 0U, 0U, LogLevel::kInfo, kNulId, kNulId};

/* Single static session: all members are constant-initialized, so no constructor runs at load. */
SyntheticRecordSource g_source{kIdleConfig};
DltWireFormatter g_formatter{kNulId};
CTransport g_transport{};
Pipeline<SyntheticRecordSource, CTransport> g_pipeline{g_source, g_transport, g_formatter};
bool g_started = false;

/* The ring session has its own source, formatter, transport and pipeline. */
ShmRingRecordSource g_ring_source{};
DltWireFormatter g_ring_formatter{kNulId};
CTransport g_ring_transport{};
Pipeline<ShmRingRecordSource, CTransport> g_ring_pipeline{g_ring_source, g_ring_transport, g_ring_formatter};
bool g_ring_started = false;

void CopyStats(const TransportStats& stats, dlt_stats* out) noexcept
{
    out->records_received = stats.records_received;
    out->packets_sent = stats.packets_sent;
    out->bytes_sent = stats.bytes_sent;
    out->packets_dropped = stats.packets_dropped;
    out->send_failures = stats.send_failures;
    out->datagrams_sent = stats.datagrams_sent;
}

DltId ToDltId(const char* four_chars) noexcept
{
    DltId id{};
    for (usize i = 0U; i < 4U; ++i)
    {
        id.chars[i] = four_chars[i];
    }
    return id;
}

}  // namespace

extern "C" dlt_u32 dlt_nanos_to_timestamp(dlt_u64 nanos)
{
    return NanosToDltTimestamp(nanos);
}

extern "C" int dlt_session_start(const dlt_config* config, const dlt_transport_ops* ops)
{
    if ((config == nullptr) || (ops == nullptr) || (ops->send == nullptr))
    {
        return -1;
    }

    g_source = SyntheticRecordSource{SyntheticConfig{config->record_count,
                                                     config->first_timestamp,
                                                     config->timestamp_step,
                                                     static_cast<LogLevel>(config->log_level),
                                                     ToDltId(config->app_id),
                                                     ToDltId(config->ctx_id)}};
    g_formatter = DltWireFormatter{ToDltId(config->ecu_id)};
    g_transport.Bind(*ops);
    g_pipeline.ResetStats();
    g_started = true;
    return 0;
}

extern "C" int dlt_session_step(dlt_stats* stats_out)
{
    if (!g_started)
    {
        return -1;
    }

    static_cast<void>(g_pipeline.Pump(1U));

    if (stats_out != nullptr)
    {
        CopyStats(g_pipeline.stats(), stats_out);
    }

    const u32 remaining = g_source.remaining();
    return (remaining > 0x7FFFFFFFU) ? 0x7FFFFFFF : static_cast<int>(remaining);
}

extern "C" dlt_size dlt_ring_size_for(dlt_u32 slot_count, dlt_u32 slot_payload_cap)
{
    return dlt_shm_ring_required_size(slot_count, slot_payload_cap);
}

extern "C" int dlt_ring_format(void* mem, dlt_size size, dlt_u32 slot_count, dlt_u32 slot_payload_cap)
{
    return dlt_shm_ring_init(mem, size, slot_count, slot_payload_cap);
}

extern "C" int dlt_ring_start(void* mem, dlt_size size, const char ecu_id[4], const dlt_transport_ops* ops)
{
    if ((mem == nullptr) || (ecu_id == nullptr) || (ops == nullptr) || (ops->send == nullptr))
    {
        return -1;
    }
    if (!g_ring_source.Attach(mem, size))
    {
        return -1;
    }

    g_ring_formatter = DltWireFormatter{ToDltId(ecu_id)};
    g_ring_transport.Bind(*ops);
    g_ring_pipeline.ResetStats();
    g_ring_started = true;
    return 0;
}

extern "C" void dlt_ring_set_batching(dlt_u32 max_datagram_bytes)
{
    g_ring_pipeline.SetBatchLimit(max_datagram_bytes);
}

extern "C" int dlt_ring_step(dlt_u32 max_records, dlt_stats* stats_out, dlt_ring_info* ring_out)
{
    if (!g_ring_started)
    {
        return -1;
    }

    const usize processed = g_ring_pipeline.Pump(max_records);

    if (stats_out != nullptr)
    {
        CopyStats(g_ring_pipeline.stats(), stats_out);
    }
    if (ring_out != nullptr)
    {
        ring_out->slot_count = g_ring_source.slot_count();
        ring_out->slot_payload_cap = g_ring_source.slot_payload_cap();
        ring_out->drops_full = g_ring_source.drops_full();
        ring_out->drops_oversize = g_ring_source.drops_oversize();
        ring_out->corrupt = g_ring_source.corrupt();
    }
    return (processed > 0x7FFFFFFFU) ? 0x7FFFFFFF : static_cast<int>(processed);
}
