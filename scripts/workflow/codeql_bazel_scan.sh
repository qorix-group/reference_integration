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
#   1. Extracting compilation commands from Bazel's action graph (aquery)
#   2. Cleaning them (stripping -MD/-MF/-o flags that target read-only dirs)
#   3. Invoking the CodeQL C++ extractor directly for each command
#   4. Finalizing the database and running the analysis
#
# Usage:
#   ./codeql_bazel_scan.sh [options]
#
# Options:
#   --codeql-home <path>   Path to CodeQL bundle (default: auto-detect)
#   --bazel-config <name>  Bazel config to use (default: linux-x86_64)
#   --bazel-target <label> Bazel target pattern (default: @score_lifecycle_health//src/...)
#   --output-dir <path>    Directory for results (default: ./codeql-output)
#   --query-suite <suite>  CodeQL query suite (default: cpp-security-and-quality)
#   --help                 Show this help message
#
set -euo pipefail

# ─── Defaults ────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CODEQL_HOME="${CODEQL_HOME:-}"
BAZEL_CONFIG="linux-x86_64"
BAZEL_TARGET="@score_lifecycle_health//src/..."
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
        --bazel-config) BAZEL_CONFIG="$2"; shift 2 ;;
        --bazel-target) BAZEL_TARGET="$2"; shift 2 ;;
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

EXEC_ROOT="$(bazel info execution_root 2>/dev/null)"
echo "Bazel execution root: $EXEC_ROOT"

# Derive the external repo name from the target label (e.g. @score_lifecycle_health -> score_lifecycle_health+)
REPO_NAME="$(echo "$BAZEL_TARGET" | sed 's|^@||' | sed 's|//.*||')+"
SOURCE_ROOT="$EXEC_ROOT/external/$REPO_NAME"

if [[ ! -d "$SOURCE_ROOT" ]]; then
    echo "WARNING: Source root $SOURCE_ROOT does not exist yet."
    echo "Running a no-op build to materialise external repos..."
    bazel build --config "$BAZEL_CONFIG" --nobuild "$BAZEL_TARGET" 2>/dev/null || true
    SOURCE_ROOT="$EXEC_ROOT/external/$REPO_NAME"
fi

echo "Source root: $SOURCE_ROOT"

# ─── Detect compiler path ────────────────────────────────────────────────────

COMPILER="$(bazel aquery --config "$BAZEL_CONFIG" \
    "mnemonic(\"CppCompile\", $BAZEL_TARGET)" --output=text 2>/dev/null \
    | grep -oP '(?<=exec )\S+g\+\+' | head -1 || true)"

if [[ -z "$COMPILER" ]]; then
    echo "ERROR: Could not detect compiler from Bazel aquery output."
    exit 1
fi

# Make absolute
if [[ "$COMPILER" != /* ]]; then
    COMPILER="$EXEC_ROOT/$COMPILER"
fi
echo "Compiler: $COMPILER"

# ─── Output directory setup ──────────────────────────────────────────────────

DB_DIR="$OUTPUT_DIR/codeql-db"
SARIF_FILE="$OUTPUT_DIR/codeql-results.sarif"
HTML_REPORT="$OUTPUT_DIR/codeql-report.html"
COMPILE_DB="$OUTPUT_DIR/compile_commands.json"
TMPDIR_OBJ="$(mktemp -d "${TMPDIR:-/tmp}/codeql-bazel-XXXXXX")"

mkdir -p "$OUTPUT_DIR"

# ─── Step 1: Extract compilation commands from Bazel aquery ──────────────────

echo ""
echo "=== Step 1/4: Extracting compilation commands from Bazel aquery ==="

python3 - "$EXEC_ROOT" "$BAZEL_CONFIG" "$BAZEL_TARGET" "$COMPILE_DB" "$TMPDIR_OBJ" <<'PYTHON_SCRIPT'
import json, re, subprocess, os, sys

exec_root   = sys.argv[1]
config      = sys.argv[2]
target      = sys.argv[3]
output_file = sys.argv[4]
tmpdir      = sys.argv[5]

result = subprocess.run(
    ["bazel", "aquery", "--config", config,
     f'mnemonic("CppCompile", {target})', "--output=text"],
    capture_output=True, text=True
)

blocks = re.split(r'\naction ', result.stdout)
compile_commands = []

for block in blocks:
    cmd_match = re.search(r'Command Line: \(exec (.*?)\)', block, re.DOTALL)
    if not cmd_match:
        continue
    parts = []
    for line in cmd_match.group(1).split('\n'):
        line = line.strip().rstrip(' \\')
        if line:
            if line.startswith("'") and line.endswith("'"):
                line = line[1:-1]
            parts.append(line)
    if not parts:
        continue

    # Find source file
    source_file = None
    for p in reversed(parts):
        if p.endswith(('.cpp', '.cc', '.c', '.cxx')):
            source_file = p
            break
    if not source_file:
        continue

    # Clean flags: remove -MD, -MF <path>, -frandom-seed=, redirect -o
    cleaned = [parts[0]]
    skip_next = False
    for idx, p in enumerate(parts[1:], 1):
        if skip_next:
            skip_next = False
            continue
        if p == '-MD':
            continue
        if p == '-MF':
            skip_next = True
            continue
        if p.startswith('-frandom-seed='):
            continue
        if p == '-o':
            skip_next = True
            base = os.path.basename(parts[idx + 1]) if idx + 1 < len(parts) else "out.o"
            cleaned.extend(['-o', os.path.join(tmpdir, base)])
            continue
        cleaned.append(p)

    abs_source = source_file if os.path.isabs(source_file) else os.path.join(exec_root, source_file)
    compile_commands.append({
        "directory": exec_root,
        "command": ' '.join(cleaned),
        "file": abs_source
    })

with open(output_file, 'w') as f:
    json.dump(compile_commands, f, indent=2)

print(f"Extracted {len(compile_commands)} compile commands -> {output_file}")
PYTHON_SCRIPT

if [[ ! -s "$COMPILE_DB" ]]; then
    echo "ERROR: No compile commands extracted."
    exit 1
fi

COUNT="$(python3 -c "import json; print(len(json.load(open('$COMPILE_DB'))))")"
echo "Found $COUNT compilation units."

# ─── Step 2: Create CodeQL database via direct extraction ────────────────────

echo ""
echo "=== Step 2/4: Building CodeQL database ==="

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

ok=0
fail=0

python3 -c "
import json
for c in json.load(open('$COMPILE_DB')):
    parts = c['command'].split()
    print(' '.join(parts[1:]))
" | while IFS= read -r args; do
    i=$((ok + fail + 1))
    cd "$EXEC_ROOT"
    if "$EXTRACTOR" --mimic "$COMPILER" $args 2>/dev/null; then
        ok=$((ok + 1))
        echo -ne "\r  Extracted $ok / $COUNT (failed: $fail)"
    else
        fail=$((fail + 1))
        echo -ne "\r  Extracted $ok / $COUNT (failed: $fail)"
    fi
done

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
