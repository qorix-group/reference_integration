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
"""Independent DLT-over-UDP checker for the io-sock DLT.

Receives UDP datagrams, decodes the DLT v1 headers (standard, extra, extended) and the verbose
payload arguments straight from the protocol layout, and verifies the synthetic stream emitted by
the module (first argument "iosock-dlt", second argument an incrementing uint32).

It shares no code with the C++ formatter, so a pass is an independent check of the wire format.
Standard library only; runs on a Linux host or on any target that has Python 3.

Examples:
    # unicast
    dlt_udp_check.py --bind 0.0.0.0 --port 3490 --count 10
    # multicast (DataRouter default group)
    dlt_udp_check.py --bind 0.0.0.0 --port 3490 --group 239.255.42.99 --count 10
    # loopback self-test: bind first, then run the sender
    dlt_udp_check.py --bind 127.0.0.1 --port 39490 --count 5 --exec "build/host_udp_sender 127.0.0.1 39490 5"

Exit code: 0 = expected number of valid synthetic messages seen, 1 = check failed, 2 = usage/socket error.
"""

import argparse
import shlex
import socket
import struct
import subprocess
import sys
import time

HTYP_UEH = 0x01
HTYP_MSBF = 0x02
HTYP_WEID = 0x04
HTYP_WSID = 0x08
HTYP_WTMS = 0x10

LEVEL_NAMES = {1: "fatal", 2: "error", 3: "warn", 4: "info", 5: "debug", 6: "verbose"}

TYPE_INFO_STRG = 0x00000200
TYPE_INFO_UINT = 0x00000040
TYPE_INFO_TYLE_MASK = 0x0000000F


class DltError(Exception):
    """Raised when a datagram does not follow the DLT v1 layout."""


def _id(raw):
    return raw.rstrip(b"\x00").decode("ascii", errors="replace")


def decode_arguments(payload, count, little_endian):
    """Decodes `count` verbose arguments; returns a list of str/int/bytes values."""
    endian = "<" if little_endian else ">"
    args = []
    pos = 0
    for _ in range(count):
        if pos + 4 > len(payload):
            raise DltError("payload ends inside a type-info field")
        (type_info,) = struct.unpack_from(endian + "I", payload, pos)
        pos += 4
        if type_info & TYPE_INFO_STRG:
            (length,) = struct.unpack_from(endian + "H", payload, pos)
            pos += 2
            raw = payload[pos:pos + length]
            if len(raw) != length:
                raise DltError("string argument exceeds the payload")
            pos += length
            args.append(raw.rstrip(b"\x00").decode("utf-8", errors="replace"))
        elif type_info & TYPE_INFO_UINT:
            size = {1: 1, 2: 2, 3: 4, 4: 8}.get(type_info & TYPE_INFO_TYLE_MASK)
            if size is None:
                raise DltError("unsupported unsigned integer length")
            raw = payload[pos:pos + size]
            if len(raw) != size:
                raise DltError("integer argument exceeds the payload")
            pos += size
            args.append(int.from_bytes(raw, "little" if little_endian else "big"))
        else:
            args.append(payload[pos:])
            pos = len(payload)
            break
    return args


def decode_datagram(datagram):
    """Splits a datagram into DLT messages (a datagram may carry several) and decodes each."""
    messages = []
    offset = 0
    while offset < len(datagram):
        if len(datagram) - offset < 4:
            raise DltError("truncated standard header")
        htyp, mcnt, length = struct.unpack_from(">BBH", datagram, offset)
        version = (htyp >> 5) & 0x07
        if version != 1:
            raise DltError("unsupported DLT version %d" % version)
        if length < 4 or offset + length > len(datagram):
            raise DltError("length field %d does not fit the datagram" % length)

        pos = offset + 4
        message = {"mcnt": mcnt, "length": length, "ecu": None, "seid": None, "tmsp": None,
                   "level": None, "noar": 0, "apid": None, "ctid": None, "verbose": False}
        if htyp & HTYP_WEID:
            message["ecu"] = _id(datagram[pos:pos + 4])
            pos += 4
        if htyp & HTYP_WSID:
            (message["seid"],) = struct.unpack_from(">I", datagram, pos)
            pos += 4
        if htyp & HTYP_WTMS:
            (message["tmsp"],) = struct.unpack_from(">I", datagram, pos)
            pos += 4
        if htyp & HTYP_UEH:
            msin, noar = datagram[pos], datagram[pos + 1]
            message["verbose"] = bool(msin & 0x01)
            message["level"] = (msin >> 4) & 0x0F
            message["noar"] = noar
            message["apid"] = _id(datagram[pos + 2:pos + 6])
            message["ctid"] = _id(datagram[pos + 6:pos + 10])
            pos += 10

        payload = datagram[pos:offset + length]
        message["payload_size"] = len(payload)
        message["args"] = (decode_arguments(payload, message["noar"], not (htyp & HTYP_MSBF))
                           if message["verbose"] else [])
        messages.append(message)
        offset += length
    return messages


