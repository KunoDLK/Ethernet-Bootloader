#!/usr/bin/env python3
"""Test the resident bootloader UDP discovery service."""

from __future__ import annotations

import argparse
import ipaddress
import os
import random
import socket
import struct
import time
from dataclasses import dataclass


PROTO_MAGIC = 0x424C4452
PROTO_VERSION = 1
PROTO_MSG_DISCOVER_REQ = 0x01
PROTO_MSG_DISCOVER_REPLY = 0x02
PROTO_FLAG_REPLY = 1 << 0
DISCOVERY_PORT = 45000

DISCOVERY_HEADER = struct.Struct("<IBBHI")
DISCOVERY_REPLY = struct.Struct("<IBBHI12s6s4s4s4sIII")


@dataclass(frozen=True)
class DiscoveryReply:
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send a bootloader DISCOVER_REQ and print DISCOVER_REPLY packets."
    )
    parser.add_argument(
        "--target",
        default="255.255.255.255",
        help="destination IPv4 address, e.g. 255.255.255.255, 10.0.0.255, or 10.0.0.2",
    )
    parser.add_argument("--port", type=int, default=DISCOVERY_PORT, help="UDP discovery port")
    parser.add_argument(
        "--bind-ip",
        default="0.0.0.0",
        help="local IPv4 address to bind; use 10.0.0.1 to force the Ethernet adapter",
    )
    parser.add_argument("--timeout", type=float, default=1.0, help="seconds to listen after each request")
    parser.add_argument("--repeat", type=int, default=3, help="number of discovery requests to send")
    parser.add_argument(
        "--transaction-id",
        type=lambda value: int(value, 0),
        default=None,
        help="optional transaction id, decimal or 0x-prefixed hex",
    )
    return parser.parse_args()


def make_request(transaction_id: int) -> bytes:
    return DISCOVERY_HEADER.pack(
        PROTO_MAGIC,
        PROTO_VERSION,
        PROTO_MSG_DISCOVER_REQ,
        0,
        transaction_id,
    )


def parse_reply(data: bytes, source: tuple[str, int]) -> DiscoveryReply | None:
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

    return DiscoveryReply(
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
    )


def format_mac(mac: bytes) -> str:
    return ":".join(f"{byte:02x}" for byte in mac)


def print_reply(reply: DiscoveryReply) -> None:
    print(f"reply from {reply.source[0]}:{reply.source[1]}")
    print(f"  transaction_id   0x{reply.transaction_id:08x}")
    print(f"  uid              {reply.uid.hex()}")
    print(f"  mac              {format_mac(reply.mac)}")
    print(f"  ip               {reply.ip}")
    print(f"  subnet           {reply.subnet}")
    print(f"  gateway          {reply.gateway}")
    print(f"  capabilities     0x{reply.capabilities:08x}")
    print(f"  resident_version 0x{reply.resident_version:08x}")
    print(f"  app_version      0x{reply.app_version:08x}")


def main() -> int:
    args = parse_args()
    transaction_id = args.transaction_id
    if transaction_id is None:
        transaction_id = random.SystemRandom().randrange(0, 0x1_0000_0000)

    request = make_request(transaction_id)
    target = (args.target, args.port)
    replies: dict[tuple[str, int, bytes], DiscoveryReply] = {}

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.bind_ip, 0))
        sock.settimeout(0.05)

        print(
            f"sending DISCOVER_REQ tx=0x{transaction_id:08x} "
            f"from {sock.getsockname()[0]}:{sock.getsockname()[1]} to {target[0]}:{target[1]}"
        )

        for attempt in range(1, args.repeat + 1):
            sock.sendto(request, target)
            deadline = time.monotonic() + args.timeout

            while time.monotonic() < deadline:
                try:
                    data, source = sock.recvfrom(2048)
                except TimeoutError:
                    continue
                except socket.timeout:
                    continue

                reply = parse_reply(data, source)
                if reply is None:
                    print(f"ignored {len(data)} bytes from {source[0]}:{source[1]}")
                    continue
                if reply.transaction_id != transaction_id:
                    print(
                        f"ignored reply with unexpected tx=0x{reply.transaction_id:08x} "
                        f"from {source[0]}:{source[1]}"
                    )
                    continue

                key = (reply.source[0], reply.source[1], reply.uid)
                if key not in replies:
                    replies[key] = reply
                    print_reply(reply)

            if attempt < args.repeat:
                time.sleep(0.1)

    if replies:
        print(f"received {len(replies)} unique discover reply/replies")
        return os.EX_OK

    print("no discover replies received")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
