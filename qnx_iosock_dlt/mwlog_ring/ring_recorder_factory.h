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
#ifndef QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_FACTORY_H
#define QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_FACTORY_H

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "score/mw/log/detail/log_recorder_factory.hpp"

namespace qorix
{
namespace dlt
{

/// Name of the shared-memory object the io-sock module creates (DLT_RING_NAME in iosock_module.c).
inline constexpr const char* kDefaultRingName{"/score_dlt_ring"};

/// Environment variable that selects another ring, for example the one of tools/ring_forwarder.cpp.
inline constexpr const char* kRingNameEnvironmentVariable{"DLT_RING_NAME"};

/// \brief Ring the recorder writes to: `DLT_RING_NAME` if set, else `kDefaultRingName`.
///
/// \return The name, or an empty string if the variable is set but is not a POSIX shared-memory name
///         ("/name" without a further slash). An invalid name is not replaced by the default, so a
///         benchmark cannot end up in the module's ring by accident.
inline std::string ResolveRingName()
{
    const char* const value = std::getenv(kRingNameEnvironmentVariable);
    if (value == nullptr)
    {
        return kDefaultRingName;
    }
    const std::string_view name{value};
    const bool valid = (name.size() > 1U) && (name.size() < 255U) && (name.front() == '/') &&
                       (name.find('/', 1U) == std::string_view::npos);
    return valid ? std::string{name} : std::string{};
}

/// \brief Creates the ring recorder for mw::log's `kCustom` log mode.
class RingRecorderFactory final : public score::mw::log::detail::LogRecorderFactory<RingRecorderFactory>
{
  public:
    RingRecorderFactory() = default;

    std::unique_ptr<score::mw::log::Recorder> CreateConcreteLogRecorder(
        const score::mw::log::detail::Configuration& config,
        score::cpp::pmr::memory_resource* memory_resource);
};

}  // namespace dlt
}  // namespace qorix

#endif  // QNX_IOSOCK_DLT_MWLOG_RING_RING_RECORDER_FACTORY_H