def check_synthetic(message, expected_text, expected_ecu):
    """Returns a list of problems found in one decoded message (empty list = valid)."""
    problems = []
    if expected_ecu and message["ecu"] != expected_ecu:
        problems.append("ECU id %r != %r" % (message["ecu"], expected_ecu))
    if not message["verbose"]:
        problems.append("not a verbose message")
    if message["level"] not in LEVEL_NAMES:
        problems.append("invalid log level %r" % message["level"])
    args = message["args"]
    if len(args) != 2 or args[0] != expected_text or not isinstance(args[1], int):
        problems.append("arguments %r do not match [%r, <uint32>]" % (args, expected_text))
    return problems


def join_group(sock, group, interface):
    membership = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton(interface))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--bind", default="0.0.0.0", help="local address to bind (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=3490, help="UDP port (default 3490)")
    parser.add_argument("--group", help="IPv4 multicast group to join")
    parser.add_argument("--iface", default="0.0.0.0", help="local interface address for the group join")
    parser.add_argument("--count", type=int, default=1, help="valid messages required (default 1)")
    parser.add_argument("--timeout", type=float, default=15.0, help="overall timeout in seconds")
    parser.add_argument("--expect-text", default="iosock-dlt", help="expected first argument")
    parser.add_argument("--expect-ecu", default="IOS1", help="expected ECU id (empty: do not check)")
    parser.add_argument("--exec", dest="command", help="command to run after the socket is bound")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.bind, args.port))
        if args.group:
            join_group(sock, args.group, args.iface)
    except OSError as error:
        print("socket setup failed: %s" % error, file=sys.stderr)
        return 2

    child = None
    if args.command:
        child = subprocess.Popen(shlex.split(args.command))

    valid = 0
    invalid = 0
    previous_mcnt = None
    mcnt_gaps = 0
    deadline = time.monotonic() + args.timeout
    try:
        while valid < args.count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                datagram, peer = sock.recvfrom(65535)
            except socket.timeout:
                break
            try:
                messages = decode_datagram(datagram)
            except DltError as error:
                invalid += 1
                print("INVALID datagram from %s:%d: %s" % (peer[0], peer[1], error))
                continue
            for message in messages:
                problems = check_synthetic(message, args.expect_text, args.expect_ecu)
                if previous_mcnt is not None and message["mcnt"] != (previous_mcnt + 1) % 256:
                    mcnt_gaps += 1
                previous_mcnt = message["mcnt"]
                status = "ok" if not problems else "BAD (" + "; ".join(problems) + ")"
                print("from %s:%d ecu=%s apid=%s ctid=%s level=%s mcnt=%d tmsp=%s len=%d args=%r -> %s" % (
                    peer[0], peer[1], message["ecu"], message["apid"], message["ctid"],
                    LEVEL_NAMES.get(message["level"], message["level"]), message["mcnt"],
                    message["tmsp"], message["length"], message["args"], status))
                if problems:
                    invalid += 1
                else:
                    valid += 1
    finally:
        sock.close()
        if child is not None:
            child.wait()

    print("valid=%d invalid=%d mcnt_gaps=%d (required valid=%d)" % (valid, invalid, mcnt_gaps, args.count))
    if child is not None and child.returncode != 0:
        print("--exec command failed with exit code %d" % child.returncode, file=sys.stderr)
        return 1
    return 0 if (valid >= args.count and invalid == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
