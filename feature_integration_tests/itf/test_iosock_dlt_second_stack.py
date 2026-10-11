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
"""ITF test: the DLT io-sock module in a second, dedicated io-sock instance (qnx_iosock_dlt).

Needs a target with two network interfaces: vtnet0 (SSH, the default io-sock stack) and vtnet1 (a free NIC for the
logging stack). The test skips itself if vtnet1 does not exist, so it can stay in the ITF target. Run it with the
two-NIC environment (tap0 + tap1 on the host, see tools/run_under_qemu_2nic.sh):

    IOSOCK_DLT_RING_MODULE=$PWD/qnx_iosock_dlt/qnx_module/mods-score-dlt-ring.so \\
    IOSOCK_DLT_RING_PRODUCER=$PWD/qnx_iosock_dlt/out/qnx_x86_64/ring_producer \\
    bazel test $QNX_FLAGS --config=itf-qnx-x86_64 \\
        --run_under=$PWD/qnx_iosock_dlt/tools/run_under_qemu_2nic.sh \\
        --test_arg=--qemu-config=$PWD/feature_integration_tests/configs/qemu_bridge_2nic_config.json \\
        --test_env=IOSOCK_DLT_RING_MODULE --test_env=IOSOCK_DLT_RING_PRODUCER \\
        --test_arg=-k --test_arg=second_stack //feature_integration_tests/itf:itf --test_output=streamed

What it does (every step is logged with its command and output):
  1. At boot the default stack attaches both virtio NICs (Plug and Play). The test releases vtnet1 from it.
  2. It starts a second stack, `io-sock -o prefix=/alt -m phy -m pci -d vtnet_pci`, which picks up the free NIC
     (called vtnet0 inside that stack; the test matches it by MAC address), and gives it 192.168.77.2/24 and a
     multicast route.
  3. It loads the unchanged module into that stack (`mount -T io-sock -o prefix=/alt ...`) and writes records into
     the ring. They must arrive on the host through tap1 (192.168.77.1) and not through tap0 (raw capture of the DLT
     multicast frames on both tap devices), and the module must be visible only through the second stack's sysctl
     tree.
  4. The default stack and the SSH connection keep working throughout.
  5. It kills only the second stack (the module disappears with it), starts a new one and loads the module again:
     the module is replaced without touching the default stack.
"""

import logging
import os
import re
import socket
import threading
import time

import pytest

import bench_metrics
from score.itf.plugins.dlt.dlt_receive import Protocol
from score.itf.plugins.dlt.dlt_window import DltWindow

logger = logging.getLogger(__name__)

MODULE_ENV = "IOSOCK_DLT_RING_MODULE"
PRODUCER_ENV = "IOSOCK_DLT_RING_PRODUCER"
HOST_IP_ENV = "IOSOCK_DLT_STACK2_HOST_IP"

REMOTE_MODULE = "/tmp_ram/mods-score-dlt-ring.so"
REMOTE_PRODUCER = "/tmp_ram/ring_producer"
STACK_LOG = "/tmp_ram/io-sock-alt.log"
RING_NAME = "/score_dlt_ring"

PREFIX = "/alt"
NIC = "vtnet1"  # name of the second NIC in the default stack
ALT_NIC = "vtnet0"  # interface names are per stack: the second stack numbers its first vtnet device 0
PRIMARY_NIC = "vtnet0"
PRIMARY_IP = "169.254.158.190"
STACK2_IP = "192.168.77.2"
STACK2_NETMASK = "255.255.255.0"
DEFAULT_HOST_IP = "192.168.77.1"  # tap1 in tools/run_under_qemu_2nic.sh
TAP_DEFAULT = "tap0"
TAP_SECOND = "tap1"
DLT_PORT = 3490
SYSCTL_ROOT = "qnx.module.score_dlt"

APP_ID = "DLTM"
CTX_FIRST = "SS2"
CTX_AFTER_RESTART = "SS3"
RECORDS = 20
PRODUCER_PERIOD_MS = 20

# The ssh session may not have the boot image on its PATH or library path.
_ENV = "PATH=$PATH:/proc/boot LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/proc/boot"
_PIDIN = '/proc/boot/pidin -F "%a %u %v %N"'
_STACK_TIMEOUT_SEC = 15
_NIC_TIMEOUT_SEC = 15
_STATE_TIMEOUT_SEC = 15
_POLL_INTERVAL_SEC = 0.5
_LAST_DATAGRAM_GRACE_SEC = 1
_started_stack_pids = set()  # for the cleanup after a failed test


