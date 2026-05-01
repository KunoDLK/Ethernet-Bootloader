#!/usr/bin/env python3
"""Exercise bootloader broadcast UID control and network device-tree nodes."""

from __future__ import annotations

import argparse
import ipaddress
import random
import socket
import struct
import time


PROTO_MAGIC = 0x424C4452
PROTO_VERSION = 1
PROTO_FLAG_REPLY = 1 << 0

MSG_DISCOVER_REQ = 0x01
MSG_DISCOVER_REPLY = 0x02
MSG_BCAST_UID_REQ = 0x12
MSG_BCAST_UID_REPLY = 0x13
MSG_DEVICETREE_REQ = 0x20
MSG_DEVICETREE_REPLY = 0x21
MSG_ERROR_REPLY = 0x7F

DISCOVERY_PORT = 45000
CONTROL_PORT = 45001
DEFAULT_BIND_IP = "192.168.1.99"
DEFAULT_TARGET = "255.255.255.255"

OP_LIST = 0x01
OP_GET = 0x02
OP_SET = 0x03

HEADER = struct.Struct("<IBBHI")
CMD_HEADER = struct.Struct("<IBBHIHH")
DISCOVERY_REPLY = struct.Struct("<IBBHI12s6s4s4s4sIII")

PATHS = {
    "root": [],
    "network": [1],
    "mac": [1, 1],
    "ipv4": [1, 2],
    "address": [1, 2, 1],
    "subnet": [1, 2, 2],
    "gateway": [1, 2, 3],
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Test broadcast UID device-tree control.")
    parser.add_argument("--bind-ip", default=DEFAULT_BIND_IP)
    parser.add_argument("--target", default=DEFAULT_TARGET)
    subcommands = parser.add_subparsers(dest="command")

    list_cmd = subcommands.add_parser("list", help="list a node")
    list_cmd.add_argument("path", choices=PATHS.keys(), default="root", nargs="?")

    get_cmd = subcommands.add_parser("get", help="get a leaf value")
    get_cmd.add_argument("path", choices=["mac", "address", "subnet", "gateway"])

    set_cmd = subcommands.add_parser("set", help="set an IPv4 leaf value")
    set_cmd.add_argument("path", choices=["address", "subnet", "gateway"])
    set_cmd.add_argument("value", help="IPv4 value, e.g. 192.168.1.50")

    parser.set_defaults(command="list")
    return parser.parse_args()


def send_discovery(sock: socket.socket, target: str) -> bytes:
    tx = random.randrange(0, 0x1_0000_0000)
    sock.sendto(HEADER.pack(PROTO_MAGIC, PROTO_VERSION, MSG_DISCOVER_REQ, 0, tx), (target, DISCOVERY_PORT))
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            data, source = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < DISCOVERY_REPLY.size:
            continue
        fields = DISCOVERY_REPLY.unpack_from(data)
        if fields[0] == PROTO_MAGIC and fields[2] == MSG_DISCOVER_REPLY and fields[4] == tx:
            uid = fields[5]
            print(f"device {source[0]} uid={uid.hex()} ip={ipaddress.IPv4Address(fields[7])}")
            return uid
    raise RuntimeError("no discovery reply received")


def build_tree_payload(op: int, location: list[int], op_payload: bytes = b"") -> bytes:
    return bytes([op, len(location), *location]) + struct.pack("<H", len(op_payload)) + op_payload


def send_bcast_tree(sock: socket.socket, target: str, uid: bytes, tree_payload: bytes) -> tuple[int, bytes]:
    tx = random.randrange(0, 0x1_0000_0000)
    inner = bytes([MSG_DEVICETREE_REQ]) + struct.pack("<H", len(tree_payload)) + tree_payload
    payload = uid + inner
    packet = CMD_HEADER.pack(PROTO_MAGIC, PROTO_VERSION, MSG_BCAST_UID_REQ, 0, tx, len(payload), 0) + payload
    sock.sendto(packet, (target, CONTROL_PORT))

    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            data, _source = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < CMD_HEADER.size:
            continue
        magic, version, msg_type, _flags, rx_tx, payload_len, _reserved = CMD_HEADER.unpack_from(data)
        if magic != PROTO_MAGIC or version != PROTO_VERSION or msg_type != MSG_BCAST_UID_REPLY or rx_tx != tx:
            continue
        payload = data[CMD_HEADER.size : CMD_HEADER.size + payload_len]
        if len(payload) < 15 or payload[:12] != uid:
            continue
        inner_type = payload[12]
        inner_len = struct.unpack_from("<H", payload, 13)[0]
        return inner_type, payload[15 : 15 + inner_len]
    raise RuntimeError("no control reply received")


def decode_tree_reply(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 6:
        raise RuntimeError("short device-tree reply")
    op = payload[0]
    depth = payload[1]
    result_offset = 2 + depth
    result, response_len = struct.unpack_from("<hH", payload, result_offset)
    response = payload[result_offset + 4 : result_offset + 4 + response_len]
    print(f"op=0x{op:02x} result={result} response_len={response_len}")
    return result, response


def print_list(response: bytes) -> None:
    count = struct.unpack_from("<H", response, 0)[0]
    offset = 2
    print(f"{count} item(s)")
    for _ in range(count):
        node_id = response[offset]
        has_children = response[offset + 1]
        access = response[offset + 2]
        data_len = struct.unpack_from("<H", response, offset + 3)[0]
        data = response[offset + 5 : offset + 5 + data_len].decode("utf-8", errors="replace")
        offset += 5 + data_len
        print(f"  id={node_id} children={has_children} access=0x{access:02x} {data}")


def main() -> int:
    args = parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((args.bind_ip, 0))
        sock.settimeout(0.05)

        uid = send_discovery(sock, args.target)
        location = PATHS[getattr(args, "path", "root")]
        if args.command == "get":
            tree_payload = build_tree_payload(OP_GET, location)
        elif args.command == "set":
            value = args.value.encode("utf-8")
            tree_payload = build_tree_payload(OP_SET, location, struct.pack("<H", len(value)) + value)
        else:
            tree_payload = build_tree_payload(OP_LIST, location)

        inner_type, payload = send_bcast_tree(sock, args.target, uid, tree_payload)
        if inner_type == MSG_ERROR_REPLY:
            result, detail = struct.unpack_from("<hH", payload)
            print(f"error result={result} detail={detail}")
            return 1
        if inner_type != MSG_DEVICETREE_REPLY:
            print(f"unexpected inner reply 0x{inner_type:02x}")
            return 1

        result, response = decode_tree_reply(payload)
        if result != 0:
            return 1
        if args.command == "list":
            print_list(response)
        elif args.command == "get":
            value_len = struct.unpack_from("<H", response)[0]
            print(response[2 : 2 + value_len].decode("utf-8", errors="replace"))
        else:
            print(f"reboot_required={response[0] if response else 0}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
