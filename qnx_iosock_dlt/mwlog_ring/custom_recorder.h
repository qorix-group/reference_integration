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
#ifndef QNX_IOSOCK_DLT_MWLOG_RING_CUSTOM_RECORDER_H
#define QNX_IOSOCK_DLT_MWLOG_RING_CUSTOM_RECORDER_H

// Picked up by score_logging's custom_registrant.cpp through the `custom_recorder_impl` label flag.
#include "ring_recorder.h"
#include "ring_recorder_factory.h"

namespace score
{
namespace mw
{
namespace log
{
namespace detail
{
using CustomRecorder = qorix::dlt::RingRecorder;
using CustomRecorderFactory = qorix::dlt::RingRecorderFactory;
}  // namespace detail
}  // namespace log
}  // namespace mw
}  // namespace score

#endif  // QNX_IOSOCK_DLT_MWLOG_RING_CUSTOM_RECORDER_H
