# Device Tree

## Overview

The resident firmware exposes a unified hierarchical **device tree** of nodes.

- resident-defined subtrees: `Network/*`, `Program/*`, `Hardware/*`, `Debug/*`, `Reboot`, ...
- application subtree: `App/*` (mounted by app via resident API)

Human-readable docs may describe paths like `net/ip` or `boot/reboot`, but packets address nodes by compact **node location bytes**.

## Node kinds

Each node is either:

- **container**: has children, no scalar value
- **leaf**: UTF-8 text value interpreted by firmware
- **action**: executable operation (no scalar value)

All value fields are encoded as UTF-8 text. The firmware interprets the text according to the node. For example, a motor current of `1.35 A` is sent as the text value `"1.35"`; units and descriptions are node metadata.

## Access byte

Every returned node includes a single `access` byte. The high nibble describes the access the node supports when unlocked; the low nibble describes access currently available without another unlock.

```text
bits 7..4: unlocked/supported access
bits 3..0: current access
```

Each nibble uses the same bit layout:

| Bit | Mask in nibble | Meaning |
|---:|---:|---|
| 3 | `0b1000` | read/list children |
| 2 | `0b0100` | write |
| 1 | `0b0010` | execute |
| 0 | `0b0001` | reserved; set 0 |

Example: `11101000`

- unlocked/supported access: `1110` = read + write + execute when unlocked
- current access: `1000` = read currently allowed without unlock
- meaning: the node can be read now, but write/execute requires unlock

### Write behavior

Whether a write takes effect immediately or requires reboot can be reported in the node JSON metadata (for example `"WriteEffect":"reboot_required"`).

## Locking (hierarchical password protection)

Nodes may be marked **password protected**.

- if a protected node is locked for **read**, its subnodes cannot be listed or read.
- if a protected node is locked for write/execute, descendants may still be readable if the read bit is currently set.
- `UNLOCK` is a device-tree operation that supplies a password for a node (unlocking its subtree).
- device may re-lock automatically after a short timeout (recommended).

## Application / program-specific nodes

Program-specific nodes are owned by the currently running main program and are mounted under `App/*`.

- The resident firmware owns core nodes such as `Network/*`, `Program/*`, `Hardware/*`, `Debug/*`, `Reboot`, and `App`.
- The main program registers its own nodes when it starts.
- When entering programming mode, the resident firmware shuts down/deactivates the main program and removes program-specific nodes from the device tree.
- While programming mode is active, only resident-owned nodes are exposed.
- When the main program starts again, it adds its program-specific nodes again.
- Program-specific nodes start at their default values unless the program restores persisted values itself.

## Programming control nodes

`Program` is a resident-owned top-level node with:

- `State`: read/write enum with values `Erasing`, `ProgrammingReady`, `Stopped`, `Paused`, `Running`.
- `Programming TCP port`: read-only integer. It is `-1` unless `State` is `ProgrammingReady`, then it is the active TCP listener port.

Writing `Program/State = Erasing` queues asynchronous work on the resident programming worker and returns to the UDP caller immediately. The worker stops the loaded program, erases the app flash sector range, opens the TCP programming listener, and transitions to `ProgrammingReady`. Successful programming currently leaves the device in `Stopped`.

`App` is always present as a top-level mount point. Loaded program nodes appear beneath it only while a program is mounted.

## Operations

All operations are carried inside `DEVICETREE_REQ`/`DEVICETREE_REPLY` command messages.

### `DEVICETREE_REQ` body

| Field | Size | Type | Notes |
|---|---:|---|---|
| `op` | 1 | u8 | opcode; for `LIST`, bit 7 enables paging |
| `node_depth` | 1 | u8 | number of node location bytes after this field |
| `node_location` | N | bytes | one byte per tree level |
| `payload_len` | 2 | u16 | bytes of op-specific payload |
| `payload` | M | bytes | op-specific |

### `DEVICETREE_REPLY` body

The response mirrors the request addressing first, then appends operation result data. The request payload is not echoed.

| Field | Size | Type | Notes |
|---|---:|---|---|
| `op` | 1 | u8 | base opcode; for `LIST`, bit 7 means `has_more` |
| `node_depth` | 1 | u8 | copied from request |
| `node_location` | N | bytes | copied from request |
| `result` | 2 | i16 | 0 = OK; negative = error |
| `response_payload_len` | 2 | u16 | bytes of operation-specific response data |
| `response_payload` | R | bytes | operation-specific response data |

### Node location encoding

`node_depth` tells the device how many bytes define the target node location.

- `node_depth = 0`: target is the root level. `LIST` returns all top-level nodes.
- `node_depth = N`: exactly `N` bytes follow, one node ID per level.
- Requests only operate on the specified node level. To inspect subnodes, issue a separate `LIST` request for that child node.

Example:

```text
03 01 0A 02
```

Meaning:

- `03`: three levels after this byte
- `01`: top-level node ID 1
- `0A`: child node ID 10 under node 1
- `02`: child node ID 2 under node 10

`op` encoding:

- Bits 6:0 (`op & 0x7f`) are the base opcode.
- On `LIST` requests, bit 7 (`0x80`) means the payload contains a one-byte `start_after` cursor.
- On `LIST` replies, bit 7 (`0x80`) means `has_more`: the reply was truncated at an item boundary and the host should request another page.
- For other replies, bit 7 is cleared.

Base `op` values:

| `op` | Name |
|---:|---|
| 0x01 | LIST |
| 0x02 | GET |
| 0x03 | SET |
| 0x04 | EXECUTE |
| 0x05 | UNLOCK |
| 0x06 | LOCK (optional) |

### LIST

Non-paged request payload: empty.

Paged request payload (`op = 0x81`):

