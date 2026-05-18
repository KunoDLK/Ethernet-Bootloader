# Programming mode (TCP binary stream)

## Goal

Provide a reliable high-throughput channel for application firmware updates while keeping discovery/control on UDP.

## Transport

- **Protocol**: TCP
- **Port**: dynamic; read `Program/Programming TCP port` from the device tree
- **Connection**: initiated by host after `Program/State` reaches `ProgrammingReady`
- **Flow control**: host may keep a small sliding window of `PROG_DATA` frames in flight; current tooling targets about four frames.

## Entry / authorization

Programming mode is gated by the **device tree**:

1. host discovers device (`DISCOVER_REPLY`)
2. host writes `Program/State = Erasing` over UDP device-tree `SET`
3. device queues the erase on the resident programming worker and replies immediately
4. worker stops the loaded program, erases app flash, enters `ProgrammingReady`, and opens TCP
5. host polls `Program/Programming TCP port` until it is not `-1`
6. host connects via TCP and begins streaming

When programming mode is entered, the resident firmware must shut down and deactivate the main program before any app flash sectors are erased or programmed.

## Program-specific device-tree nodes

Program-specific device-tree nodes are owned by the currently running main program.

- When entering programming mode, the resident firmware removes program-specific nodes from the device tree.
- While programming mode is active, only resident-owned nodes are exposed.
- When the main program starts again, it registers its program-specific nodes.
- Newly registered program-specific nodes start at their default values unless the program restores persisted values itself.

## Resident programming nodes

- `Program/State`: read/write enum: `Erasing`, `ProgrammingReady`, `Stopped`, `Paused`, `Running`.
- `Program/Programming TCP port`: read-only integer; `-1` unless `State` is `ProgrammingReady`.
- `App`: top-level mount point for loaded program nodes.

For this implementation phase, successful programming leaves the state as `Stopped`. Running the newly programmed app is a later step.

## TCP stream framing (v1)

The TCP stream is message-framed. Each frame starts with a fixed header:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `magic` | 4 | u32 | programming stream magic (TBD) |
| `proto_version` | 1 | u8 | start at 1 |
| `frame_type` | 1 | u8 | enum below |
| `flags` | 2 | u16 | reserved |
| `seq` | 4 | u32 | monotonically increasing from 0 |
| `payload_len` | 2 | u16 | bytes after header |
| `reserved` | 2 | u16 | set 0 |

All multi-byte fields are little-endian.

Frame flag bit 1 (`0x0002`) marks a raw storage write. Raw writes are accepted for
flash transport testing and staging payloads such as bootloader binaries, but the
resident firmware skips app-image validation and does not mark the staged bytes as
a runnable application.

### Frame types

| `frame_type` | Name |
|---:|---|
| 0x01 | `PROG_HELLO` |
| 0x02 | `PROG_DATA` |
| 0x03 | `PROG_FINISH` |
| 0x04 | `PROG_ABORT` |
| 0x81 | `PROG_ACK` |
| 0x82 | `PROG_NACK` |

## `PROG_HELLO`

Sent by host immediately after TCP connect.

Payload:

| Field | Size | Type |
|---|---:|---|
| `image_size` | 4 | u32 |
| `image_sha1` | 20 | bytes |

Device responds with `PROG_ACK` or `PROG_NACK`.

## `PROG_DATA` (1 KB payload chunks)

This frame carries *firmware payload bytes* only. Your requested chunk size:

- **max firmware payload per frame**: **1024 bytes**
- the 1024 bytes **does not include** the packet header above

Payload:

| Field | Size | Type | Notes |
|---|---:|---|---|
| `data_len` | 2 | u16 | must be 1..1024 |
| `data` | N | bytes | firmware bytes |

Sequencing rules:

- `seq` increments by 1 for each frame (`HELLO` is seq 0).
- Device parses the TCP byte stream, so protocol frames may be split across TCP segments or coalesced in one receive callback.
- Device ACKs each accepted `PROG_DATA` with `PROG_ACK(seq)`.
- Host may send several sequential `PROG_DATA` frames before receiving their ACKs, bounded by its configured programming window.
- Frames are still committed in sequence; a NACK stops or retransmits the affected window rather than continuing unbounded.

## `PROG_FINISH`

Host indicates end-of-image and requests verification + commit.

Payload (suggested):

| Field | Size | Type |
|---|---:|---|
| `total_bytes` | 4 | u32 |
| `final_sha1` | 20 | bytes |

Device actions:

- verify received size matches
- verify SHA-1 (and/or signature if enabled)
- update metadata "valid" marker only after full verify succeeds
- optionally reboot or restart app

## ACK/NACK

### `PROG_ACK`

Payload:

| Field | Size | Type |
|---|---:|---|
| `ack_seq` | 4 | u32 |
| `bytes_written` | 4 | u32 |

### `PROG_NACK`

Payload:

| Field | Size | Type |
|---|---:|---|
| `nack_seq` | 4 | u32 |
| `error_code` | 2 | i16 |
| `detail` | 2 | u16 |

Suggested `error_code`:

| Code | Meaning |
|---:|---|
| -1 | generic |
| -2 | not authorized (device tree locked / not unlocked) |
| -3 | bad length |
| -4 | seq mismatch |
| -5 | flash program error |
| -6 | verify failed |

## Power-loss behavior

Single-slot in-place updates can leave the main program incomplete if power is lost during flash erase/programming.

Required behavior:

- keep resident in protected sectors (never erased)
- shut down and deactivate the main program before programming begins
- only mark the app valid after the full image has been received and verified
- refuse to start app if invalid
- if power is lost mid-flash, the firmware update must be attempted again and fully completed before the app can run

