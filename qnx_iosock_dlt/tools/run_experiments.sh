#!/bin/bash
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
# Runs the verification and performance experiments of the io-sock DLT PoC on the QNX QEMU image, one after
# the other, and keeps the output of every experiment in its own directory:
#
#   <out>/<id>_<name>/run.log          full console output
#   <out>/<id>_<name>/command.txt      the commands that were run
#   <out>/<id>_<name>/status.txt       exit code and duration
#   <out>/<id>_<name>/test.log         the Bazel test log (QEMU experiments)
#   <out>/<id>_<name>/results/         bench_results.md/.json or bench_cpu_results.md/.json
#   <out>/<id>_<name>/module_build.log build log of the module variant, if it was (re)built for this experiment
#   <out>/summary.md                   one table with the status of every experiment and all result tables
#
# Usage (QNX SDP environment sourced first, e.g. `source ~/qnx800/qnxsdp-env.sh`). Experiments are selected by name;
# the single letter in brackets below is a short alias that also works:
#   qnx_iosock_dlt/tools/run_experiments.sh                                      # all experiments in the order below
#   qnx_iosock_dlt/tools/run_experiments.sh latency-datarouter cpu-datarouter    # only these
#   qnx_iosock_dlt/tools/run_experiments.sh --list
#
# Experiments (the module is rebuilt only when the variant changes, so keep experiments of one variant together):
#   host-tests [H]                    host unit and loopback tests (no QEMU)
#   ring-regression [R]               ring input regression on QEMU, module without batching
#   mwlog-regression [M]              mw::log application feeds the ring on QEMU, module without batching
#   latency-datarouter [A]            latency, delivery and loss, DataRouter (100 ms periods) against the ring, no batching
#   latency-datarouter-tuned [B]      as above, with the DataRouter restarted with DR_PERIOD_MS (default 10) periods
#   cpu-datarouter [D]                CPU use under sustained load, no batching
#   ring-regression-batching [X]      ring input regression on QEMU, module with batching
#   latency-batching [C]              as latency-datarouter, module with batching
#   cpu-batching [E]                  as cpu-datarouter, module with batching
#   cpu-batching-datarouter-tuned [F] as cpu-batching with the DataRouter restarted with DR_PERIOD_MS periods (the fair
#                                     CPU comparison: the DataRouter's 100 ms periods trade latency for CPU)
#
# Transport comparison with the userspace forwarder (tools/ring_forwarder, same ring, formatter and batching as the
# module, but sendmsg() through a BSD socket): only the Ring and Forwarder sinks, no DataRouter, so that the
# difference is the removed socket IPC hop. The forwarder gets its own ring and is built with `make qnx-tools`.
#   forwarder-functional [W]          forwarder functional test on QEMU (no module needed)
#   latency-forwarder [G]             latency, delivery and loss, module against forwarder, no batching
#   cpu-forwarder [J]                 CPU use per message and per datagram, module against forwarder, no batching, with
#                                     10 kHz and 20 kHz
#   latency-forwarder-batching [I]    as latency-forwarder, module and forwarder with batching
#   cpu-forwarder-batching [K]        as cpu-forwarder, module and forwarder with batching
#
# Deployment experiment: the module in a dedicated second io-sock instance on its own NIC (two-NIC QEMU setup,
# tools/run_under_qemu_2nic.sh; the unchanged plain module is loaded there):
#   second-stack [S]                  second io-sock stack: isolation from the default stack, and replacing the module by
#                                     restarting only that stack
#
# Settings (environment variables, defaults in brackets):
#   REPEATS [5]  CPU_REPEATS [3]  CPU_SECONDS [10]  DR_PERIOD_MS [10]  RING_SLOTS [2048]  TEST_TIMEOUT [3600]
#   OUT_DIR [qnx_iosock_dlt/out/experiments/<timestamp>]  DISTDIR [<repo>/qnx8080]  LIBCLANG_DIR [/usr/lib/llvm-18/lib]

