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
// Minimal mw::log application for the ring recorder end-to-end test (test_iosock_dlt_mwlog.py).
//
// Usage: mwlog_ring_app [count] [period_ms]
// Per iteration it logs one info message in context MWC1, one warning with mixed argument types in
// context MWC2 and one debug message in context MWD that the configured level (kInfo) must filter.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include "score/mw/log/logging.h"

int main(int argc, char** argv)
{
    const long count = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 20;
    const long period_ms = (argc > 2) ? std::strtol(argv[2], nullptr, 10) : 20;

    for (long i = 0; i < count; ++i)
    {
        const auto index = static_cast<std::uint32_t>(i);
        score::mw::log::LogInfo("MWC1") << "mwlog-ring" << index;
        score::mw::log::LogWarn("MWC2") << "mwlog-mixed" << static_cast<float>(index) << score::mw::log::LogHex32{0xBEEFU};
        score::mw::log::LogDebug("MWD") << "mwlog-filtered" << index;
        std::this_thread::sleep_for(std::chrono::milliseconds{period_ms});
    }
    return 0;
}