def _run(target, command):
    """Runs a command on the target, logs it with its result and returns (exit code, output text)."""
    exit_code, out = target.execute(command)
    text = out.decode(errors="replace").strip()
    logger.info("$ %s\n  -> exit %s\n%s", command, exit_code, text)
    return exit_code, text


def _alt(command):
    """Prefixes a command so that it talks to the second stack."""
    return f"{_ENV} SOCK={PREFIX} {command}"


def _wait_for(predicate, timeout):
    """Polls predicate() until it returns a truthy value or the timeout expires; returns the last value."""
    deadline = time.monotonic() + timeout
    value = predicate()
    while not value and time.monotonic() < deadline:
        time.sleep(_POLL_INTERVAL_SEC)
        value = predicate()
    return value


def _io_sock_pids(target):
    _, out = target.execute(_PIDIN)
    return set(bench_metrics.parse_pidin_pids(out.decode(errors="replace"), "io-sock"))


def _stack2_sysctl(target, leaf):
    exit_code, text = target.execute(_alt(f"sysctl -n {SYSCTL_ROOT}.{leaf}"))
    text = text.decode(errors="replace").strip()
    return text if exit_code == 0 else None


def _primary_stack_alive(target):
    """The default stack still serves SSH and still has vtnet0 with its address."""
    exit_code, text = _run(target, f"{_ENV} ifconfig {PRIMARY_NIC}")
    return exit_code == 0 and PRIMARY_IP in text


class _FrameCounter:
    """Counts the DLT multicast frames (UDP to `group`:DLT_PORT) that cross a host interface.

    A raw AF_PACKET socket sees exactly what goes over that tap device. A second dlt-receive bound to the other
    interface cannot serve for this: on Linux a socket bound to the wildcard address also gets multicast traffic
    that arrives on other interfaces (IP_MULTICAST_ALL), so it would report frames of tap1 as well.
    """

    def __init__(self, interface, group):
        self._interface = interface
        self._group = socket.inet_aton(group)
        self._stop = threading.Event()
        self._thread = None
        self._socket = None
        self.frames = 0

    def _matches(self, frame):
        if len(frame) < 42 or frame[12:14] != b"\x08\x00":  # Ethernet, IPv4
            return False
        header_len = (frame[14] & 0x0F) * 4
        if frame[23] != 17 or frame[30:34] != self._group:  # UDP, destination group
            return False
        port = int.from_bytes(frame[14 + header_len + 2 : 14 + header_len + 4], "big")
        return port == DLT_PORT

    def _loop(self):
        while not self._stop.is_set():
            try:
                frame = self._socket.recv(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            if self._matches(frame):
                self.frames += 1

    def __enter__(self):
        self._socket = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))  # ETH_P_ALL
        self._socket.bind((self._interface, 0))
        self._socket.settimeout(0.2)
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_):
        time.sleep(0.3)  # lets the thread read what is already queued
        self._stop.set()
        self._thread.join(timeout=2)
        self._socket.close()


def _mac_of(target, command):
    """MAC address (`ether xx:..`) in the output of an ifconfig command, or None."""
    exit_code, out = target.execute(command)
    match = re.search(r"ether\s+([0-9a-f:]{17})", out.decode(errors="replace"))
    return match.group(1) if (exit_code == 0 and match) else None


@pytest.fixture
def stack_files():
    module = os.environ.get(MODULE_ENV)
    producer = os.environ.get(PRODUCER_ENV)
    if not module or not os.path.isfile(module):
        pytest.skip(f"{MODULE_ENV} does not point to a built mods-score-dlt-ring.so")
    if not producer or not os.path.isfile(producer):
        pytest.skip(f"{PRODUCER_ENV} does not point to a QNX ring_producer binary")
    return module, producer


@pytest.fixture
def two_nic_target(target):
    _, out = target.execute("uname -s")
    if b"QNX" not in out:
        pytest.skip("io-sock tests run on QNX targets only")
    exit_code, _ = _run(target, f"{_ENV} ifconfig {NIC}")
    if exit_code != 0:
        pytest.skip(f"no {NIC}: needs the two-NIC environment (run_under_qemu_2nic.sh and qemu_bridge_2nic_config.json)")
    assert _primary_stack_alive(target), f"{PRIMARY_NIC} does not have {PRIMARY_IP}: the NIC order is not as expected"
    yield target
    # Leave no second stack behind for later tests of the session.
    _stop_stack2(target)


