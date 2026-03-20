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
#
# CodeQL analysis for Bazel-built C++ targets.
#
# Bazel uses a hermetic (statically linked) GCC toolchain, which prevents
# CodeQL's LD_PRELOAD-based tracer from intercepting compiler invocations.
# This script works around that by:
#   1. Using Hedron's compile_commands extractor to generate compile_commands.json
#   2. Cleaning commands (stripping flags that target read-only Bazel output dirs)
#   3. Invoking the CodeQL C++ extractor directly for each command
#   4. Finalizing the database and running the analysis
#
# Prerequisites:
#   - hedron_compile_commands configured in MODULE.bazel
#   - refresh_compile_commands target defined in BUILD
#
# Usage:
#   ./codeql_bazel_scan.sh [options]
#
# Options:
#   --codeql-home <path>   Path to CodeQL bundle (default: auto-detect via CODEQL_HOME or PATH)
#   --output-dir <path>    Directory for results (default: ./codeql-output)
#   --query-suite <suite>  CodeQL query suite (default: security-and-quality)
#   --help                 Show this help message
#
set -euo pipefail

# ─── Defaults ────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CODEQL_HOME="${CODEQL_HOME:-}"
OUTPUT_DIR="$REPO_ROOT/codeql-output"
QUERY_SUITE="security-and-quality"

# ─── Argument parsing ────────────────────────────────────────────────────────

usage() {
    sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# //' | sed 's/^#//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --codeql-home)  CODEQL_HOME="$2"; shift 2 ;;
        --output-dir)   OUTPUT_DIR="$2";   shift 2 ;;
        --query-suite)  QUERY_SUITE="$2";  shift 2 ;;
        --help|-h)      usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# ─── Resolve CodeQL ──────────────────────────────────────────────────────────

if [[ -z "$CODEQL_HOME" ]]; then
    if command -v codeql &>/dev/null; then
        CODEQL_HOME="$(dirname "$(dirname "$(command -v codeql)")")"
    else
        echo "ERROR: CodeQL not found. Set --codeql-home or CODEQL_HOME, or add codeql to PATH."
        exit 1
    fi
fi

CODEQL="$CODEQL_HOME/codeql"
EXTRACTOR="$CODEQL_HOME/cpp/tools/linux64/extractor"

for bin in "$CODEQL" "$EXTRACTOR"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: Required binary not found or not executable: $bin"
        exit 1
    fi
done

echo "CodeQL home: $CODEQL_HOME"
echo "CodeQL version: $("$CODEQL" version --format=terse 2>/dev/null)"

# ─── Resolve Bazel paths ─────────────────────────────────────────────────────

OUTPUT_BASE="$(bazel info output_base 2>/dev/null)"
echo "Bazel output base: $OUTPUT_BASE"

# ─── Output directory setup ──────────────────────────────────────────────────

DB_DIR="$OUTPUT_DIR/codeql-db"
SARIF_FILE="$OUTPUT_DIR/codeql-results.sarif"
HTML_REPORT="$OUTPUT_DIR/codeql-report.html"
COMPILE_DB="$OUTPUT_DIR/compile_commands.json"
TMPDIR_OBJ="$(mktemp -d "${TMPDIR:-/tmp}/codeql-bazel-XXXXXX")"

mkdir -p "$OUTPUT_DIR"

# ─── Step 1: Generate compile_commands.json via Hedron ───────────────────────

echo ""
echo "=== Step 1/4: Generating compile_commands.json via Hedron ==="

cd "$REPO_ROOT"
bazel run //:refresh_compile_commands 2>&1 | tail -5

HEDRON_COMPILE_DB="$REPO_ROOT/compile_commands.json"
if [[ ! -s "$HEDRON_COMPILE_DB" ]]; then
    echo "ERROR: Hedron did not generate compile_commands.json at $HEDRON_COMPILE_DB"
    exit 1
fi

# Clean the compile commands for CodeQL — only strip -z <arg> for now
python3 - "$HEDRON_COMPILE_DB" "$COMPILE_DB" <<'PYTHON_SCRIPT'
import json, sys

