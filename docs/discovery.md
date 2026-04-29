# Discovery (UDP broadcast)

## Goal

Allow a host on the same L2 broadcast domain (same VLAN/subnet) to discover all devices without knowing their IP.

## Transport

- **Protocol**: UDP
- **Port**: `DISCOVERY_PORT` (TBD; fixed per firmware build)
- **Scope**: L2 broadcast only (not routed)

## Host behavior

- Create a UDP socket and enable broadcast (`SO_BROADCAST`).
- Send `DISCOVER_REQ` to:
  - limited broadcast `255.255.255.255:DISCOVERY_PORT`
  - and optionally each interface-directed broadcast (e.g. `192.168.1.255:DISCOVERY_PORT`)
- Listen for `DISCOVER_REPLY` for a short window (e.g. 200–500 ms), optionally retry 2–3 times.

## Device behavior

- Bind to `0.0.0.0:DISCOVERY_PORT`.
- When `DISCOVER_REQ` is received:
  - wait random jitter (e.g. 0–50 ms)
  - reply **unicast** to `(src_ip, src_port)` with `DISCOVER_REPLY`
- Rate limit responses (e.g. max 1 reply/sec per sender IP) to avoid storms.

## Packet envelope (common)

All discovery packets use the following header.

| Field | Size | Type | Notes |
|---|---:|---|---|
| `magic` | 4 | u32 | Fixed value to identify protocol (TBD) |
| `proto_version` | 1 | u8 | Start at 1 |
| `msg_type` | 1 | u8 | See message types below |
| `flags` | 2 | u16 | Reserved; set 0 |
| `transaction_id` | 4 | u32 | Host-chosen; echoed in reply |

All multi-byte fields are little-endian.

### Message types

| `msg_type` | Name |
|---:|---|
| 0x01 | `DISCOVER_REQ` |
| 0x02 | `DISCOVER_REPLY` |

## `DISCOVER_REQ`

Header only (no body) for v1.

Future-compatible option: allow TLVs after header; unknown TLVs ignored.

## `DISCOVER_REPLY`

Body fields (immediately after header):

| Field | Size | Type | Notes |
|---|---:|---|---|
| `uid` | 12 | bytes | STM32 unique ID (descriptor, not secret) |
| `mac` | 6 | bytes | Device MAC |
| `ip` | 4 | u32 | IPv4 (network order stored as bytes, not an integer) |
| `subnet` | 4 | u32 | IPv4 |
| `gateway` | 4 | u32 | IPv4 |
| `capabilities` | 4 | u32 | Bitfield (below) |
| `resident_version` | 4 | u32 | e.g. `0xAABBCCDD` or semver packed (TBD) |
| `app_version` | 4 | u32 | 0 if absent |

### Capabilities bitfield (suggested)

| Bit | Meaning |
|---:|---|
| 0 | device tree supported |
| 1 | programming mode supported |
| 2 | firmware signature verification supported |
| 3 | app present |
