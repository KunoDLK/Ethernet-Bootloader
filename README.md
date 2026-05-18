# STM32F4 Ethernet Resident Bootloader Spec (UDP Broadcast)

This repository targets an STM32F407 + LAN8720 design where a **resident firmware** owns Ethernet (lwIP) and the RTOS, and the **main application runs as RTOS task(s)** started by the resident firmware.

The resident firmware exposes a UDP server for:

- device discovery (broadcast)
- configuration/device tree access
- entry into firmware update mode (protected via device tree)
- in-place single-slot firmware update of the application region

## Initial bootloader flash (first-time / factory programming)

Because the **resident firmware is always present** and is responsible for networking + updates, you need an **initial programming step** before any UDP discovery/update workflow can work.

### What gets flashed first

- **Resident firmware image**: programmed via SWD/JTAG (ST-LINK, J-Link, etc.) into the “resident” flash region.
- **Application image** (optional at factory): can be flashed initially via SWD as well, or left blank and installed later via the resident updater.
- **Metadata / settings defaults**: either compiled into resident defaults and persisted on first boot, or written as part of factory provisioning.

### One-time MCU configuration to consider

- **Flash layout**: define and document the sector map for:
  - resident (protected, never erased by updater)
  - application region (updater erases/programs here)
  - metadata/settings region (if stored in flash)
- **Option bytes / protection** (depending on your threat model and debug needs):
  - consider **write-protecting** the resident sectors so field updates cannot erase them
  - decide on **RDP level** policy (development vs production)
  - ensure any protection choices still allow updating the application region as intended

### Provisioning items needed for network bring-up

At minimum, ensure the device has a valid:

- **MAC address**: ideally unique per device (from EEPROM/OTP/external label, or derived deterministically with a managed OUI scheme)
- **IP configuration defaults**: e.g. safe static default, or a “first boot” behavior that forces known settings
- **PSK / credentials**: per-device PSK recommended; at minimum a product-line PSK that can be rotated via a controlled process

### Validation checklist (factory bring-up)

- Resident boots and keeps `SCB->VTOR` pointed at resident vector table
- LAN8720 link comes up; lwIP stack starts
- Host receives `DISCOVER_REPLY` on the LAN broadcast domain
- Device tree is readable; protected nodes enforce locking
- App start/stop works (if an app is present)

## High-level architecture

- **Resident firmware (always present)**:
  - initializes clocks, ETH MAC/DMA, LAN8720 PHY, lwIP, and RTOS
  - owns interrupt vector table (`SCB->VTOR` stays pointed at resident image)
  - runs UDP control server (discovery/config/update)
  - manages flash erase/program/verify and metadata
  - starts the main program as module/task(s) (does not VTOR-jump into the app reset handler)
- **Main program (application module)**:
  - is started via an explicit entrypoint (e.g. `app_start(const AppApi* api)`)
  - uses resident-provided services via an API table
  - may register an app settings provider so its subtree is accessible via the resident server

## Ethernet + RTOS ownership rules (Cortex-M implications)

- Resident firmware owns RTOS exception/interrupt usage:
  - SysTick / PendSV / SVC (RTOS)
  - ETH IRQ + DMA
  - other “global” interrupts that must remain consistent
- The application must not assume it owns the machine “from reset.”
  - peripherals/IRQs either remain resident-owned, or are granted via explicit registration/dispatch.

## Network model

- **Static IP** is stored in the device tree and applied by the resident firmware.
- Discovery and configuration are designed to work even when the host does **not** know the device’s current IP.
- Discovery scope requirement: **host and device are on the same L2 broadcast domain** (same switch/VLAN/subnet). UDP broadcast discovery is not routed by default.

## Device discovery mechanism (cross-platform)

Goal: **all devices respond** to a broadcast discovery request.

### Transport

- UDP on a fixed `DISCOVERY_PORT` (TBD).

### Host behavior (Windows / macOS / Linux, user space)

- Create a UDP socket and enable broadcast (`SO_BROADCAST`).
- Send a `DISCOVER` datagram to:
  - limited broadcast `255.255.255.255:DISCOVERY_PORT`
  - each interface’s directed broadcast (e.g. `192.168.1.255:DISCOVERY_PORT`)
- Listen for replies for a short time window (e.g. 200–500 ms), optionally repeat 2–3 times.
- Receive replies using `recvfrom()` (or equivalent).
  - Each datagram includes **source IP address and source UDP port**.
  - Binding the socket to `0.0.0.0` does **not** remove sender context; replies remain attributable per sender.

### Device behavior (lwIP)

- Bind a UDP PCB to `0.0.0.0:DISCOVERY_PORT` so it receives datagrams addressed to:
  - `255.255.255.255:DISCOVERY_PORT` (limited broadcast)
  - directed broadcast for its subnet (if the host sends it)
- On receiving `DISCOVER`, reply **unicast** to the sender’s `(src_ip, src_port)` with `DISCOVER_REPLY`.

### Reply storm mitigation

