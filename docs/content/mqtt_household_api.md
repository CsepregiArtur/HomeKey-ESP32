---
title: "MQTT Household API"
weight: 7
---

# MQTT Household API

> [!NOTE]
> **Fork-only API.** Upstream exposes only the flat, single-device topics documented
> in [MQTT](mqtt). Everything on this page is **additive** and specific to this fork;
> upstream firmware does not implement it. See [Fork vs Upstream](fork-vs-upstream).

Reference for the structured, multi-node MQTT namespace introduced by the
Household architecture. Legacy single-device topics (prefixed by the MQTT client
id) are unchanged and documented in [MQTT](mqtt).

## Namespace

When a node is enrolled in a household (`household_id` and `node_id` set), the
node publishes and subscribes under:

```
homekey/household/<household_id>/nodes/<node_id>/
```

Unenrolled nodes continue to use only the legacy `<CLIENT_ID>/...` topics.

## Topic reference

Legend: **dir** = PUB (node publishes) / SUB (node subscribes).

### Telemetry (read-only)

| Topic | dir | Retained | QoS | Payload | Notes |
|---|---|---|---|---|---|
| `.../state` | PUB | yes | 0 | JSON | node id/name/role/state/generation/firmware |
| `.../status` | PUB | yes | 1 | `online` / `offline` | presence (LWT-backed) |
| `.../health` | PUB | no | 0 | JSON | network/mqtt/nfc/lock/backup/cert/firmware/uptime/heap/reset/security |
| `.../security` | PUB | yes | 0 | `OK` / `WARNING` / `ERROR` | compact security state; **no numeric score** |
| `.../backup/status` | PUB | yes | 0 | `completed` / `failed` | last backup outcome |
| `.../backup/last` | PUB | yes | 0 | JSON | `{status, timestamp}` — metadata only, never backup contents |
| `.../last_auth` | PUB | yes | 0 | JSON | `{type, result, timestamp}` — safe metadata only |

> [!NOTE]
> `.../events` is **RESERVED / NOT IMPLEMENTED** (see below) and is not listed as
> a telemetry topic.

Example `.../state`:

```json
{"household_id":"HOUSE-7F42","node_id":"GATE-001","node_name":"Gate",
 "node_role":"gate","node_state":"ACTIVE","generation":1,
 "firmware_version":"0.10.0"}
```

Example `.../health`:

```json
{"network":"UNKNOWN","mqtt":"OK","mqtt_error":0,"nfc":"OK",
 "lock_current":1,"lock_target":1,"backup":"ok","certificate":"unknown",
 "firmware_version":"0.10.0","uptime":1234,"free_heap":123456,
 "reset_reason":"1","security":{"all_ok":false,"warnings":"..."}}
```

> [!NOTE]
> Unwired health fields: `network` is always `"UNKNOWN"` and `certificate` is
> always `"unknown"` in the current firmware (no reliable source is wired). The
> remaining fields are real and derived at snapshot time.

Example `.../security`:

```text
OK
```

(`WARNING` when any finding is not OK; `ERROR` is reserved. No numeric score.)

Example `.../backup/last` (metadata only — the encrypted backup is **never**
published to this or any other topic):

```json
{"status":"completed","timestamp":1760000000}
```

Example `.../last_auth` (safe metadata only — no credential identifiers, raw
APDU, cryptographic material or HomeKey secrets):

```json
{"type":"HomeKey","result":"SUCCESS","timestamp":1760000000}
```

### Commands (security-critical — authoritative for HA V2)

| Topic | dir | Retained | QoS | Authentication | Payload |
|---|---|---|---|---|---|
| `.../command/lock` | SUB | no | 1 | HMAC-SHA256 | JSON (see below) |
| `.../command/unlock` | SUB | no | 1 | HMAC-SHA256 | JSON (see below) |

Authenticated command payload:

```json
{"ts": 1760000000, "nonce": "<nonce>", "req_id": "<request-id>", "mac": "<64 hex chars>"}
```

There is **no `action` field in the payload**. The action is derived from the
MQTT topic (`.../command/lock` → `lock`, `.../command/unlock` → `unlock`); any
`action` field in the payload is ignored. The MAC binds the topic-derived action:

```
lock:   mac = HMAC-SHA256(key, "{ts}{nonce}{req_id}lock")
unlock: mac = HMAC-SHA256(key, "{ts}{nonce}{req_id}unlock")
```

`ts` is the decimal unix timestamp and `key` is the household command key derived
as `BLAKE2b("HK-HOUSEHOLD-CMD-v1", recovery_secret || salt)`.

Verification is **fail-closed**:

- MAC must match (constant-time comparison).
- `ts` must be within ±300 s of the device clock (when a wall clock is available).
- `nonce` must not have been seen before (bounded 32-entry replay window).
- A missing/invalid field or a replay is rejected; the command is never executed.
- A plain `unlock=true` payload is **never** treated as authorization.