| Field | Size | Type | Notes |
|---|---:|---|---|
| `start_after` | 1 | u8 | resume after the child whose `node_id` matches this cursor in firmware wire order; use 0 for the first page |

`LIST` returns only the direct children of the specified node location.

- `node_depth = 0`: returns top-level nodes only.
- `node_depth = 1`: returns direct children under that top-level node.
- deeper subnodes require separate requests.

Large replies may be truncated before `TREE_RESPONSE_MAX`; in that case the reply `op` is `0x81`.
The host should start with `start_after = 0`, then repeat while `has_more` is set using the last emitted child `node_id` as the next `start_after`.
Ordering follows the firmware's emitted sibling order, not a separate numeric sort.

The resident debug Flash subtree exposes stored KV entries generically: each KV child uses the raw key as a hex `Name` (for example `0x00000008`) and returns raw value bytes as hex text. The bootloader does not apply schema-specific formatting for these debug KV nodes.

## Raw KV Settings Store

Resident settings are persisted by a raw KV settings store. The store has no knowledge of application or resident value types; callers own all conversion between bytes and higher-level formats such as IPv4 text, enums, integers, or JSON.

The on-flash settings object keeps the existing indexed append model and integrity metadata, but the payload is a single raw entry list:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `record_total` | 4 | u32 | total bytes including CRC |
| `magic` | 4 | u32 | raw KV object marker |
| `version` | 4 | u32 | settings object version |
| `entries_len` | 4 | u32 | bytes in `entries[]` |
| `entries[]` | var | raw KV entries | packed sequentially |
| `crc32` | 4 | u32 | CRC over all previous bytes |

Each raw entry is:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `key` | 4 | u32 | caller-defined key/node ID |
| `value_len` | 1 | u8 | bytes in `value` |
| `value` | N | bytes | opaque payload |

JSON values are stored as UTF-8 text bytes in `value`, with `value_len` giving the text byte length. The store does not parse JSON.

The store initializes by finding and validating the active settings object. If no valid object exists, resident boot clears the sector and seeds defaults at the resident layer before saving. The store caches only the active settings object pointer; reads re-scan the active object each time instead of keeping a separate read cache. Writes update in-memory entries only; callers must call `SaveToFlash` to commit a group of updates in one flash append.

Reply payload:

| Field | Size | Type |
|---|---:|---|
| `count` | 2 | u16 |
| `items[]` | var | list items |

List item format:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `node_Id` | 1 | u8 | ID used in `node_location` for this level |
| `has_children` | 1 | u8 | 0/1 |
| `access` | 1 | u8 | packed access byte |
| `DataLengthBytes` | 2 | u16 | bytes of UTF-8 JSON metadata |
| `Data` | N | bytes | UTF-8 JSON object |

The JSON object must include at least:

| Property | Type | Meaning |
|---|---|---|
| `Name` | string | human-readable node name |
| `Value` | string | current value as UTF-8 text, or empty string for containers/actions |

Additional properties may be added freely. Suggested properties:

| Property | Type | Meaning |
|---|---|---|
| `Units` | string | display/interpretation units, e.g. `"A"`, `"V"`, `"ms"` |
| `Description` | string | human-readable help text |
| `Uses_Nodes` | array | related node IDs used by this node, e.g. `[123,124]` |

Example list item `Data`:

```json
{"Name":"Motor Current","Value":"1.35","Units":"A","Description":"Configured motor current limit","Uses_Nodes":[123,124]}
```

### GET

Request payload: empty.

Reply payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `value_len` | 2 | u16 | bytes of UTF-8 value text |
| `value` | N | bytes | UTF-8 text interpreted by firmware |

### SET

Request payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `value_len` | 2 | u16 | bytes of UTF-8 value text |
| `value` | N | bytes | UTF-8 text interpreted by firmware |

Reply payload:

| Field | Size | Type |
|---|---:|---|
| `reboot_required` | 1 | u8 |

### EXECUTE

Request payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `args_len` | 2 | u16 | |
| `args` | N | bytes | opaque to protocol; action-defined |

Reply payload: empty (result code conveys status).

### UNLOCK

The target node is selected by the common `node_depth` + `node_location` fields in `DEVICETREE_REQ`.

Request payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `options` | 1 | u8 | see options below |
| `password_len` | 1 | u8 | |
| `password` | N | bytes | plaintext UTF-8; not NUL-terminated |

Unlock options:

| Bit | Meaning |
|---:|---|
| 0 | remove password requirement from this node after successful password check |
| 1..7 | reserved; set 0 |

If the password is incorrect:

- the device replies immediately with an error result.
- the device rejects any further `UNLOCK` requests for the next 1 second.
- other non-UNLOCK commands may still be processed according to normal access rules.

Reply payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `access_result` | 1 | u8 | high nibble = access bits just unlocked; low nibble = status flags |

`access_result` uses the same nibble bit layout as the normal access byte:

```text
bits 7..4: access bits that changed from locked to unlocked because of this command
bits 3..0: status flags
```

Low-nibble status flags:

| Bit | Meaning |
|---:|---|
| 0 | password accepted |
| 1 | node was already unlocked before this request |
| 2..3 | reserved; set 0 |

## Recommended standard nodes (starting set)

- `device/uid` (Read, UTF-8 hex string)
- `device/mac` (Read, UTF-8 MAC string)
- `net/ip` (write_reboot_required, UTF-8 IPv4 string)
- `net/subnet` (write_reboot_required, UTF-8 IPv4 string)
- `net/gateway` (write_reboot_required, UTF-8 IPv4 string)
- `fw/resident_version` (Read, UTF-8 version string)
- `fw/app_version` (Read, UTF-8 version string)
- `boot/reboot` (**execute**, action; password protected)

