## Multi-NIC discovery + automatic control failover plan

### Goals
- Remove the need for the user to pick a single adapter for all traffic.
- Allow **discovery** to run on multiple adapters (Ethernet + Wi‑Fi) concurrently.
- Ensure **control** packets (device-tree GET/LIST/SET/EXEC) are sent **unicast** and use the OS-chosen source IP/interface for the target device.
- Make “**MAC changed → IP changed → rediscover and reconnect**” automatic by treating **UID** as the stable device identity.
- Preserve the existing UID-wrapped protocol behavior, but limit broadcast usage to cases where it’s actually valid/helpful.

### Current behavior (what we’re changing)
In `tools/bootloader_cli.py`:
- `DiscoveryWorker` binds one UDP socket to `bind_ip` and sends discovery requests to `target` (default `255.255.255.255`).
- Control requests in `send_device_tree_request()` also bind to `bind_ip`.
- `device_control_target()` returns `fallback_target` (often broadcast) whenever the device IP is not in the selected interface subnet:
  - This breaks cross-subnet control even when routing is available.

### Design

#### Discovery: multi-adapter
- Create one `DiscoveryWorker` per local interface IP (from `list_network_interfaces()`), each bound to its own `bind_ip`.
- Each worker sends discovery probes on its own interface.
- Merge discovered devices into a single view keyed by **device identity**:
  - Prefer `uid` (stable across MAC changes).
  - Fall back to `ip+mac` only if UID is missing/zero.
- Track per-device **endpoints** (multiple may exist):
  - Each endpoint record should include at least: `ip`, `last_seen`, and which local `bind_ip`/adapter observed it.

#### Control: automatic per-destination source selection (no user adapter selection)
- Add a helper that asks the OS routing table which local IP should be used for a given destination IP:
  - Create a UDP socket.
  - `connect((dest_ip, CONTROL_PORT))` (no packets sent).
  - `getsockname()` yields the chosen local IP.
- For each control request:
  - Choose the control destination as **unicast** to a chosen device endpoint IP.
  - Choose `bind_ip` via the routing helper, then send from that interface.

#### Broadcast usage policy
- Keep broadcast only for:
  - Discovery (`PROTO_MSG_DISCOVER_REQ` to `--target`), because that’s intentionally link-local.
  - True ambiguity cases (two devices share the same IP, or local bind equals device IP) *and only when the destination is on-link for that interface*.
- Remove the current behavior where “off-subnet” implies “broadcast”.

#### MAC-change rediscovery workflow (key requirement)
Because the device **UID remains stable** and MAC is updated via device-tree SET + reboot:
- After writing MAC-related nodes, the CLI should:
  - Mark the currently selected device UID as “**pending rediscovery**”.
  - Keep discovery active on all adapters (or temporarily increase refresh rate for a short period).
  - Prefer to keep the UI focused on that same UID, even if its IP disappears.
- When a discovery reply arrives with the same UID but a different IP:
  - Update that device’s “active endpoint” to the new IP.
  - Clear “pending rediscovery” status and resume normal control.
- Additionally, keep the existing behavior where control replies update cached IP from the reply source (today `mark_device_seen()` calls `worker.patch_device_network(uid, ip=source_ip)`), but adjust it to update the endpoint set (not a single IP field) once multi-endpoint support exists.

#### Failover behavior (Ethernet unplug)
- Control send path uses OS route to the endpoint IP.
- If Ethernet is unplugged and the Ethernet endpoint stops responding:
  - Try other known endpoints for that same UID (e.g., Wi‑Fi IP).
  - If all fail, rely on ongoing discovery to repopulate endpoints.

### Concrete file changes
`tools/bootloader_cli.py`
- Add a small routing helper, e.g. `route_source_ip_for(dest_ip: str) -> str`.
- Refactor discovery into either:
  - `DiscoveryWorker` (single bind IP) + `DiscoveryManager` (runs one worker per interface, merges snapshots), or
  - Extend `DiscoveryWorker` to accept a list of bind IPs.
- Replace the “interface_select” mode (or demote it to informational only). The user should not have to choose a control adapter.
- Update control send functions (`send_device_tree_request()`, `set_device_tree_value()`, `execute_device_tree_node()`, `list_device_tree_node()`, `get_device_tree_value()`):
  - Stop requiring a user-selected `bind_ip`.
  - Determine the bind/source IP per destination endpoint.
- Replace `device_control_target()` logic:
  - Stop returning broadcast purely because `ip_in_subnet(...)` is false.
  - Keep ambiguity handling but gate broadcast to on-link only.
- Add “pending rediscovery” state keyed by UID:
  - Set it when MAC is changed.
  - Clear it when the device is discovered again (same UID, new IP).

### Test plan (manual)
- Same-subnet Ethernet: discover + control works.
- Same-subnet Wi‑Fi: discover + control works.
- Dual-endpoint device (Ethernet + Wi‑Fi up): one device identity with two endpoints; control uses the working one.
- MAC change via device-tree + reboot:
  - Device disappears briefly, then reappears with new IP.
  - UI stays associated with same UID and control resumes on the new IP automatically.
- Ethernet unplug after prior Ethernet discovery:
  - Control retries via Wi‑Fi endpoint automatically.

### Notes on multicast “remote subnet scanning”
- Multicast discovery across subnets requires multicast routing (IGMP/PIM) and is usually not enabled between VLANs/Wi‑Fi by default.
- This plan avoids relying on that by using multi-adapter local discovery + unicast control with OS routing.