> [!IMPORTANT]
> These two topics are the **authoritative household lock/unlock API for HA V2**.
> The legacy `<CLIENT_ID>/homekit/set_*` topics remain functional but are
> **legacy/internal** and must not be used by the household integration (see
> below). There is no MQTT Discovery entity for the HMAC command — the HA V2
> integration publishes to these topics directly.

### Reserved / not implemented

The following topics are **RESERVED / NOT IMPLEMENTED**. The current firmware
neither publishes nor subscribes to them, and HA V2 must not depend on them.

| Topic | Status |
|---|---|
| `.../events` | RESERVED / NOT IMPLEMENTED |
| `.../backup/request` | RESERVED / NOT IMPLEMENTED |
| `.../backup/data` | RESERVED / NOT IMPLEMENTED |
| `.../restore/request` | RESERVED / NOT IMPLEMENTED |
| `.../restore/status` | RESERVED / NOT IMPLEMENTED |

Backup creation, restore and audit are **HTTP-only** endpoints
(`POST /backup/create`, `POST /backup/restore`, `GET /audit`); they are not
exposed over MQTT. Backup blobs are AEAD-encrypted (XChaCha20-Poly1305) and
Ed25519-signed before they leave the device. Plaintext private keys or HomeKey
secrets are never sent over MQTT.

## Home Assistant MQTT discovery

Enrollment publishes the household entities below (besides the existing legacy
lock/tag entities). **Each entity has its own discovery config topic** so Home
Assistant does not collapse them onto a single entity slot. Unique ids are
stable — `<household_id>_<node_id>_<entity>` — so entities are neither duplicated
nor orphaned across reconnect/reboot (configs are retained).

| Entity | Component | Discovery topic | Unique id | State topic |
|---|---|---|---|---|
| Node online | `binary_sensor` | `homeassistant/binary_sensor/<hid>_<nid>/config` | `<hid>_<nid>_online` | `.../status` |
| Node health | `sensor` | `homeassistant/sensor/<hid>_<nid>_health/config` | `<hid>_<nid>_health` | `.../health` |
| Backup status | `sensor` | `homeassistant/sensor/<hid>_<nid>_backup/config` | `<hid>_<nid>_backup` | `.../backup/last` |
| Security status | `sensor` | `homeassistant/sensor/<hid>_<nid>_security/config` | `<hid>_<nid>_security` | `.../security` |
| Firmware version | `sensor` | `homeassistant/sensor/<hid>_<nid>_firmware/config` | `<hid>_<nid>_firmware` | `.../state` |
| Last HomeKey authentication | `sensor` | `homeassistant/sensor/<hid>_<nid>_last_auth/config` | `<hid>_<nid>_last_auth` | `.../last_auth` |

State sources: backup → `{{ value_json.status }}` (attributes = full JSON);
node health → `{{ value_json.mqtt }}`; firmware →
`{{ value_json.firmware_version }}`; last auth → `{{ value_json.result }}`
(attributes = full JSON); security → raw `OK`/`WARNING`.

## Availability (online / offline)

MQTT allows **one will (LWT) per connection**, and the ESP-IDF client's will is
configured on the legacy availability topic (`<CLIENT_ID>/status`, payload
`offline`, retained). The household node-online entity therefore reuses that
broker LWT via `availability_topic`:

```
state_topic = .../status
availability_topic = <CLIENT_ID>/status   (the single broker LWT)
payload_available = online
payload_not_available = offline
```

On connect the node publishes `online` to both topics (retained); on an
unexpected disconnect the broker publishes the will `offline`. A new Home
Assistant instance reading the retained topics therefore sees the correct
current state. This is additive — the legacy `<CLIENT_ID>/status` behaviour is
unchanged. (Full independence from the legacy topic is not possible while MQTT
permits a single will per connection.)

## Legacy (internal compatibility)

The `<CLIENT_ID>/...` topics (prefix = `mqttClientId`, e.g. `ESP_XXXXXXXX`) are
**legacy/internal**. They remain functional but must not be used by the HA V2
household integration:

- `<CLIENT_ID>/status` — legacy presence/LWT.
- `<CLIENT_ID>/homekit/state` — lock state; `<CLIENT_ID>/homekit/set_state`,
  `set_target_state`, `set_current_state`, `set_battery_lvl`,
  `set_custom_state` — **plain numeric commands with no application auth**.
  The legacy HA lock entity uses `set_target_state`; HA V2 must use the HMAC
  `.../command/*` topics instead.
- `<CLIENT_ID>/homekey/auth` — HomeKey/RFID tap event. It publishes identifiers
  (`issuerId`, `endpointId`, `readerId`) — identifiers only, **no key material,
  no APDU**. HA V2 should use `.../last_auth` for authentication state.
- `<CLIENT_ID>/alt_action` — alt action pulse.

- Telemetry topics are read-only status; a compromised broker subscription cannot
  change lock state.
- Command topics can unlock the door and therefore require the authenticated
  command format plus TLS (and, recommended, per-device broker credentials and
  ACLs limiting each node to its own namespace).
