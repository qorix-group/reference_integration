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
// Copyright (c) 2026 Qorix
// SPDX-License-Identifier: Apache-2.0
#include "ring_recorder_factory.h"

#include <algorithm>
#include <utility>

#include "ring_recorder.h"
#include "score/mw/log/detail/error.h"
#include "score/mw/log/detail/initialization_reporter.h"

namespace qorix
{
namespace dlt
{

std::unique_ptr<score::mw::log::Recorder> RingRecorderFactory::CreateConcreteLogRecorder(
    const score::mw::log::detail::Configuration& config,
    score::cpp::pmr::memory_resource*)
{
    // Both sinks would send every record to the network: refuse instead of producing duplicates.
    if (config.GetLogMode().count(score::mw::LogMode::kRemote) != 0U)
    {
        score::mw::log::detail::ReportInitializationError(
            score::mw::log::detail::Error::kRecorderFactoryUnsupportedLogMode,
            "kCustom (DLT ring recorder) cannot be combined with kRemote, ring recorder not created");
        return nullptr;
    }

    std::string ring_name = ResolveRingName();
    if (ring_name.empty())
    {
        score::mw::log::detail::ReportInitializationError(
            score::mw::log::detail::Error::kRecorderFactoryUnsupportedLogMode,
            "DLT_RING_NAME is not a shared-memory object name (\"/name\"), ring recorder not created");
        return nullptr;
    }

    // The ring cannot carry more than DLT_SHM_RING_MAX_PAYLOAD per record, so do not reserve more.
    const auto max_payload = std::min<std::size_t>(config.GetSlotSizeInBytes(), DLT_SHM_RING_MAX_PAYLOAD);
    return std::make_unique<RingRecorder>(config, std::make_unique<ShmRingWriter>(std::move(ring_name)), max_payload);
}

}  // namespace dlt
}  // namespace qorix