def _start_stack2(target, expected_mac):
    """Starts the second stack (the NIC must be free) and brings its interface up; returns the new io-sock pids.

    The NIC is recognised by its MAC address: inside the second stack it is called ALT_NIC, not NIC.
    """
    before = _io_sock_pids(target)
    _run(target, f"{_ENV} nohup io-sock -o prefix={PREFIX} -m phy -m pci -d vtnet_pci > {STACK_LOG} 2>&1 &")

    def socket_ready():
        exit_code, _ = target.execute(f"ls {PREFIX}/dev/socket")
        return exit_code == 0

    ready = _wait_for(socket_ready, _STACK_TIMEOUT_SEC)
    _, log = _run(target, f"cat {STACK_LOG}")
    assert ready, f"the second stack did not create {PREFIX}/dev/socket; its output:\n{log}"
    new_pids = _io_sock_pids(target) - before
    assert new_pids, "no new io-sock process appeared"
    _started_stack_pids.update(new_pids)

    def nic_attached():
        return _mac_of(target, _alt(f"ifconfig {ALT_NIC}")) == expected_mac

    attached = _wait_for(nic_attached, _NIC_TIMEOUT_SEC)
    if not attached:
        _run(target, _alt("ifconfig -a"))
        _run(target, f"ls -R {PREFIX}/dev/io-sock")
    assert attached, (
        f"{ALT_NIC} with MAC {expected_mac} did not appear in the second stack; see the logged ifconfig and driver listing"
    )

    _run(target, _alt(f"if_up -p {ALT_NIC}"))
    exit_code, text = _run(target, _alt(f"ifconfig {ALT_NIC} {STACK2_IP} netmask {STACK2_NETMASK}"))
    assert exit_code == 0, f"cannot configure {ALT_NIC} in the second stack: {text}"
    exit_code, text = _run(target, _alt(f"route add -net 224.0.0.0 -netmask 240.0.0.0 -interface {ALT_NIC}"))
    assert exit_code == 0, f"cannot add the multicast route in the second stack: {text}"
    return new_pids


def _stop_stack2(target, pids=None):
    """Kills only the second stack (SIGTERM, then SIGKILL) and waits until its socket is gone."""
    if pids is None:
        exit_code, _ = target.execute(f"ls {PREFIX}/dev/socket")
        if exit_code != 0:
            return
        pids = set(_started_stack_pids)
    for signal in ("", "-9"):
        for pid in pids:
            target.execute(f"kill {signal} {pid}")
        deadline = time.monotonic() + _STACK_TIMEOUT_SEC / 3
        while time.monotonic() < deadline:
            exit_code, _ = target.execute(f"ls {PREFIX}/dev/socket")
            if exit_code != 0:
                _started_stack_pids.difference_update(pids)
                return
            time.sleep(_POLL_INTERVAL_SEC)
    logger.warning("the second stack is still present after SIGKILL")


def _load_module(target):
    exit_code, text = _run(target, f"{_ENV} mount -T io-sock -o prefix={PREFIX} {REMOTE_MODULE}")
    assert exit_code == 0, f"mount -T io-sock -o prefix={PREFIX} failed: {text}"
    state = _wait_for(lambda: _stack2_sysctl(target, "state") in ("1", "3") and _stack2_sysctl(target, "state"),
                      _STATE_TIMEOUT_SEC)
    assert state == "1", f"module worker state is {state!r}, last_error={_stack2_sysctl(target, 'last_error')!r}"


def _send_and_receive(target, dlt_config, host_ip, ctx, count):
    """Writes `count` records into the ring.

    Returns (decoded DLT messages received on the second NIC, DLT frames seen on tap1, DLT frames seen on tap0).
    """
    group = dlt_config.multicast_ips[0]
    with _FrameCounter(TAP_SECOND, group) as second_tap, _FrameCounter(TAP_DEFAULT, group) as default_tap, DltWindow(
        protocol=Protocol.UDP,
        host_ip=host_ip,
        multicast_ips=dlt_config.multicast_ips,
        print_to_stdout=True,
        binary_path=dlt_config.dlt_receive_path,
        logger_name="dlt_second_nic",
    ) as window:
        exit_code, text = _run(
            target, f"{REMOTE_PRODUCER} {RING_NAME} {count} {PRODUCER_PERIOD_MS} ring-dlt {APP_ID} {ctx}"
        )
        assert exit_code == 0, f"ring_producer failed: {text}"
        _wait_for(lambda: _stack2_sysctl(target, "packets_sent") == str(count), _STATE_TIMEOUT_SEC)
        time.sleep(_LAST_DATAGRAM_GRACE_SEC)

    record = window.record(filters=[(APP_ID, ctx)])
    messages = record.find(query=dict(apid=APP_ID, ctid=ctx))
    return messages, second_tap.frames, default_tap.frames