set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPEATS="${REPEATS:-5}"
CPU_REPEATS="${CPU_REPEATS:-3}"
CPU_SECONDS="${CPU_SECONDS:-10}"
DR_PERIOD_MS="${DR_PERIOD_MS:-10}"
RING_SLOTS="${RING_SLOTS:-2048}"
TEST_TIMEOUT="${TEST_TIMEOUT:-3600}"
DISTDIR="${DISTDIR:-$ROOT/qnx8080}"
LIBCLANG_DIR="${LIBCLANG_DIR:-/usr/lib/llvm-18/lib}"
OUT_DIR="${OUT_DIR:-$ROOT/qnx_iosock_dlt/out/experiments/$(date +%Y%m%d-%H%M%S)}"

MODULE="$ROOT/qnx_iosock_dlt/qnx_module/mods-score-dlt-ring.so"
PRODUCER="$ROOT/qnx_iosock_dlt/out/qnx_x86_64/ring_producer"
FORWARDER="$ROOT/qnx_iosock_dlt/out/qnx_x86_64/ring_forwarder"
TESTLOGS="$ROOT/bazel-testlogs/feature_integration_tests/itf/itf"
SCRIPT_LOG="$OUT_DIR/script.log"

# The plain variants (no batching) run first, then the batching ones, so that the module is built twice at most.
# Internally an experiment is keyed by its letter; SLUG is the name used on the command line, in the log and in the
# output directory.
DEFAULT_ORDER="H W R M A B D G J S X C E F I K"
declare -A SLUG=([H]=host-tests [W]=forwarder-functional [R]=ring-regression [M]=mwlog-regression
    [A]=latency-datarouter [B]=latency-datarouter-tuned [D]=cpu-datarouter [G]=latency-forwarder
    [J]=cpu-forwarder [S]=second-stack [X]=ring-regression-batching [C]=latency-batching [E]=cpu-batching
    [F]=cpu-batching-datarouter-tuned [I]=latency-forwarder-batching [K]=cpu-forwarder-batching)
declare -A KEY_OF_SLUG
for key in "${!SLUG[@]}"; do KEY_OF_SLUG[${SLUG[$key]}]=$key; done
declare -A DESC=(
    [H]="Host unit and loopback tests"
    [W]="Forwarder functional test on QEMU"
    [R]="Ring input regression on QEMU, no batching"
    [M]="mw::log application feeds the ring, no batching"
    [A]="Latency, delivery, loss: DataRouter against ring, no batching"
    [B]="As A, DataRouter restarted with ${DR_PERIOD_MS} ms periods (tuned)"
    [D]="CPU use under sustained load, no batching"
    [G]="Latency, delivery, loss: module against forwarder, no batching"
    [J]="CPU use per message and datagram: module against forwarder, no batching"
    [S]="Module in a second io-sock instance on its own NIC: isolation and restart"
    [X]="Ring input regression on QEMU, with batching"
    [C]="As A, module with batching"
    [E]="As D, module with batching"
    [F]="As E, DataRouter restarted with ${DR_PERIOD_MS} ms periods (tuned)"
    [I]="As G, module and forwarder with batching"
    [K]="As J, module and forwarder with batching")
declare -A VARIANT=([R]=plain [M]=plain [A]=plain [B]=plain [D]=plain [G]=plain [J]=plain [S]=plain [X]=batch [C]=batch
    [E]=batch [F]=batch [I]=batch [K]=batch)
declare -A RESULT
declare -A SECS

BAZEL_FLAGS=(
    --config=qnx-x86_64
    "--distdir=$DISTDIR"
    "--action_env=LIBCLANG_PATH=$LIBCLANG_DIR"
    "--@score_logging//score/mw/log/flags:KCustom_Logging=True"
    "--@score_logging//score/mw/log/flags:custom_recorder_impl=//qnx_iosock_dlt/mwlog_ring:recorder"
)

