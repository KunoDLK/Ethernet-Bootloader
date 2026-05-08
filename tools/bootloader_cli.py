
#!/usr/bin/env python3
"""Full-screen CLI for the resident Ethernet bootloader."""

from __future__ import annotations

import argparse
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
from dataclasses import dataclass, replace
from typing import Callable


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

ACCESS_READ = 0x8
ACCESS_WRITE = 0x4
ACCESS_EXECUTE = 0x2

DISCOVERY_HEADER = struct.Struct("<IBBHI")
COMMAND_HEADER = struct.Struct("<IBBHIHH")
DISCOVERY_REPLY = struct.Struct("<IBBHI12s6s4s4s4sIII")
DEVICE_TREE_OP_LIST = 0x01
DEVICE_TREE_OP_GET = 0x02
DEVICE_TREE_OP_SET = 0x03
DEVICE_TREE_OP_EXECUTE = 0x04
STANDARD_REBOOT_PATH = [2]
# Device tree locations (see Firmware/Resident/Src/resident_device_tree.c TREE_ID_*).
IPV4_ADDRESS_PATH = [1, 2]
IPV4_SUBNET_PATH = [1, 3]
IPV4_GATEWAY_PATH = [1, 4]
IPV4_DHCP_PATH = [1, 5]
DEVICE_MENU_ITEMS = [
    "Device Tree",
    "Write Program",
    "Reboot Device",
]


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
    tree_payload = build_device_tree_request(op, location, op_payload)
    inner = bytes([PROTO_MSG_DEVICETREE_REQ]) + struct.pack("<H", len(tree_payload)) + tree_payload
    payload = uid + inner
    transaction_id = random.SystemRandom().randrange(0, 0x1_0000_0000)

    def attempt(dst: str, wait_s: float) -> tuple[bool, bytes, str]:
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
                    return False, b"", f"error {result} detail {detail}"
                if inner_type != PROTO_MSG_DEVICETREE_REPLY or len(inner_payload) < 6:
                    return False, b"", f"unexpected reply 0x{inner_type:02x}"

                depth = inner_payload[1]
                result_offset = 2 + depth
                result, response_len = struct.unpack_from("<hH", inner_payload, result_offset)
                response = inner_payload[result_offset + 4 : result_offset + 4 + response_len]
                if result != 0:
                    return False, b"", f"request failed result {result}"
                return True, response, "ok"

        return False, b"", "request timed out"

    max_retry_s = max(min(timeout_s, MAX_CONTROL_RETRY_S), MIN_CONTROL_RETRY_S)
    retry_s = min(max(normal_rtt_s * 2.0, MIN_CONTROL_RETRY_S), max_retry_s)
    while retry_s <= max_retry_s:
        ok, response, message = attempt(target, retry_s)
        if ok:
            return ok, response, message
        if message != "request timed out":
            return ok, response, message
        retry_s *= 2.0

    if fallback_target and fallback_target != target:
        ok2, response2, message2 = attempt(fallback_target, MAX_CONTROL_RETRY_S)
        if ok2:
            return ok2, response2, "ok (broadcast fallback)"
        return ok2, response2, message2

    return False, b"", "request timed out"


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
    ok, response, message = send_device_tree_request(
        bind_ip,
        target,
        fallback_target,
        uid,
        DEVICE_TREE_OP_LIST,
        location,
        timeout_s=timeout_s,
        normal_rtt_s=normal_rtt_s,
        on_response=on_response,
    )
    if not ok:
        return False, [], message
    if len(response) < 2:
        return False, [], "short LIST reply"

    count = struct.unpack_from("<H", response, 0)[0]
    offset = 2
    items: list[TreeItem] = []
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
        items.append(
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

    return True, items, "ok"


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
                " Intended flow:",
                " * unlock programming if required",
                " * start programming mode",
                " * stream application image over TCP",
                " * finalize and verify",
                "",
                " Not wired to firmware yet.",
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


def main() -> int:
    args = parse_args()
    network_interfaces = list_network_interfaces()
    selected_interface_index = default_interface_index(network_interfaces)
    selected_interface: NetworkInterface | None = None
    worker: DiscoveryWorker | None = None
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
                    )
                )

                key = keys.read_key()
                if key in ("q", "\x03"):
                    break

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
