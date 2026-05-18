
#!/usr/bin/env python3
"""Full-screen CLI for the resident Ethernet bootloader."""

from __future__ import annotations

import argparse
import hashlib
import ctypes
import errno
import ipaddress
import re
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import tempfile
from dataclasses import dataclass, replace
from typing import Callable


PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PROTO_MAGIC = 0x424C4452
PROTO_VERSION = 1
PROTO_MSG_DISCOVER_REQ = 0x01
PROTO_MSG_DISCOVER_REPLY = 0x02
PROTO_MSG_BCAST_UID_REQ = 0x12
PROTO_MSG_BCAST_UID_REPLY = 0x13
PROTO_MSG_DEVICETREE_REQ = 0x20
PROTO_MSG_DEVICETREE_REPLY = 0x21
PROTO_MSG_ERROR_REPLY = 0x7F
PROTO_FLAG_REPLY = 1 << 0
DISCOVERY_PORT = 45000
CONTROL_PORT = 45001
DEFAULT_BIND_IP = "192.168.1.99"
DEFAULT_TARGET = "255.255.255.255"
DEFAULT_REFRESH_S = 0.5
DEFAULT_REQUEST_TIMEOUT_S = 1.0
DEFAULT_WRITE_TIMEOUT_S = 0.5
TREE_REFRESH_S = 0.5
PASSIVE_TREE_TIMEOUT_S = 0.1
DEFAULT_CONTROL_RTT_S = 0.01
MIN_CONTROL_RETRY_S = 0.002
MAX_CONTROL_RETRY_S = 0.5
DUMMY_HEX_PATH = os.path.join(os.path.dirname(__file__), "dummy_app.hex")
PROGRAM_IMAGE_CANDIDATES = [
    os.path.join(PROJECT_ROOT, "Firmware", "Debug", "Bootloader.bin"),
    os.path.join(PROJECT_ROOT, "Firmware", "Debug", "Bootloader.hex"),
    os.path.join(PROJECT_ROOT, "Firmware", "Debug", "Bootloader.elf"),
]
BOOT_PAYLOAD_CANDIDATES = [
    os.path.join(os.path.dirname(__file__), "boot_payload.bin"),
    os.path.join(PROJECT_ROOT, "Firmware", "Debug", "Bootloader.bin"),
]
PROGRAM_STATE_PATH = [2, 1]
PROGRAM_TCP_PORT_PATH = [2, 2]
PROGRAM_APPLY_BOOT_UPDATE_PATH = [2, 3]
PROGRAM_READY_STATE = "ProgrammingReady"
PROGRAM_ERASING_STATE = "Erasing"
PROGRAM_STOPPED_STATE = "Stopped"
PROGRAM_BLOCK_BYTES = 1024
PROGRAM_WINDOW_FRAMES = 4
PROGRAM_BLOCK_MAX_RETRIES = 3
PROGRAM_TCP_TIMEOUT_S = 3.0
PROGRAM_READY_TIMEOUT_S = 30.0
APP_STORE_SIZE_BYTES = 640 * 1024
BOOT_PAYLOAD_BASE_OFFSET = 0x60000
BOOT_PAYLOAD_SLOT_SIZE_BYTES = 256 * 1024
BOOT_PAYLOAD_HEADER_MAGIC = 0x54534C42
BOOT_PAYLOAD_HEADER = struct.Struct("<II20sI")
APP_IMAGE_MAGIC = 0x41505031
APP_IMAGE_ABI_VERSION = 1
APP_IMAGE_SHA1_DIGEST_BYTES = 20
APP_IMAGE_DIGEST_SHA1 = 1
APP_IMAGE_FLAG_VALID = 1 << 0
APP_EXEC_BASE = 0x10000000
APP_EXEC_SIZE = 64 * 1024
APP_IMAGE_HEADER_FORMAT = "<IHHIIIIIIIIIB3s20s"
APP_IMAGE_HEADER_SIZE = struct.calcsize(APP_IMAGE_HEADER_FORMAT)

ACCESS_READ = 0x8
ACCESS_WRITE = 0x4
ACCESS_EXECUTE = 0x2

DISCOVERY_HEADER = struct.Struct("<IBBHI")
COMMAND_HEADER = struct.Struct("<IBBHIHH")
DISCOVERY_REPLY = struct.Struct("<IBBHI12s6s4s4s4sIII")
DEVICE_TREE_OP_LIST = 0x01
DEVICE_TREE_OP_PAGING = 0x80
DEVICE_TREE_OP_GET = 0x02
DEVICE_TREE_OP_SET = 0x03
DEVICE_TREE_OP_EXECUTE = 0x04
STANDARD_REBOOT_PATH = [5]
# Device tree locations (see Firmware/Resident/Src/resident_device_tree.c TREE_ID_*).
IPV4_ADDRESS_PATH = [1, 2]
IPV4_SUBNET_PATH = [1, 3]
IPV4_GATEWAY_PATH = [1, 4]
IPV4_DHCP_PATH = [1, 5]
DEVICE_MENU_ITEMS = [
    "Device Tree",
    "Write Program",
    "Write Boot Payload",
    "Apply Boot Update",
    "Reboot Device",
]

PROG_FRAME_HELLO = 0x01
PROG_FRAME_DATA = 0x02
PROG_FRAME_FINISH = 0x03
PROG_FRAME_ABORT = 0x04
PROG_FRAME_ACK = 0x81
PROG_FRAME_NACK = 0x82
PROG_FLAG_RAW_STORAGE = 1 << 1
PROG_HEADER = struct.Struct("<IBBHIHH")
PROG_HELLO_PAYLOAD = struct.Struct("<I20s")
PROG_HELLO_RAW_PAYLOAD = struct.Struct("<I20sI")
PROG_DATA_LEN = struct.Struct("<H")
PROG_FINISH_PAYLOAD = struct.Struct("<I20s")
PROG_ACK_PAYLOAD = struct.Struct("<II")
PROG_NACK_PAYLOAD = struct.Struct("<IhH")


@dataclass
class Device:
    source: tuple[str, int]
    transaction_id: int
    uid: bytes
    mac: bytes
    ip: str
    subnet: str
    gateway: str
    capabilities: int
    resident_version: int
    app_version: int
    first_seen: float
    last_seen: float


@dataclass
class TreeItem:
    node_id: int
    has_children: bool
    access: int
    name: str
    value: str
    write_effect: str
    description: str
    action: bool
    unit: str
    enum_values: list[str]


@dataclass
class NetworkInterface:
    name: str
    ip: str
    subnet: str
    gateway: str


@dataclass
class ProgrammingUpdate:
    phase: str | None = None
    status: str | None = None
    payload_name: str | None = None
    image_bytes: int | None = None
    total_blocks: int | None = None
    sent_blocks: int | None = None
    in_flight_blocks: int | None = None
    acked_blocks: int | None = None
    acked_bytes: int | None = None


@dataclass
class ProgrammingSnapshot:
    active: bool = False
    phase: str = "idle"
    status: str = ""
    payload_name: str = ""
    image_bytes: int = 0
    total_blocks: int = 0
    sent_blocks: int = 0
    in_flight_blocks: int = 0
    acked_blocks: int = 0
    acked_bytes: int = 0
    finished: bool = False
    success: bool = False
    result: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Interactive CLI for the resident bootloader.")
    parser.add_argument(
        "--target",
        default=DEFAULT_TARGET,
        help="discovery destination IPv4 address",
    )
    parser.add_argument(
        "--bind-ip",
        default=None,
        help="local IPv4 address to bind; skips interface prompt when provided",
    )
    parser.add_argument("--port", type=int, default=DISCOVERY_PORT, help="UDP discovery port")
    parser.add_argument(
        "--refresh",
        type=float,
        default=DEFAULT_REFRESH_S,
        help="seconds between automatic discovery requests",
    )
    parser.add_argument(
        "--program-image",
        default=None,
        help=(
            "programming payload to stream; defaults to a packaged bootloader artifact "
            "when available, otherwise a generated large stress image"
        ),
    )
    parser.add_argument(
        "--boot-payload",
        default=None,
        help="boot payload .bin for sectors 9-10 (raw bootloader or BLST-packed blob)",
    )
    return parser.parse_args()


def is_usable_ipv4(ip: str) -> bool:
    try:
        address = ipaddress.IPv4Address(ip)
    except ipaddress.AddressValueError:
        return False
    return not address.is_loopback and not address.is_unspecified