def test_iosock_dlt_second_stack_isolates_and_restarts_module(two_nic_target, stack_files, dlt_config):
    """The module runs in a dedicated io-sock instance, is isolated from the default stack and can be replaced."""
    target = two_nic_target
    module_path, producer_path = stack_files
    host_ip = os.environ.get(HOST_IP_ENV, DEFAULT_HOST_IP)
    with target.sftp() as sftp:
        sftp.upload(module_path, REMOTE_MODULE)
        sftp.upload(producer_path, REMOTE_PRODUCER)
    exit_code, out = target.execute(f"chmod +x {REMOTE_PRODUCER}")
    assert exit_code == 0, f"chmod failed: {out.decode(errors='replace')}"

    # 1. Release the second NIC from the default stack, which attached both at boot.
    exit_code, text = _run(target, "ls /dev/io-sock/devs-vtnet_pci.so")
    assert NIC in text, f"the default stack has no {NIC} device: {text}"
    nic_mac = _mac_of(target, f"{_ENV} ifconfig {NIC}")
    assert nic_mac, f"cannot read the MAC address of {NIC}"
    exit_code, text = _run(target, f"{_ENV} umount /dev/io-sock/devs-vtnet_pci.so/{NIC}")
    assert exit_code == 0, f"cannot release {NIC} from the default stack: {text}"
    assert _primary_stack_alive(target), "the default stack lost vtnet0 when vtnet1 was released"

    # 2. Second stack with the freed NIC.
    stack_pids = _start_stack2(target, nic_mac)
    logger.info("second stack pids: %s", sorted(stack_pids))
    assert _primary_stack_alive(target), "the default stack was affected by starting the second stack"
    exit_code, text = _run(target, f"{_ENV} ifconfig {NIC}")
    assert exit_code != 0, f"{NIC} is still visible in the default stack: {text}"

    # 3. The unchanged module in the second stack.
    _load_module(target)
    assert _stack2_sysctl(target, "input") == "1"
    exit_code, _ = _run(target, f"{_ENV} sysctl -n {SYSCTL_ROOT}.state")
    assert exit_code != 0, "the module is visible in the default stack's sysctl tree"
    assert _primary_stack_alive(target), "the default stack was affected by loading the module"

    on_second, tap1_frames, tap0_frames = _send_and_receive(target, dlt_config, host_ip, CTX_FIRST, RECORDS)
    logger.info("decoded %d messages; DLT frames: %d on tap1, %d on tap0", len(on_second), tap1_frames, tap0_frames)
    assert _stack2_sysctl(target, "packets_sent") == str(RECORDS)
    assert _stack2_sysctl(target, "send_failures") == "0"
    assert len(on_second) >= RECORDS, f"expected {RECORDS} DLT messages through the second NIC, got {len(on_second)}"
    assert tap1_frames >= RECORDS, f"only {tap1_frames} DLT frames crossed tap1"
    assert tap0_frames == 0, f"{tap0_frames} DLT frames crossed tap0, the default NIC"
    assert _primary_stack_alive(target), "the default stack was affected while the module sent"

    # 4. Replace the module by restarting only the second stack.
    _stop_stack2(target, stack_pids)
    exit_code, _ = _run(target, f"ls {PREFIX}/dev/socket")
    assert exit_code != 0, "the second stack is still running"
    assert _primary_stack_alive(target), "killing the second stack affected the default stack"
    exit_code, _ = _run(target, "uname -a")
    assert exit_code == 0, "the SSH session over the default stack is broken"

    stack_pids = _start_stack2(target, nic_mac)
    assert _stack2_sysctl(target, "state") is None, "the old module is still present in the new stack"
    _load_module(target)
    assert _stack2_sysctl(target, "packets_sent") == "0", "the counters were not reset by the restart"

    on_second, tap1_frames, tap0_frames = _send_and_receive(target, dlt_config, host_ip, CTX_AFTER_RESTART, RECORDS)
    assert _stack2_sysctl(target, "packets_sent") == str(RECORDS)
    assert len(on_second) >= RECORDS, f"expected {RECORDS} DLT messages after the restart, got {len(on_second)}"
    assert tap1_frames >= RECORDS, f"only {tap1_frames} DLT frames crossed tap1 after the restart"
    assert tap0_frames == 0, f"{tap0_frames} DLT frames crossed tap0 after the restart"
    assert _primary_stack_alive(target), "the default stack was affected by the restart"
