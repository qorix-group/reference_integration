#!/bin/sh
# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
# Copyright (c) 2026 Qorix
# SPDX-License-Identifier: Apache-2.0
#
# Host end-to-end run of the ring input: producer -> shared-memory ring -> bridge (module stand-in)
# -> DLT frame -> UDP. Started by tools/dlt_udp_check.py (--exec) after its socket is bound.
#
# usage: host_ring_e2e.sh <host_ring_bridge> <ring_producer> <port> <count> [batch-bytes] [producer-period-ms]
#   batch-bytes > 0 lets the bridge pack several DLT messages into one datagram, a producer period of 0
#   writes the records back to back so that several are waiting when the bridge reads.

set -u
BRIDGE="$1"
PRODUCER="$2"
PORT="$3"
COUNT="$4"
BATCH="${5:-0}"
PERIOD="${6:-5}"
NAME="/score_dlt_ring_e2e_$$"

"$BRIDGE" "$NAME" 127.0.0.1 "$PORT" "$COUNT" 256 512 20 "$BATCH" &
BRIDGE_PID=$!

"$PRODUCER" "$NAME" "$COUNT" "$PERIOD" ring-dlt DLTM RNG
PRODUCER_STATUS=$?

wait "$BRIDGE_PID"
BRIDGE_STATUS=$?

if [ "$PRODUCER_STATUS" -ne 0 ] || [ "$BRIDGE_STATUS" -ne 0 ]; then
    echo "producer exit $PRODUCER_STATUS, bridge exit $BRIDGE_STATUS" >&2
    exit 1
fi
exit 0