def list_windows_interfaces() -> list[NetworkInterface]:
    try:
        result = subprocess.run(
            ["netsh", "interface", "ipv4", "show", "addresses"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return []

    interfaces: list[NetworkInterface] = []
    current_name = ""
    current_ip = ""
    current_subnet = ""
    current_gateway = ""
    seen: set[tuple[str, str]] = set()

    def flush_current() -> None:
        nonlocal current_ip, current_subnet, current_gateway
        key = (current_name, current_ip)
        if current_name and is_usable_ipv4(current_ip) and key not in seen:
            interfaces.append(NetworkInterface(current_name, current_ip, current_subnet, current_gateway))
            seen.add(key)
        current_ip = ""
        current_subnet = ""
        current_gateway = ""

    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if line.startswith('Configuration for interface "') and line.endswith('"'):
            flush_current()
            current_name = line.removeprefix('Configuration for interface "').removesuffix('"')
            continue

        if current_name and line.startswith("IP Address:"):
            flush_current()
            ip = line.split(":", 1)[1].strip()
            current_ip = ip
        elif current_name and line.startswith("Subnet Prefix:"):
            if "(mask " in line:
                current_subnet = line.split("(mask ", 1)[1].split(")", 1)[0].strip()
        elif current_name and line.startswith("Default Gateway:"):
            gateway = line.split(":", 1)[1].strip()
            if is_usable_ipv4(gateway):
                current_gateway = gateway

    flush_current()
    return interfaces


def list_fallback_interfaces() -> list[NetworkInterface]:
    interfaces: list[NetworkInterface] = []
    seen: set[str] = set()
    try:
        candidates = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        candidates = []

    for candidate in candidates:
        ip = candidate[4][0]
        if is_usable_ipv4(ip) and ip not in seen:
            interfaces.append(NetworkInterface("Local interface", ip, "", ""))
            seen.add(ip)

    if DEFAULT_BIND_IP not in seen:
        interfaces.append(NetworkInterface("Default bootloader NIC", DEFAULT_BIND_IP, "", ""))

    return interfaces


def list_network_interfaces() -> list[NetworkInterface]:
    if os.name == "nt":
        interfaces = list_windows_interfaces()
        if interfaces:
            return interfaces
    return list_fallback_interfaces()


def default_interface_index(interfaces: list[NetworkInterface]) -> int:
    return next(
        (index for index, interface in enumerate(interfaces) if interface.ip == DEFAULT_BIND_IP),
        0,
    )


def interface_for_bind_ip(bind_ip: str, interfaces: list[NetworkInterface]) -> NetworkInterface:
    for interface in interfaces:
        if interface.ip == bind_ip:
            return interface
    return NetworkInterface("Manual bind IP", bind_ip, "", "")


def choose_bind_ip(explicit_bind_ip: str | None) -> str:
    if explicit_bind_ip:
        return explicit_bind_ip

    interfaces = list_network_interfaces()
    if not interfaces:
        return DEFAULT_BIND_IP

    print("Select local network interface for bootloader discovery:")
    for index, interface in enumerate(interfaces, start=1):
        print(f" {index}. {interface.name} - {interface.ip}")

    default_index = next(
        (index for index, interface in enumerate(interfaces, start=1) if interface.ip == DEFAULT_BIND_IP),
        1,
    )
    while True:
        try:
            selection = input(f"Interface [{default_index}]: ").strip()
        except EOFError:
            selection = ""

        if not selection:
            return interfaces[default_index - 1].ip
        if selection.isdigit():
            selected_index = int(selection)
            if 1 <= selected_index <= len(interfaces):
                return interfaces[selected_index - 1].ip

        print(f"Please enter a number from 1 to {len(interfaces)}.")


def make_discovery_request(transaction_id: int) -> bytes:
    return DISCOVERY_HEADER.pack(
        PROTO_MAGIC,
        PROTO_VERSION,
        PROTO_MSG_DISCOVER_REQ,
        0,
        transaction_id,
    )


def parse_discovery_reply(data: bytes, source: tuple[str, int]) -> Device | None:
    if len(data) < DISCOVERY_REPLY.size:
        return None

    (
        magic,
        proto_version,
        msg_type,
        flags,
        transaction_id,
        uid,
        mac,
        ip,
        subnet,
        gateway,
        capabilities,
        resident_version,
        app_version,
    ) = DISCOVERY_REPLY.unpack_from(data)

    if magic != PROTO_MAGIC:
        return None
    if proto_version != PROTO_VERSION:
        return None
    if msg_type != PROTO_MSG_DISCOVER_REPLY:
        return None
    if (flags & PROTO_FLAG_REPLY) == 0:
        return None

    now = time.monotonic()
    return Device(
        source=source,
        transaction_id=transaction_id,
        uid=uid,
        mac=mac,
        ip=str(ipaddress.IPv4Address(ip)),
        subnet=str(ipaddress.IPv4Address(subnet)),
        gateway=str(ipaddress.IPv4Address(gateway)),
        capabilities=capabilities,
        resident_version=resident_version,
        app_version=app_version,
        first_seen=now,
        last_seen=now,
    )


def format_mac(mac: bytes) -> str:
    return ":".join(f"{byte:02x}" for byte in mac)


# ANSI CSI sequences (SGR and cursor modes we emit / tolerate in pane lines).
_ANSI_ESCAPE_RE = re.compile(r"\x1b\[[\d;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    return _ANSI_ESCAPE_RE.sub("", text)


def visible_len(text: str) -> int:
    return len(strip_ansi(text))


SGR_RESET = "\033[0m"
SGR_DATA_GREEN = "\033[38;2;51;255;0m"  # #33ff00
SGR_CYAN = "\033[38;2;0;255;255m"  # bright cyan (#00ffff); ANSI 36 is often dark on some palettes
# 256-color amber/orange (falls back visually to yellow on limited palettes).
SGR_AMBER = "\033[38;5;214m"


def style_data(text: str) -> str:
    """Light green (#33ff00) for protocol/host/read values ("data")."""
    if text == "":
        return text
    return f"{SGR_DATA_GREEN}{text}{SGR_RESET}"


def style_input(text: str) -> str:
    """Cyan for user-editable draft values."""
    if text == "":
        return text
    return f"{SGR_CYAN}{text}{SGR_RESET}"


def style_amber(text: str) -> str:
    """Amber foreground for reboot-required attention."""
    if text == "":
        return text
    return f"{SGR_AMBER}{text}{SGR_RESET}"


def truncate(text: str, width: int) -> str:
    """Pad or truncate to exactly ``width`` visible columns; keep embedded ANSI sequences."""
    if width <= 0:
        return ""
    vl = visible_len(text)
    if vl <= width:
        return text + (" " * (width - vl))
    if width == 1:
        return "~"

    out: list[str] = []
    vis = 0
    i = 0
    limit = width - 1
    while i < len(text):
        if text[i] == "\x1b" and i + 1 < len(text) and text[i + 1] == "[":
            m = _ANSI_ESCAPE_RE.match(text, i)
            if m:
                out.append(m.group(0))
                i = m.end()
                continue
        if vis >= limit:
            break
        out.append(text[i])
        vis += 1
        i += 1

    truncated = "".join(out) + "~" + SGR_RESET
    pad = width - visible_len(truncated)
    return truncated + (" " * max(0, pad))


def format_age(seconds: float) -> str:
    if seconds < 1:
        return "<1s"
    if seconds < 60:
        return f"{int(seconds)}s"
    return f"{int(seconds // 60)}m"


def format_rtt(seconds: float | None) -> str:
    if seconds is None:
        return "unknown"
    if seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}us"
    if seconds < 1:
        return f"{seconds * 1000:.1f}ms"
    return f"{seconds:.3f}s"


def progress_bar(completed: int, total: int, width: int = 24) -> str:
    width = max(4, width)
    if total <= 0:
        return "[" + "." * width + "]"
    ratio = max(0.0, min(1.0, completed / total))
    filled = int(round(ratio * width))
    filled = max(0, min(width, filled))
    return "[" + ("#" * filled) + ("-" * (width - filled)) + "]"


def progress_line(label: str, completed: int, total: int, width: int, suffix: str = "") -> str:
    pct = 0 if total <= 0 else int(round((completed / total) * 100))
    detail = f" {completed}/{total} ({pct:3d}%)"
    if visible_len(detail) + 1 >= width:
        return truncate(detail.strip(), width).rstrip()

    min_bar = progress_bar(completed, total, 4)
    label_width = min(14, max(0, width - visible_len(detail) - visible_len(min_bar) - 4))
    line_label = label if visible_len(label) <= label_width else label[: max(0, label_width - 1)] + "~"
    prefix = f" {line_label:<{label_width}}: " if label_width > 0 else " "
    bar_width = max(4, width - visible_len(prefix) - visible_len(detail) - 2)
    bar = progress_bar(completed, total, bar_width)
    suffix_budget = width - visible_len(prefix) - visible_len(bar) - visible_len(detail)
    suffix_text = f" {suffix}"[:suffix_budget] if suffix_budget > 1 and suffix else ""
    return f"{prefix}{bar}{detail}{suffix_text}".rstrip()


class ProgrammingWorker:
    def __init__(
        self,
        bind_ip: str,
        target: str,
        fallback_target: str | None,
        device: Device,
        normal_rtt_s: float,
        program_image_path: str | None,
    ) -> None:
        self.bind_ip = bind_ip
        self.target = target
        self.fallback_target = fallback_target
        self.device = device
        self.normal_rtt_s = normal_rtt_s
        self.program_image_path = program_image_path
        self._lock = threading.Lock()
        self._snapshot = ProgrammingSnapshot(active=True)
        self._thread = threading.Thread(target=self._run, name="Programming", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def is_alive(self) -> bool:
        return self._thread.is_alive()

    def snapshot(self) -> ProgrammingSnapshot:
        with self._lock:
            return replace(self._snapshot)

    def _apply(self, update: ProgrammingUpdate) -> None:
        with self._lock:
            apply_programming_update(self._snapshot, update)

    def _finish(self, success: bool, result: str) -> None:
        with self._lock:
            self._snapshot.finished = True
            self._snapshot.success = success
            self._snapshot.result = result
            self._snapshot.active = False
            if success:
                self._snapshot.phase = "done"
            elif self._snapshot.phase in ("idle", "loading", "erasing", "ready", "connecting", "programming", "finalizing"):
                self._snapshot.phase = "failed"
            if not self._snapshot.status:
                self._snapshot.status = result

    def _run(self) -> None:
        try:
            ok, result = run_write_program(
                self.bind_ip,
                self.target,
                self.fallback_target,
                self.device,
                self.normal_rtt_s,
                on_response=lambda source_ip, elapsed_s, uid=self.device.uid: None,
                progress=self._apply,
                program_image_path=self.program_image_path,
            )
            self._finish(ok, result)
        except Exception as exc:  # pragma: no cover - safety net for UI thread
            self._apply(ProgrammingUpdate(phase="failed", status=str(exc)))
            self._finish(False, str(exc))


class BootPayloadWorker:
    def __init__(
        self,
        bind_ip: str,
        target: str,
        fallback_target: str | None,
        device: Device,
        normal_rtt_s: float,
        boot_payload_path: str | None,
    ) -> None:
        self.bind_ip = bind_ip
        self.target = target
        self.fallback_target = fallback_target
        self.device = device
        self.normal_rtt_s = normal_rtt_s
        self.boot_payload_path = boot_payload_path
        self._lock = threading.Lock()
        self._snapshot = ProgrammingSnapshot(active=True)
        self._thread = threading.Thread(target=self._run, name="BootPayload", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def snapshot(self) -> ProgrammingSnapshot:
        with self._lock:
            return replace(self._snapshot)

    def _apply(self, update: ProgrammingUpdate) -> None:
        with self._lock:
            apply_programming_update(self._snapshot, update)

    def _finish(self, success: bool, result: str) -> None:
        with self._lock:
            self._snapshot.finished = True
            self._snapshot.success = success
            self._snapshot.result = result
            self._snapshot.active = False
            if success:
                self._snapshot.phase = "done"
            elif self._snapshot.phase in ("idle", "loading", "erasing", "ready", "connecting", "programming", "finalizing"):
                self._snapshot.phase = "failed"
            if not self._snapshot.status:
                self._snapshot.status = result

    def _run(self) -> None:
        try:
            ok, result = run_write_boot_payload(
                self.bind_ip,
                self.target,
                self.fallback_target,
                self.device,
                self.normal_rtt_s,
                on_response=lambda source_ip, elapsed_s, uid=self.device.uid: None,
                progress=self._apply,
                boot_payload_path=self.boot_payload_path,
            )
            self._finish(ok, result)
        except Exception as exc:  # pragma: no cover
            self._apply(ProgrammingUpdate(phase="failed", status=str(exc)))
            self._finish(False, str(exc))


def metadata_string(metadata: dict, *keys: str) -> str:
    for key in keys:
        value = metadata.get(key)
        if value is not None:
            return str(value)
    return ""


def format_access(access: int, write_effect: str) -> str:
    labels: list[str] = []
    if effective_access(access) & ACCESS_READ:
        labels.append("Read")
    if effective_access(access) & ACCESS_WRITE:
        labels.append("Write")
    if write_effect.lower() in ("reboot_required", "requires_reboot", "requires-reboot"):
        labels.append("Requires Re-Boot")
    if effective_access(access) & ACCESS_EXECUTE:
        labels.append("Execute")

    return ", ".join(labels) if labels else "None"


def effective_access(access: int) -> int:
    return ((access >> 4) & 0xF) | (access & 0xF)


def can_read(item: TreeItem) -> bool:
    return (effective_access(item.access) & ACCESS_READ) != 0


def can_write(item: TreeItem) -> bool:
    return (effective_access(item.access) & ACCESS_WRITE) != 0


def can_execute(item: TreeItem) -> bool:
    return (effective_access(item.access) & ACCESS_EXECUTE) != 0


def is_standard_reboot_path(location: list[int]) -> bool:
    return location == STANDARD_REBOOT_PATH


def is_implemented_in_device_menu(location: list[int], item: TreeItem) -> bool:
    return location + [item.node_id] == STANDARD_REBOOT_PATH


def ipv4_prefix_for_interface(interface: NetworkInterface | None) -> str:
    if interface is None or not interface.subnet:
        return ""

    try:
        network = ipaddress.IPv4Network(f"{interface.ip}/{interface.subnet}", strict=False)
    except ipaddress.AddressValueError:
        return ""

    prefix_octets = int(network.prefixlen // 8)
    if prefix_octets <= 0:
        return ""

    return ".".join(str(octet) for octet in network.network_address.packed[:prefix_octets]) + "."


def edit_default_value(path_names: list[str], selected_interface: NetworkInterface | None) -> str:
    if path_names[-2:] == ["Network", "Subnet"] and selected_interface is not None:
        return selected_interface.subnet
    if path_names[-2:] in (["Network", "Address"], ["Network", "Gateway"]):
        return ipv4_prefix_for_interface(selected_interface)
    return ""


def ip_in_subnet(ip: str, network_ip: str, subnet: str) -> bool:
    if not subnet:
        return False

    try:
        network = ipaddress.IPv4Network(f"{network_ip}/{subnet}", strict=False)
        address = ipaddress.IPv4Address(ip)
    except (ipaddress.AddressValueError, ipaddress.NetmaskValueError):
        return False

    return address in network


def device_ipv4_duplicated_among_known(device: Device, known_devices: list[Device]) -> bool:
    """True if two or more discovered devices report the same configured IPv4."""
    return sum(1 for d in known_devices if d.ip == device.ip) > 1


def device_control_target(
    device: Device,
    bind_ip: str,
    selected_interface: NetworkInterface | None,
    fallback_target: str,
    known_devices: list[Device],
    *,
    broadcast_when_adapter_ip_matches_device: bool,
) -> str:
    """Pick UDP destination for BCAST_UID-wrapped control packets.

    When unicast to ``device.ip`` is ambiguous—local bind equals that IP, or multiple
    discovered devices share it—optional broadcast uses ``fallback_target`` (e.g.
    255.255.255.255) while still selecting the peer via UID in the packet body.
    """
    ambiguous_unicast = bind_ip == device.ip or device_ipv4_duplicated_among_known(device, known_devices)
    if broadcast_when_adapter_ip_matches_device and ambiguous_unicast:
        return fallback_target
    if selected_interface is not None and ip_in_subnet(device.ip, selected_interface.ip, selected_interface.subnet):
        return device.ip
    return fallback_target


def build_device_tree_request(op: int, location: list[int], op_payload: bytes = b"") -> bytes:
    return bytes([op, len(location), *location]) + struct.pack("<H", len(op_payload)) + op_payload


def _ignore_udp_recv_oserror(exc: OSError) -> bool:
    """Windows: ICMP port/host unreachable for prior send -> WSAECONNRESET (10054) on recvfrom."""
    if getattr(exc, "winerror", None) == 10054:
        return True
    if exc.errno in (errno.ECONNRESET, errno.EHOSTUNREACH, errno.ENETUNREACH):
        return True
    return False


def send_device_tree_request_with_reply_op(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    op: int,
    location: list[int],
    op_payload: bytes = b"",
    timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, int, bytes, str]:
    tree_payload = build_device_tree_request(op, location, op_payload)
    inner = bytes([PROTO_MSG_DEVICETREE_REQ]) + struct.pack("<H", len(tree_payload)) + tree_payload
    payload = uid + inner
    transaction_id = random.SystemRandom().randrange(0, 0x1_0000_0000)

    def attempt(dst: str, wait_s: float) -> tuple[bool, int, bytes, str]:
        packet = (
            COMMAND_HEADER.pack(
                PROTO_MAGIC,
                PROTO_VERSION,
                PROTO_MSG_BCAST_UID_REQ,
                0,
                transaction_id,
                len(payload),
                0,
            )
            + payload
        )

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((bind_ip, 0))
            sock.settimeout(0.05)
            started_at = time.monotonic()
            rtt_started_at_ns = time.perf_counter_ns()
            sock.sendto(packet, (dst, CONTROL_PORT))

            deadline = started_at + wait_s
            while time.monotonic() < deadline:
                try:
                    data, source = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError as exc:
                    if _ignore_udp_recv_oserror(exc):
                        continue
                    raise

                if len(data) < COMMAND_HEADER.size:
                    continue
                magic, version, msg_type, _flags, rx_tx, payload_len, _reserved = COMMAND_HEADER.unpack_from(data)
                if (
                    magic != PROTO_MAGIC
                    or version != PROTO_VERSION
                    or msg_type != PROTO_MSG_BCAST_UID_REPLY
                    or rx_tx != transaction_id
                ):
                    continue

                reply_payload = data[COMMAND_HEADER.size : COMMAND_HEADER.size + payload_len]
                if len(reply_payload) < 15 or reply_payload[:12] != uid:
                    continue

                elapsed_s = max((time.perf_counter_ns() - rtt_started_at_ns) / 1_000_000_000, 0.000001)
                if on_response is not None:
                    on_response(source[0], elapsed_s)

                inner_type = reply_payload[12]
                inner_len = struct.unpack_from("<H", reply_payload, 13)[0]
                inner_payload = reply_payload[15 : 15 + inner_len]
                if inner_type == PROTO_MSG_ERROR_REPLY:
                    result, detail = struct.unpack_from("<hH", inner_payload)
                    return False, 0, b"", f"error {result} detail {detail}"
                if inner_type != PROTO_MSG_DEVICETREE_REPLY or len(inner_payload) < 6:
                    return False, 0, b"", f"unexpected reply 0x{inner_type:02x}"

                reply_op = inner_payload[0]
                depth = inner_payload[1]
                result_offset = 2 + depth
                result, response_len = struct.unpack_from("<hH", inner_payload, result_offset)
                response = inner_payload[result_offset + 4 : result_offset + 4 + response_len]
                if result != 0:
                    return False, reply_op, b"", f"request failed result {result}"
                return True, reply_op, response, "ok"

        return False, 0, b"", "request timed out"

    max_retry_s = max(min(timeout_s, MAX_CONTROL_RETRY_S), MIN_CONTROL_RETRY_S)
    retry_s = min(max(normal_rtt_s * 2.0, MIN_CONTROL_RETRY_S), max_retry_s)
    while retry_s <= max_retry_s:
        ok, reply_op, response, message = attempt(target, retry_s)
        if ok:
            return ok, reply_op, response, message
        if message != "request timed out":
            return ok, reply_op, response, message
        retry_s *= 2.0

    if fallback_target and fallback_target != target:
        ok2, reply_op2, response2, message2 = attempt(fallback_target, MAX_CONTROL_RETRY_S)
        if ok2:
            return ok2, reply_op2, response2, "ok (broadcast fallback)"
        return ok2, reply_op2, response2, message2

    return False, 0, b"", "request timed out"


def send_device_tree_request(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    op: int,
    location: list[int],
    op_payload: bytes = b"",
    timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, bytes, str]:
    ok, _reply_op, response, message = send_device_tree_request_with_reply_op(
        bind_ip,
        target,
        fallback_target,
        uid,
        op,
        location,
        op_payload,
        timeout_s=timeout_s,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    return ok, response, message


def set_device_tree_value(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    location: list[int],
    value: str,
    timeout_s: float = DEFAULT_WRITE_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, str, bool]:
    encoded = value.encode("utf-8")
    ok, response, message = send_device_tree_request(
        bind_ip,
        target,
        fallback_target,
        uid,
        DEVICE_TREE_OP_SET,
        location,
        struct.pack("<H", len(encoded)) + encoded,
        timeout_s=timeout_s,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    if not ok:
        return False, message, False

    reboot_required = bool(response and response[0] != 0)
    return True, "reboot required" if reboot_required else "applied", reboot_required


def execute_device_tree_node(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    location: list[int],
    args: str = "",
    timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, str]:
    encoded = args.encode("utf-8")
    ok, _response, message = send_device_tree_request(
        bind_ip,
        target,
        fallback_target,
        uid,
        DEVICE_TREE_OP_EXECUTE,
        location,
        struct.pack("<H", len(encoded)) + encoded,
        timeout_s=timeout_s,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    return ok, "executed" if ok else message


def list_device_tree_node(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    location: list[int],
    timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, list[TreeItem], str]:
    items: list[TreeItem] = []
    start_after = 0
    page = 0

    while True:
        op_payload = bytes([start_after]) if page > 0 else b""
        ok, reply_op, response, message = send_device_tree_request_with_reply_op(
            bind_ip,
            target,
            fallback_target,
            uid,
            DEVICE_TREE_OP_LIST | (DEVICE_TREE_OP_PAGING if page > 0 else 0),
            location,
            op_payload,
            timeout_s=timeout_s,
            normal_rtt_s=normal_rtt_s,
            on_response=on_response,
        )
        if not ok:
            return False, items, message
        if len(response) < 2:
            return False, items, "short LIST reply"

        count = struct.unpack_from("<H", response, 0)[0]
        offset = 2
        page_items: list[TreeItem] = []
        for _ in range(count):
            if offset + 5 > len(response):
                return False, items, "truncated LIST item"
            node_id = response[offset]
            has_children = response[offset + 1] != 0
            access = response[offset + 2]
            data_len = struct.unpack_from("<H", response, offset + 3)[0]
            data_start = offset + 5
            data_end = data_start + data_len
            if data_end > len(response):
                return False, items, "truncated LIST metadata"
            metadata = json.loads(response[data_start:data_end].decode("utf-8"))
            page_items.append(
                TreeItem(
                    node_id=node_id,
                    has_children=has_children,
                    access=access,
                    name=metadata_string(metadata, "Name", "name") or f"Node {node_id}",
                    value=metadata_string(metadata, "Value", "value"),
                    write_effect=metadata_string(metadata, "WriteEffect", "write_effect"),
                    description=metadata_string(metadata, "Description", "description"),
                    action=bool(metadata.get("Action", metadata.get("action", False))),
                    unit=metadata_string(metadata, "Unit", "unit"),
                    enum_values=[
                        str(item)
                        for item in metadata.get("Enum", metadata.get("enum", []))
                        if isinstance(item, str)
                    ],
                )
            )
            offset = data_end

        items.extend(page_items)
        has_more = (reply_op & DEVICE_TREE_OP_PAGING) != 0
        if not has_more:
            return True, items, "ok"
        if not page_items:
            return False, items, "paged LIST made no progress"
        start_after = page_items[-1].node_id
        page += 1
        if page > 255:
            return False, items, "paged LIST exceeded iteration cap"


def get_device_tree_value(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    uid: bytes,
    location: list[int],
    timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    normal_rtt_s: float = DEFAULT_CONTROL_RTT_S,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, str, str]:
    ok, response, message = send_device_tree_request(
        bind_ip,
        target,
        fallback_target,
        uid,
        DEVICE_TREE_OP_GET,
        location,
        timeout_s=timeout_s,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    if not ok:
        return False, "", message
    if len(response) < 2:
        return False, "", "short GET reply"

    value_len = struct.unpack_from("<H", response, 0)[0]
    if len(response) < 2 + value_len:
        return False, "", "truncated GET value"
    return True, response[2 : 2 + value_len].decode("utf-8", errors="replace"), "ok"


def load_intel_hex(path: str) -> bytes:
    segments: dict[int, int] = {}
    upper = 0
    min_addr: int | None = None
    max_addr = 0
    with open(path, "r", encoding="ascii") as handle:
        for line_no, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError(f"line {line_no}: missing ':'")
            record = bytes.fromhex(line[1:])
            if len(record) < 5:
                raise ValueError(f"line {line_no}: short record")
            length = record[0]
            address = (record[1] << 8) | record[2]
            record_type = record[3]
            data = record[4 : 4 + length]
            checksum = sum(record) & 0xFF
            if checksum != 0:
                raise ValueError(f"line {line_no}: bad checksum")
            if record_type == 0x00:
                absolute = upper + address
                for offset, byte in enumerate(data):
                    segments[absolute + offset] = byte
                min_addr = absolute if min_addr is None else min(min_addr, absolute)
                max_addr = max(max_addr, absolute + len(data))
            elif record_type == 0x01:
                break
            elif record_type == 0x04:
                if length != 2:
                    raise ValueError(f"line {line_no}: bad extended linear address")
                upper = ((data[0] << 8) | data[1]) << 16
            else:
                continue

    if min_addr is None:
        return b""
    return bytes(segments.get(address, 0xFF) for address in range(min_addr, max_addr))


def pack_app_image(payload: bytes, *, version: int = 1, entry_offset: int = 0, stop_offset: int = 0,
                   bss_offset: int | None = None, bss_size: int = 0) -> bytes:
    digest = hashlib.sha1(payload).digest()
    if bss_offset is None:
        bss_offset = len(payload)
    header = struct.pack(
        APP_IMAGE_HEADER_FORMAT,
        APP_IMAGE_MAGIC,
        APP_IMAGE_HEADER_SIZE,
        APP_IMAGE_ABI_VERSION,
        len(payload),
        APP_EXEC_BASE,
        APP_EXEC_SIZE,
        entry_offset,
        stop_offset,
        bss_offset,
        bss_size,
        version,
        APP_IMAGE_FLAG_VALID,
        APP_IMAGE_DIGEST_SHA1,
        b"\x00\x00\x00",
        digest,
    )
    return header + payload


def build_stress_program_image() -> bytes:
    payload_len = 60 * 1024
    pattern = bytes(range(256))
    payload = (pattern * ((payload_len + len(pattern) - 1) // len(pattern)))[:payload_len]
    return pack_app_image(payload)


def resolve_program_image_path(explicit_path: str | None) -> str:
    if explicit_path:
        return explicit_path

    for candidate in PROGRAM_IMAGE_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate

    return ""


def resolve_boot_payload_path(explicit_path: str | None) -> str:
    if explicit_path:
        return explicit_path
    for candidate in BOOT_PAYLOAD_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    return ""


def build_boot_payload_blob(image: bytes) -> bytes:
    if not image:
        raise ValueError("boot payload image is empty")
    max_image = BOOT_PAYLOAD_SLOT_SIZE_BYTES - BOOT_PAYLOAD_HEADER.size
    if len(image) > max_image:
        raise ValueError(f"boot payload image {len(image)} bytes exceeds {max_image} byte max")
    digest = hashlib.sha1(image).digest()
    return BOOT_PAYLOAD_HEADER.pack(BOOT_PAYLOAD_HEADER_MAGIC, len(image), digest, 0) + image


def load_boot_payload_image(source_path: str | None) -> tuple[bytes, str]:
    path = resolve_boot_payload_path(source_path)
    if not path:
        raise ValueError("no boot payload image found (pass --boot-payload or build Bootloader.bin)")
    if not os.path.isfile(path):
        raise ValueError(f"boot payload file not found: {path}")
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) >= BOOT_PAYLOAD_HEADER.size:
        magic, _image_length, _sha1, _reserved = BOOT_PAYLOAD_HEADER.unpack_from(data)
        if magic == BOOT_PAYLOAD_HEADER_MAGIC:
            return data, os.path.basename(path)
    return build_boot_payload_blob(data), os.path.basename(path)


def load_program_image(source_path: str | None) -> tuple[bytes, str, bool]:
    if not source_path:
        return build_stress_program_image(), "generated stress image", False

    if not os.path.isfile(source_path):
        return build_stress_program_image(), "generated stress image", False

    ext = os.path.splitext(source_path)[1].lower()
    if ext == ".hex":
        return load_intel_hex(source_path), os.path.basename(source_path), False
    if ext == ".bin":
        with open(source_path, "rb") as handle:
            return handle.read(), os.path.basename(source_path), True
    if ext == ".elf":
        objcopy = shutil.which("arm-none-eabi-objcopy") or shutil.which("objcopy")
        nm = shutil.which("arm-none-eabi-nm") or shutil.which("nm")
        if objcopy is None or nm is None:
            raise ValueError("ELF packaging requires arm-none-eabi-objcopy and arm-none-eabi-nm")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_bin = os.path.join(temp_dir, "payload.bin")
            subprocess.run([objcopy, "-O", "binary", source_path, temp_bin], check=True, capture_output=True)
            symbols = read_symbols(nm, source_path)
            app_start = symbols.get("app_start")
            if app_start is None:
                return build_stress_program_image(), "generated stress image", False
            app_stop = symbols.get("app_stop", 0)
            bss_start = symbols.get("__app_bss_start__", APP_EXEC_BASE)
            bss_end = symbols.get("__app_bss_end__", bss_start)
            with open(temp_bin, "rb") as handle:
                payload = handle.read()
            return (
                pack_app_image(
                    payload,
                    entry_offset=app_start - APP_EXEC_BASE,
                    stop_offset=(app_stop - APP_EXEC_BASE) if app_stop else 0,
                    bss_offset=bss_start - APP_EXEC_BASE,
                    bss_size=bss_end - bss_start,
                ),
                os.path.basename(source_path),
                False,
            )

    raise ValueError(f"unsupported program image format: {source_path}")


def read_symbols(nm: str, elf: str) -> dict[str, int]:
    output = subprocess.check_output([nm, "-g", elf], text=True)
    symbols: dict[str, int] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    return symbols


def make_prog_frame(frame_type: int, seq: int, payload: bytes) -> bytes:
    return make_prog_frame_with_flags(frame_type, seq, payload, 0)


def make_prog_frame_with_flags(frame_type: int, seq: int, payload: bytes, flags: int) -> bytes:
    return PROG_HEADER.pack(
        PROTO_MAGIC,
        PROTO_VERSION,
        frame_type,
        flags,
        seq,
        len(payload),
        0,
    ) + payload


def recv_exact(sock: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise ConnectionError("TCP connection closed")
        data.extend(chunk)
    return bytes(data)


def recv_prog_reply(sock: socket.socket, expected_seq: int | None = None) -> tuple[bool, str, int, int]:
    header_bytes = recv_exact(sock, PROG_HEADER.size)
    magic, version, frame_type, flags, seq, payload_len, _reserved = PROG_HEADER.unpack(header_bytes)
    payload = recv_exact(sock, payload_len)
    if magic != PROTO_MAGIC or version != PROTO_VERSION or (expected_seq is not None and seq != expected_seq):
        return False, "invalid TCP reply header", 0, seq
    if (flags & PROTO_FLAG_REPLY) == 0:
        return False, "TCP reply missing reply flag", 0, seq
    if frame_type == PROG_FRAME_ACK:
        if len(payload) < PROG_ACK_PAYLOAD.size:
            return False, "short ACK payload", 0, seq
        ack_seq, bytes_written = PROG_ACK_PAYLOAD.unpack_from(payload)
        if expected_seq is not None and ack_seq != expected_seq:
            return False, f"ACK sequence mismatch {ack_seq}", bytes_written, seq
        return True, "ack", bytes_written, ack_seq
    if frame_type == PROG_FRAME_NACK:
        if len(payload) < PROG_NACK_PAYLOAD.size:
            return False, "short NACK payload", 0, seq
        nack_seq, error_code, detail = PROG_NACK_PAYLOAD.unpack_from(payload)
        return False, f"NACK seq {nack_seq} error {error_code} detail {detail}", 0, nack_seq
    return False, f"unexpected TCP frame 0x{frame_type:02x}", 0, seq


def apply_programming_update(snapshot: ProgrammingSnapshot, update: ProgrammingUpdate) -> None:
    if update.phase is not None:
        snapshot.phase = update.phase
    if update.status is not None:
        snapshot.status = update.status
    if update.payload_name is not None:
        snapshot.payload_name = update.payload_name
    if update.image_bytes is not None:
        snapshot.image_bytes = update.image_bytes
    if update.total_blocks is not None:
        snapshot.total_blocks = update.total_blocks
    if update.sent_blocks is not None:
        snapshot.sent_blocks = update.sent_blocks
    if update.in_flight_blocks is not None:
        snapshot.in_flight_blocks = update.in_flight_blocks
    if update.acked_blocks is not None:
        snapshot.acked_blocks = update.acked_blocks
    if update.acked_bytes is not None:
        snapshot.acked_bytes = update.acked_bytes


def stream_program_tcp(
    host: str,
    port: int,
    image: bytes,
    raw_storage: bool = False,
    write_base_offset: int = 0,
    progress: Callable[[ProgrammingUpdate], None] | None = None,
) -> tuple[bool, str]:
    digest = hashlib.sha1(image).digest()
    seq = 0
    try:
        with socket.create_connection((host, port), timeout=PROGRAM_TCP_TIMEOUT_S) as sock:
            sock.settimeout(PROGRAM_TCP_TIMEOUT_S)
            total_blocks = max(1, (len(image) + PROGRAM_BLOCK_BYTES - 1) // PROGRAM_BLOCK_BYTES)
            if progress is not None:
                progress(
                    ProgrammingUpdate(
                        phase="programming",
                        image_bytes=len(image),
                        total_blocks=total_blocks,
                        acked_blocks=0,
                        acked_bytes=0,
                        status="connected, sending HELLO",
                    )
                )
            program_flags = PROG_FLAG_RAW_STORAGE if raw_storage else 0
            if raw_storage:
                hello_payload = PROG_HELLO_RAW_PAYLOAD.pack(len(image), digest, write_base_offset)
            else:
                hello_payload = PROG_HELLO_PAYLOAD.pack(len(image), digest)
            sock.sendall(make_prog_frame_with_flags(PROG_FRAME_HELLO, seq, hello_payload, program_flags))
            ok, message, _bytes_written, _reply_seq = recv_prog_reply(sock, seq)
            if not ok:
                return False, message
            seq += 1

            blocks = [image[offset : offset + PROGRAM_BLOCK_BYTES] for offset in range(0, len(image), PROGRAM_BLOCK_BYTES)]
            next_block = 0
            acked_blocks = 0
            bytes_written = 0
            in_flight: dict[int, tuple[int, bytes, int]] = {}
            seq_to_block: dict[int, int] = {}

            while acked_blocks < len(blocks):
                while next_block < len(blocks) and len(in_flight) < PROGRAM_WINDOW_FRAMES:
                    block = blocks[next_block]
                    payload = PROG_DATA_LEN.pack(len(block)) + block
                    frame = make_prog_frame(PROG_FRAME_DATA, seq, payload)
                    sock.sendall(frame)
                    in_flight[seq] = (next_block, frame, 1)
                    seq_to_block[seq] = next_block
                    next_block += 1
                    seq += 1

                if progress is not None:
                    progress(
                        ProgrammingUpdate(
                            phase="programming",
                            status=f"sent {next_block}/{total_blocks}, waiting for ACKs",
                            image_bytes=len(image),
                            total_blocks=total_blocks,
                            sent_blocks=next_block,
                            in_flight_blocks=len(in_flight),
                            acked_blocks=acked_blocks,
                            acked_bytes=bytes_written,
                        )
                    )

                ok, message, reply_bytes_written, reply_seq = recv_prog_reply(sock, None)
                block_entry = in_flight.get(reply_seq)
                if block_entry is None:
                    return False, f"{message}; unexpected reply seq {reply_seq}"

                block_index, frame, attempts = block_entry
                if ok:
                    in_flight.pop(reply_seq, None)
                    bytes_written = reply_bytes_written
                    acked_blocks += 1
                    if progress is not None:
                        progress(
                            ProgrammingUpdate(
                                phase="programming",
                                status=f"ack {acked_blocks}/{total_blocks}",
                                image_bytes=len(image),
                                total_blocks=total_blocks,
                                sent_blocks=next_block,
                                in_flight_blocks=len(in_flight),
                                acked_blocks=acked_blocks,
                                acked_bytes=bytes_written,
                            )
                        )
                    continue

                if "NACK" not in message:
                    return False, message

                if attempts >= PROGRAM_BLOCK_MAX_RETRIES:
                    return False, f"{message}; giving up on block {block_index + 1}"

                sock.sendall(frame)
                in_flight[reply_seq] = (block_index, frame, attempts + 1)
                if progress is not None:
                    progress(
                        ProgrammingUpdate(
                            phase="programming",
                            status=f"{message}; retransmitting block {block_index + 1}",
                            image_bytes=len(image),
                            total_blocks=total_blocks,
                            sent_blocks=next_block,
                            in_flight_blocks=len(in_flight),
                            acked_blocks=acked_blocks,
                            acked_bytes=bytes_written,
                        )
                    )

            if progress is not None:
                progress(
                    ProgrammingUpdate(
                        phase="finalizing",
                        status="sending FINISH",
                        image_bytes=len(image),
                        total_blocks=total_blocks,
                        sent_blocks=next_block,
                        in_flight_blocks=0,
                        acked_blocks=acked_blocks,
                        acked_bytes=bytes_written,
                    )
                )
            sock.sendall(make_prog_frame_with_flags(PROG_FRAME_FINISH, seq, PROG_FINISH_PAYLOAD.pack(len(image), digest), program_flags))
            ok, message, bytes_written, _reply_seq = recv_prog_reply(sock, seq)
            if not ok:
                return False, message
            return True, f"programmed {bytes_written} bytes"
    except (OSError, ConnectionError, TimeoutError) as exc:
        return False, f"TCP programming failed: {exc}"


def wait_for_programming_port(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, int, str]:
    deadline = time.monotonic() + PROGRAM_READY_TIMEOUT_S
    while time.monotonic() < deadline:
        ok, value, message = get_device_tree_value(
            bind_ip,
            target,
            fallback_target,
            device.uid,
            PROGRAM_TCP_PORT_PATH,
            timeout_s=DEFAULT_REQUEST_TIMEOUT_S,
            normal_rtt_s=normal_rtt_s,
            on_response=on_response,
        )
        if ok:
            try:
                port = int(value)
            except ValueError:
                return False, -1, f"invalid programming port {value!r}"
            if port > 0:
                return True, port, "ok"
        elif message != "request timed out":
            return False, -1, message
        time.sleep(0.25)
    return False, -1, "timed out waiting for programming TCP port"


def wait_for_programming_ready(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, str]:
    deadline = time.monotonic() + PROGRAM_READY_TIMEOUT_S
    while time.monotonic() < deadline:
        ok, value, message = get_device_tree_value(
            bind_ip,
            target,
            fallback_target,
            device.uid,
            PROGRAM_STATE_PATH,
            timeout_s=DEFAULT_REQUEST_TIMEOUT_S,
            normal_rtt_s=normal_rtt_s,
            on_response=on_response,
        )
        if ok:
            if value == PROGRAM_READY_STATE:
                return True, "ok"
        elif message != "request timed out":
            return False, message
        time.sleep(0.25)
    return False, "timed out waiting for programming ready"


def run_write_program_image(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    image: bytes,
    payload_name: str,
    raw_storage: bool,
    on_response: Callable[[str, float], None] | None = None,
    progress: Callable[[ProgrammingUpdate], None] | None = None,
    write_base_offset: int = 0,
) -> tuple[bool, str]:
    if not image:
        return False, "programming image produced no bytes"
    if write_base_offset < 0 or write_base_offset > APP_STORE_SIZE_BYTES:
        return False, "invalid write base offset"
    if len(image) > (APP_STORE_SIZE_BYTES - write_base_offset):
        return False, f"programming image {len(image)} bytes exceeds remaining app store bytes"

    if progress is not None:
        progress(
            ProgrammingUpdate(
                phase="loading",
                status="raw storage image loaded" if raw_storage else "image loaded",
                payload_name=payload_name,
                image_bytes=len(image),
                total_blocks=max(1, (len(image) + PROGRAM_BLOCK_BYTES - 1) // PROGRAM_BLOCK_BYTES),
                acked_blocks=0,
                acked_bytes=0,
            )
        )

    skip_erase = False
    state_ok, current_state, _state_message = get_device_tree_value(
        bind_ip,
        target,
        fallback_target,
        device.uid,
        PROGRAM_STATE_PATH,
        timeout_s=DEFAULT_REQUEST_TIMEOUT_S,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    if state_ok and current_state == PROGRAM_READY_STATE:
        skip_erase = True
        if progress is not None:
            progress(
                ProgrammingUpdate(
                    phase="ready",
                    status="already programming ready, skipping erase",
                    payload_name=payload_name,
                    image_bytes=len(image),
                )
            )

    if not skip_erase:
        ok, message, _reboot_required = set_device_tree_value(
            bind_ip,
            target,
            fallback_target,
            device.uid,
            PROGRAM_STATE_PATH,
            PROGRAM_ERASING_STATE,
            timeout_s=PROGRAM_READY_TIMEOUT_S,
            normal_rtt_s=normal_rtt_s,
            on_response=on_response,
        )
        if not ok:
            return False, f"failed to enter programming mode: {message}"
        if progress is not None:
            progress(
                ProgrammingUpdate(
                    phase="erasing",
                    status="erase queued, waiting for programming ready",
                    payload_name=payload_name,
                    image_bytes=len(image),
                )
            )

    ok, message = wait_for_programming_ready(
        bind_ip,
        target,
        fallback_target,
        device,
        normal_rtt_s,
        on_response=on_response,
    )
    if not ok:
        return False, message
    if progress is not None:
        progress(
            ProgrammingUpdate(
                phase="ready",
                status="programming ready, waiting for TCP port",
                payload_name=payload_name,
                image_bytes=len(image),
            )
        )

    ok, port, message = wait_for_programming_port(
        bind_ip,
        target,
        fallback_target,
        device,
        normal_rtt_s,
        on_response=on_response,
    )
    if not ok:
        return False, message
    if progress is not None:
        progress(
            ProgrammingUpdate(
                phase="connecting",
                status=f"connecting to TCP port {port}",
                payload_name=payload_name,
                image_bytes=len(image),
            )
        )

    ok, message = stream_program_tcp(
        device.ip,
        port,
        image,
        raw_storage=raw_storage,
        write_base_offset=write_base_offset,
        progress=progress,
    )
    if not ok:
        return False, message
    return True, f"{message}; state should now be {PROGRAM_STOPPED_STATE}"


def run_write_program(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    on_response: Callable[[str, float], None] | None = None,
    progress: Callable[[ProgrammingUpdate], None] | None = None,
    program_image_path: str | None = None,
    write_base_offset: int = 0,
) -> tuple[bool, str]:
    try:
        image, payload_name, is_raw = load_program_image(program_image_path)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        return False, f"failed to load programming image: {exc}"
    return run_write_program_image(
        bind_ip,
        target,
        fallback_target,
        device,
        normal_rtt_s,
        image,
        payload_name,
        is_raw,
        on_response=on_response,
        progress=progress,
        write_base_offset=write_base_offset,
    )


def run_write_boot_payload(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    on_response: Callable[[str, float], None] | None = None,
    progress: Callable[[ProgrammingUpdate], None] | None = None,
    boot_payload_path: str | None = None,
) -> tuple[bool, str]:
    image, payload_name = load_boot_payload_image(boot_payload_path)
    if progress is not None:
        progress(
            ProgrammingUpdate(
                phase="loading",
                status="boot payload loaded",
                payload_name=payload_name,
                image_bytes=len(image),
                total_blocks=max(1, (len(image) + PROGRAM_BLOCK_BYTES - 1) // PROGRAM_BLOCK_BYTES),
                acked_blocks=0,
                acked_bytes=0,
            )
        )
    ok, message = run_write_program_image(
        bind_ip,
        target,
        fallback_target,
        device,
        normal_rtt_s,
        image,
        payload_name,
        True,
        on_response=on_response,
        progress=progress,
        write_base_offset=BOOT_PAYLOAD_BASE_OFFSET,
    )
    if not ok:
        return False, message
    return True, f"{message}; boot payload offset 0x{BOOT_PAYLOAD_BASE_OFFSET:X}"


def run_apply_boot_update(
    bind_ip: str,
    target: str,
    fallback_target: str | None,
    device: Device,
    normal_rtt_s: float,
    on_response: Callable[[str, float], None] | None = None,
) -> tuple[bool, str]:
    return execute_device_tree_node(
        bind_ip,
        target,
        fallback_target,
        device.uid,
        PROGRAM_APPLY_BOOT_UPDATE_PATH,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )


class DiscoveryWorker:
    def __init__(self, bind_ip: str, target: str, port: int, refresh_s: float) -> None:
        self.bind_ip = bind_ip
        self.target = target
        self.port = port
        self.refresh_s = max(refresh_s, 0.25)
        self._devices: dict[bytes, Device] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="Discovery", daemon=True)
        self._last_error = ""
        self._last_tx = 0
        self._enabled = True

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)

    def refresh_now(self) -> None:
        self._last_tx = 0

    def set_enabled(self, enabled: bool) -> None:
        with self._lock:
            self._enabled = enabled
            if enabled:
                self._last_tx = 0

    def snapshot(self) -> tuple[list[Device], str]:
        with self._lock:
            devices = sorted(self._devices.values(), key=lambda item: (item.ip, format_mac(item.mac)))
            return devices, self._last_error

    def mark_seen(self, uid: bytes) -> None:
        with self._lock:
            device = self._devices.get(uid)
            if device is not None:
                device.last_seen = time.monotonic()

    def patch_device_network(
        self,
        uid: bytes,
        *,
        ip: str | None = None,
        subnet: str | None = None,
        gateway: str | None = None,
    ) -> None:
        """Update cached discovery fields after a local SET (device RX address may change immediately)."""
        with self._lock:
            device = self._devices.get(uid)
            if device is None:
                return
            new_ip = ip.strip() if ip is not None else device.ip
            new_subnet = subnet.strip() if subnet is not None else device.subnet
            new_gateway = gateway.strip() if gateway is not None else device.gateway
            new_source: tuple[str, int] = (
                (new_ip, device.source[1]) if ip is not None else device.source
            )
            self._devices[uid] = replace(
                device,
                ip=new_ip,
                subnet=new_subnet,
                gateway=new_gateway,
                source=new_source,
                last_seen=time.monotonic(),
            )

    def _store_device(self, device: Device) -> None:
        key = device.uid if any(device.uid) else f"{device.source[0]}:{format_mac(device.mac)}".encode()
        with self._lock:
            existing = self._devices.get(key)
            if existing is not None:
                device.first_seen = existing.first_seen
            self._devices[key] = device

    def _set_error(self, message: str) -> None:
        with self._lock:
            self._last_error = message

    def _run(self) -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind((self.bind_ip, 0))
                sock.settimeout(0.05)

                while not self._stop.is_set():
                    now = time.monotonic()
                    with self._lock:
                        enabled = self._enabled
                    if enabled and (self._last_tx == 0 or (now - self._last_tx) >= self.refresh_s):
                        transaction_id = random.SystemRandom().randrange(0, 0x1_0000_0000)
                        request = make_discovery_request(transaction_id)
                        sock.sendto(request, (self.target, self.port))
                        self._last_tx = now
                        self._set_error("")

                    try:
                        data, source = sock.recvfrom(2048)
                    except TimeoutError:
                        continue
                    except socket.timeout:
                        continue
                    except OSError as exc:
                        if _ignore_udp_recv_oserror(exc):
                            continue
                        raise

                    device = parse_discovery_reply(data, source)
                    if device is not None:
                        self._store_device(device)
        except OSError as exc:
            self._set_error(str(exc))


class Terminal:
    def __enter__(self) -> "Terminal":
        enable_virtual_terminal()
        sys.stdout.write("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H")
        sys.stdout.flush()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:  # type: ignore[no-untyped-def]
        sys.stdout.write("\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()

    def render(self, text: str) -> None:
        sys.stdout.write("\x1b[H\x1b[0m" + text)
        sys.stdout.flush()


def enable_virtual_terminal() -> None:
    if os.name != "nt":
        return

    kernel32 = ctypes.windll.kernel32
    handle = kernel32.GetStdHandle(-11)
    mode = ctypes.c_uint32()
    if kernel32.GetConsoleMode(handle, ctypes.byref(mode)) == 0:
        return
    kernel32.SetConsoleMode(handle, mode.value | 0x0004)


class KeyReader:
    def __enter__(self) -> "KeyReader":
        if os.name == "nt":
            import msvcrt

            self._msvcrt = msvcrt
        else:
            import termios
            import tty

            self._termios = termios
            self._fd = sys.stdin.fileno()
            self._old_settings = termios.tcgetattr(self._fd)
            tty.setcbreak(self._fd)
        return self

    def __exit__(self, exc_type, exc, tb) -> None:  # type: ignore[no-untyped-def]
        if os.name != "nt":
            self._termios.tcsetattr(self._fd, self._termios.TCSADRAIN, self._old_settings)

    def read_key(self) -> str | None:
        if os.name == "nt":
            if not self._msvcrt.kbhit():
                return None
            char = self._msvcrt.getwch()
            if char in ("\x00", "\xe0"):
                code = self._msvcrt.getwch()
                return {"H": "up", "P": "down", "K": "left", "M": "right"}.get(code)
            if char in ("\r", "\n"):
                return "enter"
            if char == "\x08":
                return "backspace"
            return char.lower()

        import select

        readable, _, _ = select.select([sys.stdin], [], [], 0)
        if not readable:
            return None

        char = sys.stdin.read(1)
        if char == "\x1b":
            readable, _, _ = select.select([sys.stdin], [], [], 0.01)
            if not readable:
                return "\x1b"
            tail = sys.stdin.read(2)
            return {"[A": "up", "[B": "down", "[D": "left", "[C": "right"}.get(tail, "\x1b")
        if char in ("\r", "\n"):
            return "enter"
        if char in ("\x7f", "\b"):
            return "backspace"
        return char.lower()


def build_screen(
    network_interfaces: list[NetworkInterface],
    selected_interface_index: int,
    selected_interface: NetworkInterface | None,
    devices: list[Device],
    selected_device_index: int,
    selected_menu_index: int,
    tree_selected_index: int,
    mode: str,
    reboot_required_count: int,
    tree_path_names: list[str],
    tree_items: list[TreeItem],
    tree_value: str,
    edit_path: list[int],
    edit_path_names: list[str],
    edit_old_value: str,
    edit_new_value: str,
    edit_enum_values: list[str],
    edit_enum_index: int,
    status_message: str,
    bind_ip: str,
    target: str,
    port: int,
    error: str,
    control_rtt_by_uid: dict[bytes, float],
    programming_snapshot: ProgrammingSnapshot | None,
) -> str:
    size = shutil.get_terminal_size((100, 30))
    cols = max(size.columns, 60)
    rows = max(size.lines, 20)
    footer_rows = 2
    body_rows = rows - footer_rows
    left_w = max(30, cols // 2)
    right_w = cols - left_w - 1

    now = time.monotonic()
    lines: list[str] = []
    if mode == "interface_select":
        title = " Bootloader CLI - Network"
    elif mode == "discovery":
        title = " Bootloader CLI - Discovery"
    elif mode == "programming":
        title = " Bootloader CLI - Programming"
    else:
        title = " Bootloader CLI - Device"
    lines.append(truncate(title, cols))
    lines.append("-" * cols)

    if mode == "interface_select":
        left_content = build_interface_left_pane(network_interfaces, selected_interface_index)
        right_content = build_interface_right_pane(network_interfaces, selected_interface_index, right_w)
        bindings = " Up/Down: select adapter   Enter: use adapter   Q: quit"
    elif mode == "discovery":
        left_content = build_discovery_left_pane(devices, selected_device_index, now, left_w)
        right_content = build_discovery_right_pane(
            devices,
            selected_device_index,
            selected_interface,
            bind_ip,
            target,
            port,
            error,
            now,
            right_w,
            control_rtt_by_uid,
        )
        bindings = (
            " Up/Down: select device   Enter: open device"
            "   R: refresh discovery   Q: quit"
        )
    elif mode == "programming":
        device = devices[selected_device_index] if devices else None
        snapshot = programming_snapshot or ProgrammingSnapshot(active=True, phase="loading", status="starting")
        left_content = build_programming_left_pane(device, snapshot, left_w)
        right_content = build_programming_right_pane(snapshot, right_w)
        bindings = " Q: quit   programming runs in background"
    else:
        device = devices[selected_device_index] if devices else None
        if mode in ("raw_tree", "edit_leaf"):
            left_content = build_tree_left_pane(
                device,
                tree_path_names,
                tree_items,
                tree_selected_index,
                reboot_required_count,
                left_w,
            )
        else:
            left_content = build_device_menu_left_pane(
                device,
                selected_menu_index,
                reboot_required_count,
                left_w,
            )
        right_content = build_device_menu_right_pane(
            device,
            selected_menu_index,
            tree_selected_index,
            mode,
            tree_path_names,
            tree_items,
            tree_value,
            edit_path,
            edit_path_names,
            edit_old_value,
            edit_new_value,
            edit_enum_values,
            edit_enum_index,
            status_message,
            now,
            right_w,
            control_rtt_by_uid.get(device.uid) if device else None,
        )
        if mode == "edit_leaf":
            if edit_enum_values:
                bindings = " Left/Right: select value   Enter: write   Esc/B: cancel   Q: quit"
            else:
                bindings = " Type: edit new value   Enter: write   Esc/B: cancel   Q: quit"
        elif mode == "raw_tree":
            bindings = " Up/Down: select node   Enter: open/read   Esc/B: back/up   R: refresh   Q: quit"
        else:
            bindings = " Up/Down: select   Enter: select/save   Esc/B: back   R: refresh   Q: quit"

    for row in range(body_rows - 3):
        left = left_content[row] if row < len(left_content) else ""
        right = right_content[row] if row < len(right_content) else ""
        lines.append(f"{truncate(left, left_w)}|{truncate(right, right_w)}")

    lines.append("-" * cols)
    lines.append(truncate(bindings, cols))

    return "\n".join(truncate(line, cols) for line in lines[:rows])


def build_interface_left_pane(interfaces: list[NetworkInterface], selected_index: int) -> list[str]:
    lines = [" Network interfaces", f" {len(interfaces)} adapter(s)", ""]
    if not interfaces:
        lines.extend([" No IPv4 adapters found.", "", " Press Q to quit."])
        return lines

    selected_index = max(0, min(selected_index, len(interfaces) - 1))
    for index, interface in enumerate(interfaces):
        selected = ">" if index == selected_index else " "
        lines.append(f"{selected} {style_data(interface.name)}")
        lines.append(f"  IP      {style_data(interface.ip)}")
        if interface.subnet:
            lines.append(f"  Subnet  {style_data(interface.subnet)}")
        if interface.gateway:
            lines.append(f"  Gateway {style_data(interface.gateway)}")
        lines.append("")

    return lines


def build_interface_right_pane(
    interfaces: list[NetworkInterface],
    selected_index: int,
    width: int,
) -> list[str]:
    lines = [" Adapter details", ""]
    if not interfaces:
        lines.extend(
            [
                " No usable IPv4 adapters were detected.",
                "",
                " You can still pass --bind-ip manually.",
            ]
        )
        return lines

    selected_index = max(0, min(selected_index, len(interfaces) - 1))
    interface = interfaces[selected_index]
    lines.extend(
        [
            f" Name             : {style_data(interface.name)}",
            f" IP               : {style_data(interface.ip)}",
            f" Subnet mask      : {style_data(interface.subnet or 'unknown')}",
            f" Gateway          : {style_data(interface.gateway or 'unknown')}",
            "",
            " Press Enter to use this adapter for",
            " discovery and control traffic.",
            "",
            " The selected adapter details will fill",
            " Local bind IP, subnet mask, and gateway.",
        ]
    )

    if width < 60:
        lines.append("")
        lines.append(" Widen terminal for easier reading.")

    return lines


def build_discovery_left_pane(devices: list[Device], selected_index: int, now: float, width: int) -> list[str]:
    lines = [
        " Discovered devices",
        f" {style_data(str(len(devices)))} device(s)",
        "",
    ]

    if not devices:
        lines.extend(
            [
                " No devices discovered yet.",
                "",
                " Waiting for DISCOVER_REPLY packets...",
            ]
        )
        return lines

    for index, device in enumerate(devices):
        selected = ">" if index == selected_index else " "
        age = format_age(now - device.last_seen)
        ip_col = device.ip.ljust(15)
        lines.append(f"{selected} {style_data(ip_col)} {style_data(format_mac(device.mac))}")
        lines.append(f"  UID {style_data(device.uid.hex()[:24])}")
        lines.append(f"  last seen {style_data(age)} ago")
        lines.append("")

    return lines


def build_discovery_right_pane(
    devices: list[Device],
    selected_index: int,
    selected_interface: NetworkInterface | None,
    bind_ip: str,
    target: str,
    port: int,
    error: str,
    now: float,
    width: int,
    control_rtt_by_uid: dict[bytes, float],
) -> list[str]:
    lines = [
        " Device information",
        "",
        f" Discovery target : {style_data(f'{target}:{port}')}",
        f" Local bind IP    : {style_data(bind_ip)}",
        f" Local subnet     : {style_data(selected_interface.subnet if selected_interface else 'unknown')}",
        f" Local gateway    : {style_data(selected_interface.gateway if selected_interface else 'unknown')}",
        "",
    ]

    if error:
        lines.extend([" Discovery error:", f" {style_data(error)}", ""])

    if not devices:
        lines.extend(
            [
                " Select a device once it appears on the left,",
                " then press Enter to open the device menu.",
                "",
                " Discovery refreshes automatically every 0.5s.",
            ]
        )
        return lines

    selected_index = max(0, min(selected_index, len(devices) - 1))
    device = devices[selected_index]
    lines.extend(
        [
            f" Source           : {style_data(f'{device.source[0]}:{device.source[1]}')}",
            f" IP               : {style_data(device.ip)}",
            f" Subnet           : {style_data(device.subnet)}",
            f" Gateway          : {style_data(device.gateway)}",
            f" MAC              : {style_data(format_mac(device.mac))}",
            f" UID              : {style_data(device.uid.hex())}",
            f" Capabilities     : {style_data(f'0x{device.capabilities:08x}')}",
            f" Resident version : {style_data(f'0x{device.resident_version:08x}')}",
            f" App version      : {style_data(f'0x{device.app_version:08x}')}",
            f" Last tx id       : {style_data(f'0x{device.transaction_id:08x}')}",
            f" First seen       : {style_data(format_age(now - device.first_seen))} ago",
            f" Last seen        : {style_data(format_age(now - device.last_seen))} ago",
            f" RTT              : {style_data(format_rtt(control_rtt_by_uid.get(device.uid)))}",
            "",
            " Press Enter to manage this device.",
        ]
    )

    if width < 60:
        lines.append("")
        lines.append(" Widen terminal for easier reading.")

    return lines


def build_left_header(title: str, reboot_required_count: int, width: int) -> str:
    if reboot_required_count <= 0:
        return f" {title}"

    label = style_amber(f"Nodes Requiring Re-Boot : {reboot_required_count}")
    budget = max(0, width - visible_len(label))
    return truncate(f" {title}", budget) + label


def build_device_menu_left_pane(
    device: Device | None,
    selected_menu_index: int,
    reboot_required_count: int,
    width: int,
) -> list[str]:
    lines = [build_left_header("Device menu", reboot_required_count, width), ""]
    if device is None:
        lines.append(" Selected device is no longer visible.")
        return lines

    lines.extend(
        [
            f" {style_data(device.ip)}",
            f" {style_data(format_mac(device.mac))}",
            "",
        ]
    )

    for index, item in enumerate(DEVICE_MENU_ITEMS):
        selected = ">" if index == selected_menu_index else " "
        lines.append(f"{selected} {item}")

    return lines


def build_tree_left_pane(
    device: Device | None,
    tree_path_names: list[str],
    tree_items: list[TreeItem],
    selected_index: int,
    reboot_required_count: int,
    width: int,
) -> list[str]:
    lines = [build_left_header("Raw device tree", reboot_required_count, width), ""]
    if device is None:
        lines.append(" Device disappeared from discovery.")
        return lines

    path = "/" + "/".join(tree_path_names)
    lines.extend([f" {style_data(device.ip)}", f" {truncate(style_data(path), 28)}", ""])

    if not tree_items:
        lines.append(" No child nodes.")
        return lines

    for index, item in enumerate(tree_items):
        selected = ">" if index == selected_index else " "
        if item.has_children:
            lines.append(f"{selected} {item.name}/")
        elif item.value:
            lines.append(f"{selected} {item.name} : {style_data(item.value)}")
        else:
            lines.append(f"{selected} {item.name}")

    return lines


def build_device_menu_right_pane(
    device: Device | None,
    selected_menu_index: int,
    tree_selected_index: int,
    mode: str,
    tree_path_names: list[str],
    tree_items: list[TreeItem],
    tree_value: str,
    edit_path: list[int],
    edit_path_names: list[str],
    edit_old_value: str,
    edit_new_value: str,
    edit_enum_values: list[str],
    edit_enum_index: int,
    status_message: str,
    now: float,
    width: int,
    control_rtt_s: float | None,
) -> list[str]:
    if device is None:
        return [" Device details", "", " Device disappeared from discovery."]

    selected_item = DEVICE_MENU_ITEMS[selected_menu_index]
    lines = [
        f" {selected_item}",
        "",
        f" Device IP        : {style_data(device.ip)}",
        f" Last seen        : {style_data(format_age(now - device.last_seen))} ago",
        f" RTT              : {style_data(format_rtt(control_rtt_s))}",
        "",
    ]

    if status_message:
        lines.extend([f" Status           : {style_data(status_message)}", ""])

    if mode == "edit_leaf":
        path = "/".join(edit_path_names)
        lines.extend(
            [
                " Set raw tree leaf.",
                "",
                f" Path             : {style_data(path)}",
                f" Node location    : {style_data('.'.join(str(part) for part in edit_path))}",
                "",
                f" Old Value        : {style_data(edit_old_value)}",
            ]
        )
        if edit_enum_values:
            selected_index = max(0, min(edit_enum_index, len(edit_enum_values) - 1))
            lines.extend(
                [
                    f" New Value        : < [{style_input(edit_enum_values[selected_index])}] >",
                    "",
                    " Use Left/Right to choose an option.",
                    " Press Enter to write the selected value.",
                    " Press Esc/B to cancel.",
                ]
            )
        else:
            lines.extend(
                [
                    f" New Value        : {style_input(edit_new_value)}",
                    "",
                    " Press Enter to write the new value.",
                    " Press Esc/B to cancel.",
                ]
            )
        return lines

    if mode == "raw_tree":
        path = "/" + "/".join(tree_path_names)
        lines.extend(
            [
                " Raw device tree navigation.",
                "",
                f" Path             : {style_data(path)}",
                "",
            ]
        )
        if tree_items:
            item = tree_items[max(0, min(tree_selected_index, len(tree_items) - 1))]
            lines.extend(
                [
                    f" Selected         : {style_data(item.name)}",
                    f" Node ID          : {style_data(str(item.node_id))}",
                    f" Access           : {style_data(format_access(item.access, item.write_effect))}",
                    f" Type             : {style_data('container' if item.has_children else 'leaf')}",
                ]
            )
            if item.value:
                lines.append(f" Listed value     : {style_data(item.value)}")
            if item.description:
                lines.append(f" Description      : {style_data(item.description)}")
            if item.unit:
                lines.append(f" Unit             : {style_data(item.unit)}")
            if item.enum_values:
                lines.append(f" Options          : {style_data(', '.join(item.enum_values))}")
            if tree_value:
                lines.append(f" Read value       : {style_data(tree_value)}")
            if not item.has_children and can_execute(item) and not can_write(item):
                lines.append(" Enter executes this action.")
            elif not item.has_children and can_write(item):
                lines.append(" Enter edits this leaf.")
            elif not item.has_children and can_read(item):
                lines.append(" Enter reads this read-only leaf.")
            lines.extend(
                [
                    "",
                    " Enter opens a container, reads, edits, or executes.",
                    " Esc/B goes up one level, then back to menu.",
                ]
            )
        else:
            lines.extend(
                [
                    " No child nodes at this location.",
                    "",
                    " Esc/B goes up one level, then back to menu.",
                ]
            )
        return lines

    if selected_item == "Device Tree":
        lines.extend(
            [
                " Browse raw device tree nodes.",
                "",
                " No firmware nodes are treated as locked for now.",
                " Press Enter to list the root node.",
            ]
        )
    elif selected_item == "Write Program":
        lines.extend(
            [
                " Firmware programming workflow.",
                "",
                " Press Enter to start a background",
                " programming job with live progress bars.",
                "",
                " Flow:",
                " * set Program/State to Erasing over UDP",
                " * poll Program/Programming TCP port",
                " * stream MSS-sized TCP DATA blocks",
                " * finalize and leave state Stopped",
                "",
                " Payload          : bootloader artifact or",
                "                    generated stress image",
            ]
        )
    elif selected_item == "Reboot Device":
        lines.extend(
            [
                " Standard reboot action.",
                "",
                " Press Enter to execute the top-level",
                " Reboot device-tree action.",
            ]
        )

    if width < 60:
        lines.append("")
        lines.append(" Widen terminal for easier reading.")

    return lines


def build_programming_left_pane(device: Device | None, snapshot: ProgrammingSnapshot, width: int) -> list[str]:
    lines = [build_left_header("Programming", 0, width), ""]
    if device is None:
        lines.append(" Selected device is no longer visible.")
        return lines

    lines.extend(
        [
            f" {style_data(device.ip)}",
            f" {style_data(format_mac(device.mac))}",
            "",
            f" Payload          : {style_data(snapshot.payload_name or 'loading...')}",
            f" Phase            : {style_data(snapshot.phase)}",
            "",
            " The transfer runs in the background",
            " so the progress bars can repaint live.",
        ]
    )
    return lines


def build_programming_right_pane(snapshot: ProgrammingSnapshot, width: int) -> list[str]:
    lines = [
        " Programming progress",
        "",
        f" Status           : {style_data(snapshot.status or 'working...')}",
        f" Image size       : {style_data(str(snapshot.image_bytes or 0))} bytes",
        "",
        progress_line("Erase", 1 if snapshot.phase in ("ready", "connecting", "programming", "finalizing", "done") else 0, 1, width, "phase"),
        progress_line(
            "Sent blocks",
            snapshot.sent_blocks,
            max(snapshot.total_blocks, 1),
            width,
            "queued to TCP",
        ),
        progress_line(
            "In flight",
            snapshot.in_flight_blocks,
            PROGRAM_WINDOW_FRAMES,
            width,
            "window",
        ),
        progress_line(
            "ACK packets",
            snapshot.acked_blocks,
            max(snapshot.total_blocks, 1),
            width,
            "received blocks",
        ),
        progress_line(
            "Bytes written",
            snapshot.acked_bytes,
            max(snapshot.image_bytes, 1),
            width,
            "payload bytes",
        ),
        "",
    ]

    if snapshot.finished:
        lines.append(f" Result           : {style_data(snapshot.result or ('ok' if snapshot.success else 'failed'))}")
    else:
        lines.append(" Waiting for final reply...")

    if width < 60:
        lines.append("")
        lines.append(" Widen terminal for easier reading.")

    return lines


def main() -> int:
    args = parse_args()
    program_image_path = resolve_program_image_path(args.program_image)
    boot_payload_path = resolve_boot_payload_path(args.boot_payload)
    network_interfaces = list_network_interfaces()
    selected_interface_index = default_interface_index(network_interfaces)
    selected_interface: NetworkInterface | None = None
    worker: DiscoveryWorker | None = None
    programming_worker: ProgrammingWorker | None = None
    mode = "interface_select"
    if args.bind_ip:
        selected_interface = interface_for_bind_ip(args.bind_ip, network_interfaces)
        worker = DiscoveryWorker(args.bind_ip, args.target, args.port, args.refresh)
        mode = "discovery"
    elif network_interfaces:
        args.bind_ip = network_interfaces[selected_interface_index].ip
    else:
        args.bind_ip = DEFAULT_BIND_IP
    selected_device_index = 0
    selected_menu_index = 0
    tree_selected_index = 0
    tree_location: list[int] = []
    tree_path_names: list[str] = []
    tree_items: list[TreeItem] = []
    tree_value = ""
    edit_path: list[int] = []
    edit_path_names: list[str] = []
    edit_old_value = ""
    edit_new_value = ""
    edit_enum_values: list[str] = []
    edit_enum_index = 0
    status_message = ""
    last_tree_refresh = 0.0
    reboot_required_nodes_by_uid: dict[bytes, set[tuple[int, ...]]] = {}
    # IPv4 tree writes update flash only; lwIP uses new values after reboot. Keep discovery IP
    # until reboot succeeds, otherwise control packets (e.g. Reboot) still go to the old address.
    pending_network_patch_by_uid: dict[bytes, dict[str, str]] = {}
    control_rtt_by_uid: dict[bytes, float] = {}

    def effective_control_target(device: Device, known_devices: list[Device]) -> str:
        return device_control_target(
            device,
            args.bind_ip,
            selected_interface,
            args.target,
            known_devices,
            broadcast_when_adapter_ip_matches_device=True,
        )

    def normal_control_rtt(uid: bytes) -> float:
        return control_rtt_by_uid.get(uid, DEFAULT_CONTROL_RTT_S)

    def mark_device_seen(uid: bytes, source_ip: str | None = None, elapsed_s: float | None = None) -> None:
        if worker is not None:
            worker.mark_seen(uid)
            if source_ip and is_usable_ipv4(source_ip):
                worker.patch_device_network(uid, ip=source_ip)
        if elapsed_s is not None and elapsed_s > 0:
            previous = control_rtt_by_uid.get(uid)
            if previous is None:
                control_rtt_by_uid[uid] = elapsed_s
            else:
                control_rtt_by_uid[uid] = (previous * 0.8) + (elapsed_s * 0.2)

    def apply_pending_network_patch(uid: bytes) -> None:
        """After reboot, lwIP uses new IPv4 from flash — align cached discovery record."""
        if worker is None:
            pending_network_patch_by_uid.pop(uid, None)
            return
        pending = pending_network_patch_by_uid.pop(uid, None)
        if not pending:
            return
        worker.patch_device_network(
            uid,
            ip=pending.get("ip"),
            subnet=pending.get("subnet"),
            gateway=pending.get("gateway"),
        )

    def load_tree_node(
        device: Device,
        known_devices: list[Device],
        update_status: bool = True,
        clear_read_value: bool = True,
        preserve_items_on_error: bool = False,
        fallback_on_timeout: bool = True,
        timeout_s: float = DEFAULT_REQUEST_TIMEOUT_S,
    ) -> None:
        nonlocal tree_items, tree_selected_index, tree_value, status_message
        ctrl_target = effective_control_target(device, known_devices)
        ok, items, message = list_device_tree_node(
            args.bind_ip,
            ctrl_target,
            args.target if fallback_on_timeout else None,
            device.uid,
            tree_location,
            timeout_s=timeout_s,
            normal_rtt_s=normal_control_rtt(device.uid),
            on_response=lambda source_ip, elapsed_s, uid=device.uid: mark_device_seen(uid, source_ip, elapsed_s),
        )
        if ok:
            tree_items = [item for item in items if not is_implemented_in_device_menu(tree_location, item)]
            tree_selected_index = min(tree_selected_index, max(0, len(tree_items) - 1))
            if clear_read_value:
                tree_value = ""
            if update_status:
                status_message = f"loaded {len(tree_items)} node(s)"
        else:
            if not preserve_items_on_error:
                tree_items = []
            if clear_read_value:
                tree_value = ""
            if update_status:
                status_message = message

    if worker is not None:
        worker.start()
    try:
        with Terminal() as terminal, KeyReader() as keys:
            while True:
                devices, error = worker.snapshot() if worker is not None else ([], "")
                programming_snapshot = programming_worker.snapshot() if programming_worker is not None else None
                if programming_worker is not None and programming_snapshot.finished:
                    status_message = (
                        programming_snapshot.result
                        if programming_snapshot.success
                        else f"programming failed: {programming_snapshot.result}"
                    )
                    if worker is not None:
                        worker.set_enabled(True)
                    programming_worker = None
                    mode = "device_menu" if devices else "discovery"
                    programming_snapshot = None
                if selected_device_index >= len(devices):
                    selected_device_index = max(0, len(devices) - 1)
                if mode not in ("discovery", "interface_select") and not devices:
                    mode = "discovery"
                    if worker is not None:
                        worker.set_enabled(True)
                selected_device = devices[selected_device_index] if devices else None
                reboot_required_count = (
                    len(reboot_required_nodes_by_uid.get(selected_device.uid, set())) if selected_device else 0
                )
                now = time.monotonic()
                if (
                    selected_device is not None
                    and mode in ("device_menu", "raw_tree", "edit_leaf")
                    and (now - last_tree_refresh) >= TREE_REFRESH_S
                ):
                    load_tree_node(
                        selected_device,
                        devices,
                        update_status=False,
                        clear_read_value=False,
                        preserve_items_on_error=True,
                        fallback_on_timeout=True,
                        timeout_s=PASSIVE_TREE_TIMEOUT_S,
                    )
                    last_tree_refresh = now

                terminal.render(
                    build_screen(
                        network_interfaces=network_interfaces,
                        selected_interface_index=selected_interface_index,
                        selected_interface=selected_interface,
                        devices=devices,
                        selected_device_index=selected_device_index,
                        selected_menu_index=selected_menu_index,
                        tree_selected_index=tree_selected_index,
                        mode=mode,
                        reboot_required_count=reboot_required_count,
                        tree_path_names=tree_path_names,
                        tree_items=tree_items,
                        tree_value=tree_value,
                        edit_path=edit_path,
                        edit_path_names=edit_path_names,
                        edit_old_value=edit_old_value,
                        edit_new_value=edit_new_value,
                        edit_enum_values=edit_enum_values,
                        edit_enum_index=edit_enum_index,
                        status_message=status_message,
                        bind_ip=args.bind_ip,
                        target=args.target,
                        port=args.port,
                        error=error,
                        control_rtt_by_uid=control_rtt_by_uid,
                        programming_snapshot=programming_snapshot,
                    )
                )

                key = keys.read_key()
                if key in ("q", "\x03"):
                    break
                if mode == "programming":
                    time.sleep(0.05)
                    continue

                if mode == "interface_select":
                    if key == "up" and network_interfaces:
                        selected_interface_index = max(0, selected_interface_index - 1)
                    elif key == "down" and network_interfaces:
                        selected_interface_index = min(len(network_interfaces) - 1, selected_interface_index + 1)
                    elif key == "enter" and network_interfaces:
                        selected_interface = network_interfaces[selected_interface_index]
                        args.bind_ip = selected_interface.ip
                        worker = DiscoveryWorker(args.bind_ip, args.target, args.port, args.refresh)
                        worker.start()
                        mode = "discovery"
                        last_tree_refresh = 0.0
                        status_message = ""
                    elif key == "r":
                        network_interfaces = list_network_interfaces()
                        selected_interface_index = default_interface_index(network_interfaces)
                        if network_interfaces:
                            args.bind_ip = network_interfaces[selected_interface_index].ip
                    time.sleep(0.05)
                    continue

                if mode == "edit_leaf":
                    if key in ("\x1b", "b"):
                        mode = "raw_tree"
                        edit_new_value = ""
                        edit_enum_values = []
                        edit_enum_index = 0
                    elif edit_enum_values and key == "left":
                        edit_enum_index = (edit_enum_index - 1) % len(edit_enum_values)
                        edit_new_value = edit_enum_values[edit_enum_index]
                    elif edit_enum_values and key == "right":
                        edit_enum_index = (edit_enum_index + 1) % len(edit_enum_values)
                        edit_new_value = edit_enum_values[edit_enum_index]
                    elif not edit_enum_values and key == "backspace":
                        edit_new_value = edit_new_value[:-1]
                    elif key == "enter" and devices:
                        ctrl_target = effective_control_target(devices[selected_device_index], devices)
                        write_started_ns = time.perf_counter_ns()
                        result, message, reboot_required = set_device_tree_value(
                            args.bind_ip,
                            ctrl_target,
                            args.target,
                            devices[selected_device_index].uid,
                            edit_path,
                            edit_new_value,
                            normal_rtt_s=normal_control_rtt(devices[selected_device_index].uid),
                            on_response=lambda source_ip, elapsed_s, uid=devices[selected_device_index].uid: mark_device_seen(
                                uid, source_ip, elapsed_s
                            ),
                        )
                        write_elapsed_s = max((time.perf_counter_ns() - write_started_ns) / 1_000_000_000, 0.000001)
                        if result:
                            device_uid = devices[selected_device_index].uid
                            pending_nodes = reboot_required_nodes_by_uid.setdefault(device_uid, set())
                            pending_path = tuple(edit_path)
                            if reboot_required:
                                pending_nodes.add(pending_path)
                                patch_fields: dict[str, str] = {}
                                written = edit_new_value.strip()
                                if edit_path == IPV4_ADDRESS_PATH:
                                    try:
                                        ipaddress.IPv4Address(written)
                                    except ipaddress.AddressValueError:
                                        pass
                                    else:
                                        patch_fields["ip"] = written
                                elif edit_path == IPV4_SUBNET_PATH:
                                    try:
                                        ipaddress.IPv4Address(written)
                                    except ipaddress.AddressValueError:
                                        pass
                                    else:
                                        patch_fields["subnet"] = written
                                elif edit_path == IPV4_GATEWAY_PATH:
                                    try:
                                        ipaddress.IPv4Address(written)
                                    except ipaddress.AddressValueError:
                                        pass
                                    else:
                                        patch_fields["gateway"] = written
                                if patch_fields:
                                    pending_network_patch_by_uid.setdefault(device_uid, {}).update(patch_fields)
                            else:
                                pending_nodes.discard(pending_path)
                                if worker is not None:
                                    written = edit_new_value.strip()
                                    if edit_path == IPV4_ADDRESS_PATH:
                                        try:
                                            ipaddress.IPv4Address(written)
                                        except ipaddress.AddressValueError:
                                            pass
                                        else:
                                            worker.patch_device_network(device_uid, ip=written)
                                    elif edit_path == IPV4_SUBNET_PATH:
                                        try:
                                            ipaddress.IPv4Address(written)
                                        except ipaddress.AddressValueError:
                                            pass
                                        else:
                                            worker.patch_device_network(device_uid, subnet=written)
                                    elif edit_path == IPV4_GATEWAY_PATH:
                                        try:
                                            ipaddress.IPv4Address(written)
                                        except ipaddress.AddressValueError:
                                            pass
                                        else:
                                            worker.patch_device_network(device_uid, gateway=written)
                        write_status = (
                            f"wrote {edit_path_names[-1]}: {message} ({format_rtt(write_elapsed_s)})"
                            if result
                            else f"{message} ({format_rtt(write_elapsed_s)})"
                        )
                        mode = "raw_tree"
                        edit_new_value = ""
                        edit_enum_values = []
                        edit_enum_index = 0
                        load_tree_node(devices[selected_device_index], devices)
                        status_message = write_status
                    elif not edit_enum_values and key is not None and len(key) == 1 and key.isprintable():
                        if len(edit_new_value) < 64:
                            edit_new_value += key
                    time.sleep(0.05)
                    continue

                if mode == "raw_tree":
                    if key in ("\x1b", "b"):
                        if tree_location and devices:
                            child_node_id = tree_location.pop()
                            tree_path_names.pop()
                            load_tree_node(devices[selected_device_index], devices)
                            for index, item in enumerate(tree_items):
                                if item.node_id == child_node_id:
                                    tree_selected_index = index
                                    break
                        else:
                            mode = "device_menu"
                    elif key == "up" and tree_items:
                        tree_selected_index = max(0, tree_selected_index - 1)
                        tree_value = ""
                    elif key == "down" and tree_items:
                        tree_selected_index = min(len(tree_items) - 1, tree_selected_index + 1)
                        tree_value = ""
                    elif key == "enter" and tree_items and devices:
                        item = tree_items[tree_selected_index]
                        if item.has_children:
                            tree_location.append(item.node_id)
                            tree_path_names.append(item.name)
                            tree_selected_index = 0
                            load_tree_node(devices[selected_device_index], devices)
                        elif can_execute(item) and not can_write(item):
                            execute_path = tree_location + [item.node_id]
                            execute_started = time.monotonic()
                            ok, message = execute_device_tree_node(
                                args.bind_ip,
                                effective_control_target(devices[selected_device_index], devices),
                                args.target,
                                devices[selected_device_index].uid,
                                execute_path,
                                normal_rtt_s=normal_control_rtt(devices[selected_device_index].uid),
                                on_response=lambda source_ip, elapsed_s, uid=devices[selected_device_index].uid: mark_device_seen(
                                    uid, source_ip, elapsed_s
                                ),
                            )
                            execute_elapsed_ms = int((time.monotonic() - execute_started) * 1000)
                            if ok and is_standard_reboot_path(execute_path):
                                uid_re = devices[selected_device_index].uid
                                reboot_required_nodes_by_uid.pop(uid_re, None)
                                apply_pending_network_patch(uid_re)
                            tree_value = ""
                            status_message = (
                                f"executed {item.name}: {message} ({execute_elapsed_ms} ms)"
                                if ok
                                else f"{message} ({execute_elapsed_ms} ms)"
                            )
                        else:
                            ok, value, message = get_device_tree_value(
                                args.bind_ip,
                                effective_control_target(devices[selected_device_index], devices),
                                args.target,
                                devices[selected_device_index].uid,
                                tree_location + [item.node_id],
                                normal_rtt_s=normal_control_rtt(devices[selected_device_index].uid),
                                on_response=lambda source_ip, elapsed_s, uid=devices[selected_device_index].uid: mark_device_seen(
                                    uid, source_ip, elapsed_s
                                ),
                            )
                            if ok:
                                tree_value = value
                                if can_write(item):
                                    edit_path = tree_location + [item.node_id]
                                    edit_path_names = tree_path_names + [item.name]
                                    edit_old_value = value
                                    edit_enum_values = item.enum_values.copy()
                                    if edit_enum_values:
                                        edit_enum_index = (
                                            edit_enum_values.index(value) if value in edit_enum_values else 0
                                        )
                                        edit_new_value = edit_enum_values[edit_enum_index]
                                    else:
                                        edit_enum_index = 0
                                        hint = edit_default_value(edit_path_names, selected_interface)
                                        edit_new_value = hint if hint else value
                                    status_message = ""
                                    mode = "edit_leaf"
                                else:
                                    status_message = "read-only node"
                            else:
                                tree_value = ""
                                status_message = message
                    elif key == "r" and devices:
                        load_tree_node(devices[selected_device_index], devices)
                    time.sleep(0.05)
                    continue

                if key in ("\x1b", "b") and mode == "device_menu":
                    mode = "discovery"
                    if worker is not None:
                        worker.set_enabled(True)
                elif key == "up" and devices:
                    if mode == "discovery":
                        selected_device_index = max(0, selected_device_index - 1)
                    else:
                        selected_menu_index = max(0, selected_menu_index - 1)
                elif key == "down" and devices:
                    if mode == "discovery":
                        selected_device_index = min(len(devices) - 1, selected_device_index + 1)
                    else:
                        selected_menu_index = min(len(DEVICE_MENU_ITEMS) - 1, selected_menu_index + 1)
                elif key == "enter" and devices:
                    if mode == "discovery":
                        mode = "device_menu"
                        selected_menu_index = 0
                        last_tree_refresh = 0.0
                        if worker is not None:
                            worker.set_enabled(False)
                        status_message = ""
                    elif DEVICE_MENU_ITEMS[selected_menu_index] == "Device Tree":
                        tree_location = []
                        tree_path_names = []
                        tree_selected_index = 0
                        tree_value = ""
                        load_tree_node(devices[selected_device_index], devices)
                        mode = "raw_tree"
                    elif DEVICE_MENU_ITEMS[selected_menu_index] == "Write Program":
                        device = devices[selected_device_index]
                        programming_worker = ProgrammingWorker(
                            args.bind_ip,
                            effective_control_target(device, devices),
                            args.target,
                            device,
                            normal_control_rtt(device.uid),
                            program_image_path,
                        )
                        programming_worker.start()
                        if worker is not None:
                            worker.set_enabled(False)
                        mode = "programming"
                        status_message = "starting programming workflow"
                    elif DEVICE_MENU_ITEMS[selected_menu_index] == "Write Boot Payload":
                        device = devices[selected_device_index]
                        programming_worker = BootPayloadWorker(
                            args.bind_ip,
                            effective_control_target(device, devices),
                            args.target,
                            device,
                            normal_control_rtt(device.uid),
                            boot_payload_path,
                        )
                        programming_worker.start()
                        if worker is not None:
                            worker.set_enabled(False)
                        mode = "programming"
                        status_message = "starting boot payload write"
                    elif DEVICE_MENU_ITEMS[selected_menu_index] == "Apply Boot Update":
                        execute_started = time.monotonic()
                        ok, message = run_apply_boot_update(
                            args.bind_ip,
                            effective_control_target(devices[selected_device_index], devices),
                            args.target,
                            devices[selected_device_index],
                            normal_control_rtt(devices[selected_device_index].uid),
                            on_response=lambda source_ip, elapsed_s, uid=devices[selected_device_index].uid: mark_device_seen(
                                uid, source_ip, elapsed_s
                            ),
                        )
                        execute_elapsed_ms = int((time.monotonic() - execute_started) * 1000)
                        status_message = (
                            f"executed Apply Boot Update: {message} ({execute_elapsed_ms} ms)"
                            if ok
                            else f"{message} ({execute_elapsed_ms} ms)"
                        )
                    elif DEVICE_MENU_ITEMS[selected_menu_index] == "Reboot Device":
                        execute_started = time.monotonic()
                        ok, message = execute_device_tree_node(
                            args.bind_ip,
                            effective_control_target(devices[selected_device_index], devices),
                            args.target,
                            devices[selected_device_index].uid,
                            STANDARD_REBOOT_PATH,
                            normal_rtt_s=normal_control_rtt(devices[selected_device_index].uid),
                            on_response=lambda source_ip, elapsed_s, uid=devices[selected_device_index].uid: mark_device_seen(
                                uid, source_ip, elapsed_s
                            ),
                        )
                        execute_elapsed_ms = int((time.monotonic() - execute_started) * 1000)
                        if ok:
                            uid_rb = devices[selected_device_index].uid
                            reboot_required_nodes_by_uid.pop(uid_rb, None)
                            apply_pending_network_patch(uid_rb)
                        status_message = (
                            f"executed Reboot: {message} ({execute_elapsed_ms} ms)"
                            if ok
                            else f"{message} ({execute_elapsed_ms} ms)"
                        )
                elif key == "r":
                    if worker is not None:
                        worker.refresh_now()

                time.sleep(0.05)
    finally:
        if worker is not None:
            worker.stop()

    return os.EX_OK


if __name__ == "__main__":
    raise SystemExit(main())