input_file  = sys.argv[1]
output_file = sys.argv[2]

with open(input_file) as f:
    commands = json.load(f)

cleaned = []
for entry in commands:
    parts = entry.get("command", "").split()
    if not parts:
        parts = entry.get("arguments", [])
    if not parts:
        continue

    source_file = entry.get("file", "")
    if not source_file:
        continue

    new_parts = [parts[0]]
    skip_next = False
    for idx, p in enumerate(parts[1:], 1):
        if skip_next:
            skip_next = False
            continue
        if p == '-z' or p.startswith('-z '):
            skip_next = p == '-z'
            continue
        new_parts.append(p)

    cleaned.append({
        "directory": entry.get("directory", ""),
        "command": ' '.join(new_parts),
        "file": source_file
    })

with open(output_file, 'w') as f:
    json.dump(cleaned, f, indent=2)

print(f"Cleaned {len(cleaned)} compile commands -> {output_file}")
PYTHON_SCRIPT

if [[ ! -s "$COMPILE_DB" ]]; then
    echo "ERROR: No compile commands after cleaning."
    exit 1
fi

COUNT="$(python3 -c "import json; print(len(json.load(open('$COMPILE_DB'))))")"
echo "Found $COUNT compilation units."

# ─── Step 2: Create CodeQL database via direct extraction ────────────────────

echo ""
echo "=== Step 2/4: Building CodeQL database ==="

# Detect compiler from first entry in compile_commands.json
COMPILER="$(python3 -c "
import json
entry = json.load(open('$COMPILE_DB'))[0]
parts = entry.get('command', '').split()
print(parts[0])
")"
echo "Compiler: $COMPILER"

# Hedron's compile_commands reference files via Bazel's external/ convenience symlinks
# (e.g. external/score_lifecycle_health+/...). The CodeQL extractor resolves symlinks,
# so archived files end up under the Bazel output_base/external/ path.
# The source root must match these resolved paths for CodeQL to classify them as "user code".
#
# Auto-detect the primary source repo from compile commands: pick the external repo
# containing the most .cpp/.cc source files, excluding toolchain and third-party repos.
SOURCE_REPO="$(python3 -c "
import json, re
from collections import Counter
entries = json.load(open('$COMPILE_DB'))
repos = []
for e in entries:
    f = e.get('file', '')
    if not f.endswith(('.cpp', '.cc', '.c', '.cxx')):
        continue
    m = re.match(r'external/([^/]+)/', f)
    if m:
        name = m.group(1)
        # Skip toolchains, test frameworks, and codegen libraries
        if any(x in name for x in ['toolchain', 'googletest', 'flatbuffers', 'rules_']):
            continue
        repos.append(name)
c = Counter(repos)
if c:
    print(c.most_common(1)[0][0])
")"

if [[ -z "$SOURCE_REPO" ]]; then
    echo "WARNING: Could not detect source repo from compile commands. Using broad source root."
    SOURCE_ROOT="$OUTPUT_BASE/external"
else
    SOURCE_ROOT="$OUTPUT_BASE/external/$SOURCE_REPO"
    echo "Detected source repo: $SOURCE_REPO"
fi
echo "Source root: $SOURCE_ROOT"

rm -rf "$DB_DIR"
"$CODEQL" database init "$DB_DIR" --language=cpp --source-root="$SOURCE_ROOT"

# Set extractor environment
export CODEQL_EXTRACTOR_CPP_TRAP_DIR="$DB_DIR/trap/cpp"
export CODEQL_EXTRACTOR_CPP_SOURCE_ARCHIVE_DIR="$DB_DIR/src"
export CODEQL_EXTRACTOR_CPP_SCRATCH_DIR="$DB_DIR/working"
export CODEQL_EXTRACTOR_CPP_WIP_DATABASE="$DB_DIR"
export CODEQL_EXTRACTOR_CPP_DIAGNOSTIC_DIR="$DB_DIR/diagnostic/cpp"
export CODEQL_EXTRACTOR_CPP_ROOT="$CODEQL_HOME/cpp"
export CODEQL_EXTRACTOR_CPP_LOG_DIR="$DB_DIR/log"
export CODEQL_PLATFORM="linux64"

