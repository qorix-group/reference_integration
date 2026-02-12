#!/bin/bash
# *******************************************************************************
# Copyright (c) 2025 Contributors to the Eclipse Foundation
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
set -euo pipefail

# === Configuration ===
QNX_DIR="$HOME/qnx800"

# === Setup QNX SDP ===
if [[ ! -d "$QNX_DIR" ]]; then
    echo "Error: QNX 8.0 SDP expected at $QNX_DIR"
    exit 1
fi

# Set QNX environment variables
source ${QNX_DIR}/qnxsdp-env.sh

# Build io-sock module
make clean
make
cp *.so ../../images/qnx_x86_64/libs/mods-kso.so
echo "[✓] io-sock module built and copied to images directory"