When many devices exist on one LAN:

- add a small randomized delay before replying (e.g. 0–50 ms)
- rate limit responses (e.g. at most 1 reply/sec per sender)

### Suggested minimal packet fields

All messages should include at least:

- `magic` (4 bytes)
- `proto_version` (1 byte)
- `msg_type` (1 byte)
- `transaction_id` (4 bytes, echoed in replies)

`DISCOVER_REPLY` additionally includes:

- `uid` (STM32 unique ID; **descriptor, not secret**)
- `mac` (6 bytes)
- current `ip` / `subnet` / `gateway`
- firmware versions + capabilities bitfield

## Security model (private-network + device tree locks)

This project assumes a trusted/private network environment. Privileged operations are guarded by **device tree password protection** (locked nodes and subtrees).

### Firmware integrity/authenticity

- Minimum: compute **SHA-256** over the incoming firmware image and verify before marking valid.
- Recommended for field devices: **signed firmware verification** (ECDSA/Ed25519) in the resident firmware.

## Firmware update model (single-slot, in-place)

Constraints:

- STM32F407 has single-bank flash; do not erase/program sectors containing currently executing code.
- Updates are **single-slot in-place** overwrites of the app region.
- During update, resident firmware will **pause/stop app tasks** (recommended) to ensure no app code is executing while its flash is modified.

### Power-loss behavior

Single-slot overwrites can corrupt the only app image if power is lost mid-update.

Mitigations:

- resident firmware must remain bootable at all times (in protected sectors)
- write metadata only after a full verify succeeds
- maintain a “last-known-good / valid” marker; refuse to start the app if verification fails

## Unified device tree (node model)

The resident firmware exposes a unified **node tree** that includes standard descriptors and app-specific settings.

### Node structure

- Each node has a **name** and either:
  - a **scalar value** (leaf), or
  - **children nodes** (container)
- Container nodes:
  - do not have a scalar value
  - have `has_children = true`
- A node “value” can conceptually be other nodes (i.e., containers).

### Access mode per node

All nodes share one of these modes:

- `Read` (read-only)
- `write_unrestricted` (writable; takes effect immediately / no reboot required)
- `write_reboot_required` (writable; persisted but takes effect after reboot)
- `execute` (action; invoked to perform a command like reboot)

### Password protection (hierarchical locking)

- Any node may be marked **password protected**.
- If a protected node is **locked**, then **all descendant nodes are locked** until the parent node is unlocked.
- Any number of nodes may be protected simultaneously (independent lock domains).

### Always accessible

The node tree must be accessible at any time via the resident server (not only during update mode).

### Protocol operations (conceptual)

- `LIST(path)`:
  - returns children and per-node metadata: `has_children`, access mode, locked/unlocked state
- `GET(path)`:
  - returns scalar value (error if container)
- `SET(path,value)`:
  - writes scalar value if permitted; returns whether reboot is required
- `UNLOCK(path, credential_proof)`:
  - unlocks a protected node (and therefore its subtree) for a session/time window
- `LOCK(path)` (optional):
  - re-lock early
- `GET_SCHEMA(path)` (optional):
  - returns richer types/ranges/enum info for tooling/UI

### Standard descriptors vs app-specific

- **Standard descriptors** (resident-defined), e.g.:
  - `device/uid`, `device/mac`, `device/hw_rev`
  - `net/ip`, `net/subnet`, `net/gateway`
  - `fw/resident_version`, `fw/app_version`
  - `boot/proto_version`, `boot/capabilities`
- **App-specific subtree**:
  - e.g. `app/`*
  - provided/registered by the application module through the resident’s app API.

## Open items / TBD

- Port numbers (`DISCOVERY_PORT`, control/update ports)
- Exact on-wire binary message formats (endianness, alignment, max size)
- LAN8720 RMII clocking (PHY crystal vs MCU-provided REF_CLK) and pin mapping
- Flash sector map and exact memory layout for resident/app/metadata

## CMake build setup

This repository now includes a CMake workflow for the app images:

- `Firmware/Application` -> `example_app.elf/.bin/.appimg`
- `Firmware/IapApp` -> `iap_app.elf/.bin/.appimg`

### Prerequisites

- CMake 3.22+
- NMake (Visual Studio Build Tools) or another CMake generator you prefer
- ARM GNU toolchain in `PATH`:
  - `arm-none-eabi-gcc`
  - `arm-none-eabi-objcopy`
  - `arm-none-eabi-nm`
  - `arm-none-eabi-size`
- Python 3

If the toolchain is not in `PATH`, set `ARM_GCC_ROOT` to the install root (the directory that contains `bin/arm-none-eabi-gcc`).

### Configure and build

From repo root:

```bash
cmake --preset arm-debug
cmake --build --preset build-debug
```

If `NMake` is unavailable in your shell, use the VS generator preset:

```bash
cmake --preset arm-vs2022
cmake --build --preset build-vs2022
```

Release build:

```bash
cmake --preset arm-release
cmake --build --preset build-release
```