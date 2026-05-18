# Commands (UDP unicast + broadcast control plane)

## Goal

Provide a broadcast-safe discovery mechanism plus a **unicast** control plane for:

- device tree access (LIST/GET/SET/EXECUTE/UNLOCK/LOCK)
- entry into programming mode through device-tree writes
- status queries

## Transport

- **Protocol**: UDP
- **Port**: `CONTROL_PORT` (TBD; fixed per firmware build)
- **Destination**:
  - **unicast** to the device IP learned from discovery (normal case)
  - **broadcast** to reach devices whose IP/subnet config does not match the host (same L2 domain only)

### Broadcast command transport (target by UID)

When the host cannot reliably unicast to a device (e.g. device configured for a different IPv4 subnet), it may send **standard commands over UDP broadcast** and target a specific device by UID.

- **Broadcast destination**: `255.255.255.255:CONTROL_PORT` (and/or interface-directed broadcast)
- **Device behavior**:
  - Devices receive broadcast frames on `CONTROL_PORT`
  - Only the device whose `uid` matches the packet target UID processes the inner command
  - The device replies **unicast** to `(src_ip, src_port)` with the corresponding reply type
- **Security**: same as unicast; privileged operations are still guarded by device-tree locks/passwords

## Envelope (common)


| Field            | Size | Type | Notes                                      |
| ---------------- | ---- | ---- | ------------------------------------------ |
| `magic`          | 4    | u32  | Same as discovery                          |
| `proto_version`  | 1    | u8   | Start at 1                                 |
| `msg_type`       | 1    | u8   | See below                                  |
| `flags`          | 2    | u16  | Bit0: request(0)/reply(1); others reserved |
| `transaction_id` | 4    | u32  | Host-chosen; echoed in reply               |
| `payload_len`    | 2    | u16  | Bytes after header                         |
| `reserved`       | 2    | u16  | Set 0                                      |


All multi-byte fields are little-endian.

### Message types (v1)


| `msg_type` | Name               | Notes                                                       |
| ---------- | ------------------ | ----------------------------------------------------------- |
| 0x12       | `BCAST_UID_REQ`    | broadcast wrapper targeting a UID; carries an inner request |
| 0x13       | `BCAST_UID_REPLY`  | wrapper for broadcast-transport replies                     |
| 0x20       | `DEVICETREE_REQ`   | carries op + node location + value                          |
| 0x21       | `DEVICETREE_REPLY` | carries result + data                                       |
| 0x40       | `PING_REQ`         | liveness                                                    |
| 0x41       | `PING_REPLY`       | liveness                                                    |
| 0x7F       | `ERROR_REPLY`      | generic error when parsing fails                            |


## Broadcast wrapper format (`BCAST_UID_REQ` / `BCAST_UID_REPLY`)

These message types allow transporting any existing request/reply over broadcast while targeting a single device by UID.

### `BCAST_UID_REQ` payload


| Field            | Size | Type  | Notes                                               |
| ---------------- | ---- | ----- | --------------------------------------------------- |
| `target_uid`     | 12   | bytes | STM32 UID as advertised in discovery                |
| `inner_msg_type` | 1    | u8    | e.g. `DEVICETREE_REQ`, `PING_REQ` |
| `inner_len`      | 2    | u16   | bytes of `inner_payload`                            |
| `inner_payload`  | N    | bytes | exact payload of the inner request                  |


The outer envelope `transaction_id` SHOULD be copied into the inner request’s `transaction_id` (or the receiver may ignore the inner one and use the outer). Pick one rule and implement consistently.

### `BCAST_UID_REPLY` payload


| Field            | Size | Type  | Notes                                              |
| ---------------- | ---- | ----- | -------------------------------------------------- |
| `target_uid`     | 12   | bytes | echoed UID                                         |
| `inner_msg_type` | 1    | u8    | corresponding reply type (e.g. `DEVICETREE_REPLY`) |
| `inner_len`      | 2    | u16   | bytes of `inner_payload`                           |
| `inner_payload`  | N    | bytes | exact payload of the inner reply                   |


### Notes / constraints

- Hosts should retry broadcast requests (UDP loss) and use `transaction_id` to de-dup replies.
- Devices should apply reply jitter (small random delay) to reduce collisions if many hosts broadcast simultaneously.

## Authorization model (device-tree password protection)

This protocol assumes operation on a trusted/private network. Privileged functions are protected by **locked nodes** in the **device tree**.

- A node (and its descendants) may be **locked**.
- Host must `UNLOCK(path, password)` before it can `SET` or `EXECUTE` on protected nodes.
- Device may re-lock automatically after a short timeout (recommended).

## Programming entry

Programming entry is device-tree-only. The host writes `Program/State = Erasing`, then polls `Program/Programming TCP port` until it is not `-1`. Bulk image bytes are then sent over the reported TCP port in framed `PROG_DATA` messages. The current host may keep a small window of sequential frames in flight, while the firmware reassembles the TCP byte stream and ACKs each accepted frame by sequence number.

The host must preflight the programming image size against the application storage slot before requesting erase. On the current flash map the application storage slot is 640 KiB starting at `0x08040000`.

For transport and staging tests, the host may set the programming stream raw-storage flag when sending a `.bin` payload such as the 256 KiB bootloader binary. Raw-storage writes still erase and program the app storage area, but they skip app-image validation and are not marked as runnable applications.

## Error model

### Standard result codes (suggested)


| Code | Meaning                     |
| ---- | --------------------------- |
| 0    | OK                          |
| -1   | generic error               |
| -2   | parse error / malformed     |
| -3   | unsupported proto_version   |
| -4   | unauthorized / bad password |
| -5   | locked node                 |
| -6   | not found                   |
| -7   | invalid type/value          |
| -8   | busy                        |


### `ERROR_REPLY`

Used when the device cannot parse enough to emit the specific reply type.

Body:


| Field    | Size | Type |
| -------- | ---- | ---- |
| `result` | 2    | i16  |
| `detail` | 2    | u16  |


`detail` is implementation-defined (e.g. parser state).

## Ping

`PING_REQ` has empty body. `PING_REPLY` body:


| Field              | Size | Type | Notes |
| ------------------ | ---- | ---- | ----- |
| `uptime_ms`        | 4    | u32  |       |
| `resident_version` | 4    | u32  |       |
| `app_version`      | 4    | u32  |       |
