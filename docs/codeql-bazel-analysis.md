# CodeQL Analysis for Bazel C++ Targets

## Overview

This documents how to run CodeQL static analysis on Bazel-built C++ code in this
repository. A dedicated script handles the entire pipeline: extracting
compilation commands, building the CodeQL database, running queries, and
producing an HTML report.

## Why a custom approach?

CodeQL normally intercepts compiler invocations via `LD_PRELOAD`. This does
**not** work with Bazel's hermetic toolchain because:

1. Bazel downloads its own GCC (`x86_64-unknown-linux-gnu-g++`) which is
   **statically linked** — `LD_PRELOAD` cannot intercept it.
2. Bazel's sandboxed execution hides compiler calls from the tracer.
3. Bazel's output directories are read-only, so replaying raw compilation
   commands fails on `-MF` (dependency file) writes.

The script works around all three issues by:

1. Using `bazel aquery` to extract the exact compilation commands from Bazel's
   action graph — no actual build needed.
2. Cleaning the commands (stripping `-MD`, `-MF`, `-frandom-seed`, redirecting
   `-o` to a temp directory).
3. Invoking the CodeQL C++ **extractor directly** (`--mimic <compiler>`) for
   each command, bypassing the `LD_PRELOAD` tracer entirely.

## Prerequisites

| Tool          | Minimum Version | Notes                                       |
|---------------|-----------------|---------------------------------------------|
| CodeQL CLI    | 2.15+           | Download the [bundle][codeql-bundle]         |
| Bazel         | 7.x             | Already configured in the repo               |
| Python 3      | 3.10+           | Used for aquery parsing                      |
| sarif-tools   | 3.x             | Optional, for HTML report (`pip install sarif-tools`) |

[codeql-bundle]: https://github.com/github/codeql-action/releases

## Quick start

```bash
# Set the path to your CodeQL bundle
export CODEQL_HOME=/path/to/codeql-bundle-linux64/codeql

# Run with defaults (score_lifecycle_health, linux-x86_64)
scripts/workflow/codeql_bazel_scan.sh
```

Results are written to `codeql-output/`:
- `codeql-db/` — the CodeQL database (reusable for custom queries)
- `codeql-results.sarif` — machine-readable findings
- `codeql-report.html` — human-readable report

## Script options

```
scripts/workflow/codeql_bazel_scan.sh [options]

  --codeql-home <path>    Path to CodeQL bundle directory
  --bazel-config <name>   Bazel --config value (default: linux-x86_64)
  --bazel-target <label>  Bazel target pattern (default: @score_lifecycle_health//src/...)
  --output-dir <path>     Where to write results (default: ./codeql-output)
  --query-suite <suite>   Query suite name without "cpp-" prefix (default: security-and-quality)
  --help                  Show help
```

### Examples

```bash
# Scan a different target with the code-scanning suite
scripts/workflow/codeql_bazel_scan.sh \
  --bazel-target "@score_baselibs//score/mw/log/..." \
  --query-suite "security-extended"

# Use a specific CodeQL bundle and write results elsewhere
scripts/workflow/codeql_bazel_scan.sh \
  --codeql-home /opt/codeql \
  --output-dir /tmp/codeql-results

# Scan for QNX targets
scripts/workflow/codeql_bazel_scan.sh \
  --bazel-config qnx-x86_64
```

## Available query suites

| Suite name                  | Focus                                    |
|-----------------------------|------------------------------------------|
| `security-and-quality`      | Security vulnerabilities + code quality (default) |
| `security-extended`         | Extended security checks                          |
| `security-experimental`     | Experimental security queries                     |

For MISRA checks, install the `codeql/misra-cpp-coding-standards` pack and run:

```bash
codeql database analyze ./codeql-output/codeql-db \
  codeql/misra-cpp-coding-standards \
  --format=sarif-latest \
  --output=misra-results.sarif
```

## How it works (step by step)

```
┌─────────────────────────────────────────────────────────────┐
│ 1. bazel aquery --config <cfg> 'mnemonic("CppCompile",...)'│
│    → extracts compiler flags, includes, source file paths   │
├─────────────────────────────────────────────────────────────┤
│ 2. Python: clean commands → compile_commands.json           │
│    → strips -MD/-MF/-o, redirects outputs to tmpdir         │
├─────────────────────────────────────────────────────────────┤
│ 3. codeql database init (source-root = external repo)       │
│    → creates skeleton DB, sets source root so files are     │
│      classified as "user code" (not library code)           │
├─────────────────────────────────────────────────────────────┤
│ 4. For each command:                                        │
│    extractor --mimic <bazel-gcc> <flags> <source.cpp>       │
│    → CodeQL C++ extractor parses source, writes TRAP files  │
├─────────────────────────────────────────────────────────────┤
│ 5. codeql database finalize                                 │
│    → imports TRAP files into the relational database        │
├─────────────────────────────────────────────────────────────┤
│ 6. codeql database analyze                                  │
│    → runs query suite, writes SARIF                         │
├─────────────────────────────────────────────────────────────┤
│ 7. sarif html → HTML report                                 │
└─────────────────────────────────────────────────────────────┘
```

## CI integration

The script can be used in a GitHub Actions workflow:

```yaml
- name: Download CodeQL Bundle
  run: |
    wget -q https://github.com/github/codeql-action/releases/latest/download/codeql-bundle-linux64.tar.gz
    tar xzf codeql-bundle-linux64.tar.gz

- name: Run CodeQL Bazel Scan
  run: |
    scripts/workflow/codeql_bazel_scan.sh --codeql-home codeql

- name: Upload SARIF
  uses: github/codeql-action/upload-sarif@v4
  with:
    sarif_file: codeql-output/codeql-results.sarif

- name: Upload HTML Report
  uses: actions/upload-artifact@v4
  with:
    name: codeql-html-report
    path: codeql-output/codeql-report.html
```

## Reusing the database

The generated database at `codeql-output/codeql-db/` can be opened in VS Code
with the [CodeQL extension](https://marketplace.visualstudio.com/items?itemName=GitHub.vscode-codeql)
for interactive exploration, or queried from the CLI:

```bash
# Run a single query
codeql query run \
  --database=codeql-output/codeql-db \
  path/to/custom-query.ql

# Run the MISRA pack
codeql database analyze codeql-output/codeql-db \
  codeql/misra-cpp-coding-standards \
  --format=csv --output=misra.csv
```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No compile commands extracted" | Bazel aquery found no `CppCompile` actions | Check `--bazel-target` is correct and has C++ code |
| "0 TRAP files generated" | Extractor can't find compiler or headers | Ensure a successful `bazel build` has run at least once to populate the execution root |
| Extraction fails with "Permission denied" on `.d` files | Using raw Bazel commands instead of the script | The script strips `-MD`/`-MF` flags; don't bypass it |
| "0 lines of user code" | Source root doesn't match file locations | The script auto-detects source root from the target label; verify with `--bazel-target` |
| Individual file extraction fails | Source code has syntax errors | Check the build output; e.g. stray characters in source files |