mkdir -p "$CODEQL_EXTRACTOR_CPP_TRAP_DIR" \
         "$CODEQL_EXTRACTOR_CPP_SOURCE_ARCHIVE_DIR" \
         "$CODEQL_EXTRACTOR_CPP_SCRATCH_DIR" \
         "$CODEQL_EXTRACTOR_CPP_DIAGNOSTIC_DIR"

JOBS="$(nproc)"
echo "Running extraction with $JOBS parallel jobs..."

# Generate a shell script per compile command, then run in parallel.
# This avoids env-var and quoting issues with xargs subshells.
JOBDIR="$(mktemp -d "${TMPDIR:-/tmp}/codeql-jobs-XXXXXX")"

python3 - "$COMPILE_DB" "$JOBDIR" "$EXTRACTOR" "$COMPILER" <<'PYSCRIPT'
import json, os, sys

compile_db = sys.argv[1]
jobdir     = sys.argv[2]
extractor  = sys.argv[3]
compiler   = sys.argv[4]

entries = json.load(open(compile_db))
for i, entry in enumerate(entries):
    parts = entry.get("command", "").split()
    if not parts:
        continue
    directory = entry.get("directory", ".")
    args = " ".join(parts[1:])
    script = os.path.join(jobdir, f"job_{i:04d}.sh")
    with open(script, "w") as f:
        f.write(f'#!/bin/bash\ncd {directory!r} && {extractor!r} --mimic {compiler!r} {args}\n')
    os.chmod(script, 0o755)
print(f"Generated {len(entries)} job scripts")
PYSCRIPT

find "$JOBDIR" -name "job_*.sh" | sort | xargs -P "$JOBS" -I {} bash {} || true
rm -rf "$JOBDIR"

echo ""

trap_count="$(find "$CODEQL_EXTRACTOR_CPP_TRAP_DIR" -name "*.trap*" 2>/dev/null | wc -l)"
echo "Generated $trap_count TRAP files."

if [[ "$trap_count" -eq 0 ]]; then
    echo "ERROR: No TRAP files generated — extraction failed."
    exit 1
fi

echo "Finalizing database..."
"$CODEQL" database finalize "$DB_DIR" --no-pre-finalize

# ─── Step 3: Run CodeQL analysis ─────────────────────────────────────────────

echo ""
echo "=== Step 3/4: Running CodeQL analysis (suite: $QUERY_SUITE) ==="

"$CODEQL" database analyze \
    "$DB_DIR" \
    "codeql/cpp-queries:codeql-suites/cpp-${QUERY_SUITE}.qls" \
    --format=sarif-latest \
    --output="$SARIF_FILE" \
    --threads=0

# ─── Step 4: Generate HTML report ────────────────────────────────────────────

echo ""
echo "=== Step 4/4: Generating HTML report ==="

if command -v sarif &>/dev/null; then
    sarif html "$SARIF_FILE" --output "$HTML_REPORT"
elif python3 -c "import sarif_tools" 2>/dev/null; then
    python3 -m sarif html "$SARIF_FILE" --output "$HTML_REPORT"
else
    echo "WARNING: sarif-tools not installed. Skipping HTML generation."
    echo "  Install with: pip3 install sarif-tools"
    HTML_REPORT=""
fi

# ─── Summary ─────────────────────────────────────────────────────────────────

echo ""
echo "=========================================="
echo "  CodeQL Analysis Complete"
echo "=========================================="

FINDING_COUNT="$(python3 -c "
import json
with open('$SARIF_FILE') as f:
    sarif = json.load(f)
total = sum(len(r.get('results', [])) for r in sarif.get('runs', []))
print(total)
")"

echo "  Findings:    $FINDING_COUNT"
echo "  Database:    $DB_DIR"
echo "  SARIF:       $SARIF_FILE"
[[ -n "${HTML_REPORT:-}" ]] && echo "  HTML Report: $HTML_REPORT"
echo ""

# Cleanup temp dir
rm -rf "$TMPDIR_OBJ"

exit 0