CURRENT_VARIANT=""
RAN=()

log() {
    echo "$*" | tee -a "$SCRIPT_LOG"
}

# Runs a command, appends its output to <dir>/run.log, records the command and returns its exit code.
execute() {
    local dir="$1"
    shift
    printf '%q ' "$@" >>"$dir/command.txt"
    printf '\n' >>"$dir/command.txt"
    (cd "$ROOT" && "$@") 2>&1 | tee -a "$dir/run.log"
    return "${PIPESTATUS[0]}"
}

# Builds the module variant (plain or batch) unless it is the one that is already built.
ensure_module() {
    local dir="$1" variant="$2" extra=()
    if [ "$CURRENT_VARIANT" = "$variant" ]; then
        echo "module variant '$variant' already built, reused" >"$dir/module_build.log"
        return 0
    fi
    [ "$variant" = batch ] && extra=(BATCH_BYTES=1472)
    log "   building the module variant '$variant'"
    (cd "$ROOT" && make -C qnx_iosock_dlt qnx-module-ring "RING_SLOTS=$RING_SLOTS" "${extra[@]}") >"$dir/module_build.log" 2>&1 || {
        log "   module build failed, see $dir/module_build.log"
        return 1
    }
    CURRENT_VARIANT="$variant"
}

ensure_producer() {
    local dir="$1"
    [ -x "$PRODUCER" ] && [ -x "$FORWARDER" ] && return 0
    log "   building the QNX tools (ring producer and ring forwarder)"
    (cd "$ROOT" && make -C qnx_iosock_dlt qnx-tools) >"$dir/producer_build.log" 2>&1
}

# Test environment of the forwarder sink: its binary and the ring size of the module build.
FORWARDER_ENV=("--test_env=IOSOCK_DLT_RING_FORWARDER=$FORWARDER" "--test_env=IOSOCK_DLT_BENCH_RING_SLOTS=$RING_SLOTS"
    "--test_env=IOSOCK_DLT_BENCH_SINKS=Ring,Forwarder")

# bazel test of the ITF target on QEMU with a pytest -k filter; further arguments are extra bazel options.
qnx_test() {
    local dir="$1" filter="$2"
    shift 2
    execute "$dir" bazel test "${BAZEL_FLAGS[@]}" --config=itf-qnx-x86_64 --nocache_test_results \
        "--test_env=IOSOCK_DLT_RING_MODULE=$MODULE" "$@" "--test_timeout=$TEST_TIMEOUT" \
        --test_arg=-k "--test_arg=$filter" //feature_integration_tests/itf:itf --test_output=streamed
}

