# Protocol documentation

This folder defines the **resident bootloader control protocol** used to discover devices, query/modify settings, and enter programming mode.

## Documents

- `discovery.md`: UDP broadcast discovery request/response format
- `commands.md`: unicast UDP command channel (errors, device-tree ops, programming entry)
- `device-tree.md`: node model + LIST/GET/SET/EXECUTE/UNLOCK semantics
- `programming.md`: TCP programming mode (binary stream, chunking, finalize/verify)

## Conventions

- **Endianness**: all multi-byte integer fields are **little-endian** unless explicitly stated.
- **Strings**: UTF-8, NUL-terminated only when noted (otherwise length-prefixed).
- **Alignment**: packets are byte-packed; do not assume alignment.
- **Reliability**: UDP messages may be lost/duplicated/reordered; clients must use `transaction_id`.
- **Versioning**: `proto_version` is carried in every packet and is required for compatibility.

