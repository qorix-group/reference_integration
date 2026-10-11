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
# Target-side smoke test of the DLT io-sock module.
# Unloading is not supported for mods-* modules (umount answers "Not supported"), so it is only reported.
#
# usage: qnx_smoke_test.sh [module-path] [expected-records]
#   module-path       default /tmp_ram/mods-score-dlt.so (copy it there with scp first)
#   expected-records  default 10 (must match RECORD_COUNT used at build time)
#
# One PASS/FAIL line per check; the exit status is the number of failed checks.
# Start a receiver first (tools/dlt_udp_check.py on the host, or tcpdump on the target), see README.md.

MODULE="${1:-/tmp_ram/mods-score-dlt.so}"
EXPECTED="${2:-10}"
NAME="$(basename "$MODULE")"
ROOT="qnx.module.score_dlt"
failed=0

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; failed=$((failed + 1)); }

value() { sysctl -n "$ROOT.$1" 2>/dev/null; }

expect_value() { # expect_value <sysctl-leaf> <expected> <description>
    actual="$(value "$1")"
    if [ "$actual" = "$2" ]; then pass "$3 ($actual)"; else fail "$3 (got '$actual', want '$2')"; fi
}

echo "== load"
if mount -T io-sock "$MODULE"; then pass "mount -T io-sock $MODULE"; else fail "mount -T io-sock $MODULE"; fi
if ls "/dev/io-sock/$NAME" >/dev/null 2>&1; then pass "module listed in /dev/io-sock"; else fail "module listed in /dev/io-sock"; fi

echo "== version and health"
if [ -n "$(value iosock_abi)" ]; then pass "io-sock ABI version visible ($(value iosock_abi))"; else fail "io-sock ABI version visible"; fi
if [ -n "$(value version)" ]; then pass "module version visible ($(value version))"; else fail "module version visible"; fi

echo "== synthetic transmission"
tries=0
while [ "$tries" -lt 20 ] && [ "$(value state)" != "2" ] && [ "$(value state)" != "3" ]; do
    sleep 1
    tries=$((tries + 1))
done
expect_value state 2 "worker finished"
expect_value records_received "$EXPECTED" "records taken from the source"
expect_value packets_sent "$EXPECTED" "packets accepted by the stack"
expect_value packets_dropped 0 "no records dropped"
expect_value send_failures 0 "no send failures"
echo "last_error=$(value last_error) bytes_sent=$(value bytes_sent)"
if slog2info 2>/dev/null | grep -q dltmod; then pass "module log lines in slog2"; else fail "module log lines in slog2"; fi

echo "== unload (informational, not counted)"
if umount "/dev/io-sock/$NAME"; then echo "INFO: umount /dev/io-sock/$NAME succeeded"; else echo "INFO: umount /dev/io-sock/$NAME failed (expected: not supported for modules)"; fi

echo "$failed check(s) failed"
exit "$failed"