# Copies the test log and the test's undeclared outputs (result tables) of the experiment that just ran. The
# report that the tests print at their end is also extracted from run.log, in case the outputs are missing.
collect() {
    local dir="$1" file
    [ -f "$TESTLOGS/test.log" ] && [ "$TESTLOGS/test.log" -nt "$dir/.start" ] && cp "$TESTLOGS/test.log" "$dir/test.log"
    mkdir -p "$dir/results"
    for file in "$TESTLOGS"/test.outputs/*; do
        [ -f "$file" ] && [ "$file" -nt "$dir/.start" ] && cp "$file" "$dir/results/"
    done
    if [ -f "$TESTLOGS/test.outputs/outputs.zip" ] && [ "$TESTLOGS/test.outputs/outputs.zip" -nt "$dir/.start" ]; then
        python3 -m zipfile -e "$TESTLOGS/test.outputs/outputs.zip" "$dir/results"
    fi
    sed -n '/^Repetitions:/,/^PASSED/p' "$dir/run.log" | grep -v '^PASSED' >"$dir/results/report_from_run_log.txt"
    [ -s "$dir/results/report_from_run_log.txt" ] || rm -f "$dir/results/report_from_run_log.txt"
    gzip -f "$dir/run.log" "$dir/test.log" 2>/dev/null
}

run_experiment() {
    local id="$1"
    local dir="$OUT_DIR/${SLUG[$id]}"
    mkdir -p "$dir"
    : >"$dir/.start"
    log ""
    log "== ${SLUG[$id]}: ${DESC[$id]}  ->  $dir"
    local started=$SECONDS status=0

    if [ -n "${VARIANT[$id]:-}" ]; then
        ensure_module "$dir" "${VARIANT[$id]}" || status=1
    fi

    if [ "$status" -eq 0 ]; then
        case "$id" in
        H)
            execute "$dir" bazel test --config=linux-x86_64 --nocache_test_results //qnx_iosock_dlt/... \
                --test_output=errors
            status=$?
            ;;
        R | X)
            ensure_producer "$dir" || status=1
            if [ "$status" -eq 0 ]; then
                qnx_test "$dir" ring "--test_env=IOSOCK_DLT_RING_PRODUCER=$PRODUCER"
                status=$?
            fi
            ;;
        M)
            qnx_test "$dir" mwlog
            status=$?
            ;;
        W)
            ensure_producer "$dir" || status=1
            if [ "$status" -eq 0 ]; then
                qnx_test "$dir" forwarder "--test_env=IOSOCK_DLT_RING_FORWARDER=$FORWARDER" \
                    "--test_env=IOSOCK_DLT_RING_PRODUCER=$PRODUCER"
                status=$?
            fi
            ;;
        S)
            ensure_producer "$dir" || status=1
            if [ "$status" -eq 0 ]; then
                # The later --run_under and --qemu-config override the single-NIC ones of the itf config and the BUILD file.
                qnx_test "$dir" second_stack "--test_env=IOSOCK_DLT_RING_PRODUCER=$PRODUCER" \
                    "--run_under=$ROOT/qnx_iosock_dlt/tools/run_under_qemu_2nic.sh" \
                    "--test_arg=--qemu-config=$ROOT/feature_integration_tests/configs/qemu_bridge_2nic_config.json"
                status=$?
            fi
            ;;
        G | I)
            ensure_producer "$dir" || status=1
            if [ "$status" -eq 0 ]; then
                qnx_test "$dir" bench_datarouter "--test_env=IOSOCK_DLT_BENCH_REPEATS=$REPEATS" "${FORWARDER_ENV[@]}" \
                    "--test_env=IOSOCK_DLT_BENCH_FORWARDER_BATCH=$([ "$id" = I ] && echo 1472 || echo 0)"
                status=$?
            fi
            ;;
        J | K)
            ensure_producer "$dir" || status=1
            if [ "$status" -eq 0 ]; then
                qnx_test "$dir" bench_cpu "--test_env=IOSOCK_DLT_BENCH_CPU_REPEATS=$CPU_REPEATS" \
                    "--test_env=IOSOCK_DLT_BENCH_CPU_SECONDS=$CPU_SECONDS" "--test_env=IOSOCK_DLT_BENCH_CPU_HIGH_RATES=1" \
                    "${FORWARDER_ENV[@]}" \
                    "--test_env=IOSOCK_DLT_BENCH_FORWARDER_BATCH=$([ "$id" = K ] && echo 1472 || echo 0)"
                status=$?
            fi
            ;;
        A | C)
            qnx_test "$dir" bench_datarouter "--test_env=IOSOCK_DLT_BENCH_REPEATS=$REPEATS"
            status=$?
            ;;
        B)
            qnx_test "$dir" bench_datarouter "--test_env=IOSOCK_DLT_BENCH_REPEATS=$REPEATS" \
                "--test_env=IOSOCK_DLT_BENCH_DR_PERIOD_MS=$DR_PERIOD_MS"
            status=$?
            ;;
        D | E)
            qnx_test "$dir" bench_cpu "--test_env=IOSOCK_DLT_BENCH_CPU_REPEATS=$CPU_REPEATS" \
                "--test_env=IOSOCK_DLT_BENCH_CPU_SECONDS=$CPU_SECONDS"
            status=$?
            ;;
        F)
            qnx_test "$dir" bench_cpu "--test_env=IOSOCK_DLT_BENCH_CPU_REPEATS=$CPU_REPEATS" \
                "--test_env=IOSOCK_DLT_BENCH_CPU_SECONDS=$CPU_SECONDS" \
                "--test_env=IOSOCK_DLT_BENCH_DR_PERIOD_MS=$DR_PERIOD_MS"
            status=$?
            ;;
        esac
    fi

    case "$id" in H) ;; *) collect "$dir" ;; esac
    SECS[$id]=$((SECONDS - started))
    if [ "$status" -eq 0 ]; then RESULT[$id]=PASS; else RESULT[$id]="FAIL ($status)"; fi
    printf 'exit code %s\nseconds %s\n' "$status" "${SECS[$id]}" >"$dir/status.txt"
    log "   ${RESULT[$id]} after ${SECS[$id]} s"
    return "$status"
}

write_summary() {
    local summary="$OUT_DIR/summary.md" id dir file
    {
        echo "# Experiment summary"
        echo
        echo "Run on $(date) from $(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null), REPEATS=$REPEATS," \
            "CPU_REPEATS=$CPU_REPEATS, CPU_SECONDS=$CPU_SECONDS, RING_SLOTS=$RING_SLOTS, DR_PERIOD_MS=$DR_PERIOD_MS"
        echo
        echo "| Experiment | Description | Result | Seconds |"
        echo "|---|---|---|---|"
        for id in "${RAN[@]}"; do
            echo "| ${SLUG[$id]} | ${DESC[$id]} | ${RESULT[$id]:-not finished} | ${SECS[$id]:--} |"
        done
        for id in "${RAN[@]}"; do
            dir="$OUT_DIR/${SLUG[$id]}"
            for file in "$dir"/results/*.md "$dir"/results/report_from_run_log.txt; do
                [ -f "$file" ] || continue
                # The extracted report only stands in for result files that were not written.
                case "$file" in *report_from_run_log.txt) ls "$dir"/results/*.md >/dev/null 2>&1 && continue ;; esac
                echo
                echo "## ${SLUG[$id]}: ${DESC[$id]} ($(basename "$file"))"
                echo
                cat "$file"
            done
        done
    } >"$summary"
    log ""
    log "Summary: $summary"
}

main() {
    local args=("$@") ids=() id arg
    if [ "${args[0]:-}" = "--list" ]; then
        for id in $DEFAULT_ORDER; do printf '%-32s [%s]  %s\n' "${SLUG[$id]}" "$id" "${DESC[$id]}"; done
        return 0
    fi
    [ "${#args[@]}" -eq 0 ] && read -r -a args <<<"$DEFAULT_ORDER"
    for arg in "${args[@]}"; do
        # A name, or the single-letter alias.
        id="${KEY_OF_SLUG[$arg]:-}"
        [ -z "$id" ] && [ -n "${SLUG[$arg]:-}" ] && id="$arg"
        [ -n "$id" ] || { echo "unknown experiment '$arg', see --list" >&2; return 2; }
        ids+=("$id")
    done
    command -v bazel >/dev/null || { echo "bazel is not on the PATH" >&2; return 2; }
    for id in "${ids[@]}"; do
        if [ "$id" != H ] && [ -z "${QNX_TARGET:-}" ]; then
            echo "QNX_TARGET is not set: source the QNX SDP environment first (qnxsdp-env.sh)." >&2
            return 2
        fi
    done

    mkdir -p "$OUT_DIR"
    log "Output directory: $OUT_DIR"
    trap 'write_summary; exit 130' INT TERM

    local failed=0
    for id in "${ids[@]}"; do
        RAN+=("$id")
        run_experiment "$id" || failed=1
    done
    write_summary
    return "$failed"
}

main "$@"
