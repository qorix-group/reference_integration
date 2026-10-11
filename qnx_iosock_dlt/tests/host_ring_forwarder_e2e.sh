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
# Host end-to-end run of the userspace forwarder: producer -> shared-memory ring -> ring_forwarder (sendmsg)
# -> DLT frame -> UDP. Started by tools/dlt_udp_check.py (--exec) after its socket is bound. Same shape as
# host_ring_e2e.sh, but the forwarder runs until it is stopped, like on the target, and its counters are checked.
#
# usage: host_ring_forwarder_e2e.sh <ring_forwarder> <ring_producer> <port> <count> [batch-bytes] [producer-period-ms]

set -u
FORWARDER="$1"
PRODUCER="$2"
PORT="$3"
COUNT="$4"
BATCH="${5:-0}"
PERIOD="${6:-5}"
NAME="/score_dlt_ring_fwd_e2e_$$"
LOG="${TMPDIR:-/tmp}/ring_forwarder_e2e_$$.log"

# Safety net of 60 s in case the stop below is never reached.
"$FORWARDER" "$NAME" 127.0.0.1 "$PORT" 256 512 "$BATCH" 1 60 >"$LOG" 2>&1 &
FORWARDER_PID=$!

"$PRODUCER" "$NAME" "$COUNT" "$PERIOD" ring-dlt DLTM FWD
PRODUCER_STATUS=$?

# Lets the forwarder drain the ring, then stops it like on the target.
sleep 1
kill -TERM "$FORWARDER_PID"
wait "$FORWARDER_PID"
FORWARDER_STATUS=$?

STATS=$(grep "ring_forwarder: records=" "$LOG" | tail -n 1)
echo "$STATS"
rm -f "$LOG"

FAILED=0
if [ "$PRODUCER_STATUS" -ne 0 ] || [ "$FORWARDER_STATUS" -ne 0 ]; then
    echo "producer exit $PRODUCER_STATUS, forwarder exit $FORWARDER_STATUS" >&2
    FAILED=1
fi
case "$STATS" in
*"records=$COUNT sent=$COUNT "*"dropped=0 failures=0 sendmsg_calls="*" sendmsg_errors=0 ring_full=0 ring_oversize=0 corrupt=0"*) ;;
*)
    echo "unexpected forwarder counters for $COUNT records: $STATS" >&2
    FAILED=1
    ;;
esac
exit "$FAILED"
