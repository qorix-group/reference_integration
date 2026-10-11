#!/usr/bin/env python3
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
"""Checks that the portable DLT core is freestanding.

Takes the files of the core library target (the static archive is picked), links all its objects
into one relocatable object and requires that it has no undefined symbols and no static
constructor/destructor sections. Needs ar, ld, nm and readelf from the host's binutils.

Usage: check_freestanding.py <library files...>
Exit code: 0 = freestanding, 1 = violation, 2 = usage or tool error.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = ("ar", "ld", "nm", "readelf")
FORBIDDEN_SECTIONS = re.compile(r"\.(init_array|ctors|fini_array|dtors)")
# Defined by the linker itself when position-independent code is built, not a real dependency.
LINKER_PROVIDED = {"_GLOBAL_OFFSET_TABLE_"}


def run(command, cwd=None):
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True).stdout


def main(files):
    archives = [os.path.abspath(f) for f in files if f.endswith(".a")]
    if not archives:
        print("no static archive among: %s" % files, file=sys.stderr)
        return 2
    missing = [tool for tool in TOOLS if shutil.which(tool) is None]
    if missing:
        print("missing host tools: %s" % ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as workdir:
        run(["ar", "x", archives[0]], cwd=workdir)
        objects = sorted(glob.glob(os.path.join(workdir, "*.o")))
        if not objects:
            print("archive contains no objects", file=sys.stderr)
            return 2
        combined = os.path.join(workdir, "combined.relocatable")
        run(["ld", "-r", "-o", combined] + objects)

        undefined = [line for line in run(["nm", "-u", combined]).splitlines()
                     if line.split()[-1] not in LINKER_PROVIDED]
        sections = run(["readelf", "-S", combined])

    failed = False
    if undefined:
        print("undefined symbols in the linked core:\n%s" % "\n".join(undefined))
        failed = True
    if FORBIDDEN_SECTIONS.search(sections):
        print("static constructor/destructor section in the linked core")
        failed = True
    if failed:
        return 1
    print("core (%d objects linked together): no undefined symbols, no static constructors" % len(objects))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
