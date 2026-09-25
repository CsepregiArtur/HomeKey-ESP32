---
title: "MQTT API Contract Matrix"
weight: 8
---

# MQTT API Contract Matrix

> [!NOTE]
> **Fork-only document.** This contract describes the household MQTT API added by
> this fork. Upstream's MQTT surface is the smaller legacy topic set; nothing here
> exists upstream. See [Fork vs Upstream](fork-vs-upstream).

Formal interface contract between **HomeKey-ESP32 0.10.0** and
**HomeKey Household — Home Assistant V2**. This document is the result of a
read-only audit of the actual firmware implementation (`main/MqttManager.cpp`,
`main/include/MqttManager.hpp`, `main/include/config.hpp`,
`main/include/defaults.h`) versus `docs/content/mqtt_household_api.md`.

**Authoritative source: the running firmware implementation.** Where the docs and
the code disagree, the code wins and the discrepancy is recorded below.

> [!IMPORTANT]
> This audit is read-only. No firmware was changed during this audit; the
> discrepancies below must be resolved in a separate change before HA V2 is built.

---

## 1. Identity model (read this first)

The firmware uses **two distinct identity models**. HA V2 must understand both.

| Identifier | Value | Used for |
|---|---|---|
| `mqttClientId` | `ESP_{XXXXXXXX}` (BT MAC bytes 2–5, 8 hex) | MQTT client id, **legacy topic prefix**, legacy discovery topic path |
| `deviceID` | HomeKit accessory id (12 hex, `HAPClient::accessory.ID` minus dashes) | legacy lock entity `unique_id`, device `identifiers` |
| `macStr` | `HK-{XXXXXXXX}` (BT MAC bytes 2–5) | device `identifiers`, `name`, `serial_number` |
| household id / node id | `<household_id>`, `<node_id>` | household namespace + household entity `unique_id` (`<hid>_<nid>_<entity>`) |

`deviceID` (HomeKit accessory id) can change if the device is re-paired in Apple
Home; `mqttClientId`/`macStr` are MAC-derived and stable across reflashes. The
household `<hid>_<nid>` identity is stable across reboot but the `node_id`
changes when a node is replaced (`GATE-001` → `GATE-002`).

---

## 2. MQTT namespace (topic tree, actual)

Legacy prefix `P` = `mqttClientId` (`ESP_{XXXXXXXX}`).
Household base `B` = `homekey/household/<household_id>/nodes/<node_id>`.

```
<P>/                          (legacy, single-device)
├── status                    LWT / presence (broker-published "offline" on loss)
├── homekey/auth              HomeKey tap + RFID UID (JSON)
├── homekit/state             lock state (numeric string)
├── homekit/set_state         [SUB] set current+target
├── homekit/set_target_state  [SUB] set target
├── homekit/set_current_state [SUB] set current
├── homekit/set_battery_lvl   [SUB] set prox battery
├── homekit/custom_state      custom lock state (conditional)
├── homekit/set_custom_state  [SUB] custom lock state (conditional)
└── alt_action                alt action pulse ("1")

homekey/household/<household_id>/nodes/<node_id>/   (household)
├── state                     node state (JSON, retained)
├── status                    "online" (retained; NO firmware "offline" path)
├── health                    health snapshot (JSON, QoS0, non-retained)
├── security                  "OK" / "WARNING"
├── backup/
│   ├── status                "completed" / "failed"
│   └── last                  {status,timestamp} JSON (metadata only)
├── last_auth                 {type,result,timestamp} JSON (safe metadata)
├── lock/last                 {current,target,source,timestamp} JSON (what changed it)
└── command/
    ├── lock                  [SUB] HMAC-authenticated lock
    └── unlock                [SUB] HMAC-authenticated unlock
```

Reserved in docs but **not implemented in code** (no publisher/subscriber):
`B/events`, `B/backup/request`, `B/backup/data`, `B/restore/request`,
`B/restore/status`.

---

## 3. Complete MQTT API matrix

Direction: **ESP→MQTT** (publish), **MQTT→ESP** (subscribe), **HA Disc**, **Bi**.
`P` = legacy prefix, `B` = household base.

| # | Topic | Direction | Purpose | Publisher / Subscriber | Retained | QoS | Payload | Auth | Sensitive? | HA map | Stable? |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `P/status` | ESP→MQTT | availability | `MqttManager::onConnected()` ("online"); broker LWT "offline" | YES | 1 | `online`/`offline` | TLS / broker creds only | NO | legacy lock `availability_topic` | STABLE |
| 2 | `P/homekey/auth` | ESP→MQTT | HomeKey tap / RFID event | `MqttManager::publishHomeKeyTap()` / `publishUidTap()` | NO | 0 | JSON (below) | TLS / broker creds only | **LOW** (identifiers) | legacy tag sensors | STABLE |
| 3 | `P/homekit/state` | ESP→MQTT | lock state | `MqttManager::publishLockState()` | YES | 0 | numeric string `0..5` | none | NO | legacy lock `state_topic` | STABLE |
| 4 | `P/homekit/set_state` | MQTT→ESP | set current+target | `MqttManager::onData()` | n/a | 0 | numeric string `0..255` | **none (plain)** | NO | — | STABLE |
| 5 | `P/homekit/set_target_state` | MQTT→ESP | set target | `MqttManager::onData()` | n/a | 0 | numeric string | **none (plain)** | NO | legacy lock `command_topic` | STABLE |
| 6 | `P/homekit/set_current_state` | MQTT→ESP | set current | `MqttManager::onData()` | n/a | 0 | numeric string | **none (plain)** | NO | — | STABLE |
| 7 | `P/homekit/set_battery_lvl` | MQTT→ESP | set prox battery | `MqttManager::onData()` | n/a | 0 | numeric string | **none (plain)** | NO | — | STABLE |
| 8 | `P/homekit/custom_state` | ESP→MQTT | custom lock state | `MqttManager::publishLockState()` (conditional) | NO | 0 | numeric string | none | NO | — | STABLE |
| 9 | `P/homekit/set_custom_state` | MQTT→ESP | custom lock state cmd | `MqttManager::onData()` (conditional) | n/a | 0 | numeric string | **none (plain)** | NO | — | STABLE |
| 10 | `P/alt_action` | ESP→MQTT | alt action pulse | `MqttManager::begin()` HW_ALT_ACTION sub | NO | 0 | `1` | none | NO | — | STABLE |
| 11 | `B/state` | ESP→MQTT | node state | `MqttManager::publishNodeStatus()` | YES | 0 | JSON (below) | TLS / broker creds only | NO | firmware entity | STABLE |
| 12 | `B/status` | ESP→MQTT | node presence | `MqttManager::publishNodeStatus()` | YES | 1 | `online` | TLS / broker creds only | NO | node online (availability via shared LWT) | STABLE |
| 13 | `B/health` | ESP→MQTT | health snapshot | `MqttManager::publishNodeStatus()` | NO | 0 | JSON (below) | TLS / broker creds only | NO | node health | STABLE |
| 14 | `B/security` | ESP→MQTT | security state | `MqttManager::publishNodeStatus()` | YES | 0 | `OK`/`WARNING` | TLS / broker creds only | NO | security entity | STABLE |
| 15 | `B/backup/status` | ESP→MQTT | backup outcome | `MqttManager::publishBackupStatus()` (from `main.cpp` BACKUP event) | YES | 0 | `completed`/`failed` | TLS / broker creds only | NO | — | STABLE |
| 16 | `B/backup/last` | ESP→MQTT | backup metadata | `MqttManager::publishBackupStatus()` | YES | 0 | JSON (below) | TLS / broker creds only | NO | backup entity | STABLE |
| 17 | `B/last_auth` | ESP→MQTT | last HomeKey auth | `MqttManager::publishLastAuth()` | YES | 0 | JSON (below) | TLS / broker creds only | **LOW** (safe metadata) | last_auth entity | STABLE |
| 17b | `B/lock/last` | ESP→MQTT | what changed the lock | `MqttManager::publishLockChange()` | YES | 0 | JSON (below) | TLS / broker creds only | **LOW** (a source word; no identifiers) | lock entity / activity cause | NEW |
| 18 | `B/command/lock` | MQTT→ESP | authenticated lock | `MqttManager::handleSecureCommand()` | n/a | 1 | JSON + HMAC | **HMAC-SHA256** | NO | — | STABLE |
| 19 | `B/command/unlock` | MQTT→ESP | authenticated unlock | `MqttManager::handleSecureCommand()` | n/a | 1 | JSON + HMAC | **HMAC-SHA256** | NO | — | STABLE |
| 20 | `homeassistant/lock/<P>/lock/config` | HA Disc | lock discovery | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | NO | lock | STABLE |
| 21 | `homeassistant/tag/<P>/hk_issuer/config` | HA Disc | issuer sensor | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | LOW | tag | STABLE |
| 22 | `homeassistant/tag/<P>/hk_endpoint/config` | HA Disc | endpoint sensor | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | LOW | tag | STABLE |
| 23 | `homeassistant/tag/<P>/rfid/config` | HA Disc | RFID sensor (conditional) | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | NO | tag | STABLE |
| 24 | `homeassistant/binary_sensor/<hid>_<nid>/config` | HA Disc | node online | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | NO | node online (+ `availability_topic`=legacy LWT) | STABLE |
| 25 | `homeassistant/sensor/<hid>_<nid>_<entity>/config` (entity = health/backup/security/firmware/last_auth) | HA Disc | household sensor(s) | `MqttManager::publishHassDiscovery()` | YES | 1 | JSON | TLS / broker creds only | NO | sensors | STABLE |

---

## 4. Payload schemas (actual)

### `P/homekey/auth` (HomeKey tap, `publishHomeKeyTap`)
```json
{"issuerId":"<hex>","endpointId":"<hex>","readerId":"<hex>","homekey":true}
```
Uppercase hex, no separators. Identifiers only (issuer id = SHA-256 of HAP LTPK;
endpoint id = key identifier). No key material or APDU.

### `P/homekey/auth` (RFID, `publishUidTap`)
```json
{"uid":"<hex>","homekey":false,"atqa":"<hex>","sak":"<hex>","readerId":"<deviceID>"}
```

### `B/state`
```json
{"household_id":"…","node_id":"…","node_name":"…","node_role":"gate|main_house|small_house|garage|workshop|other",
 "node_state":"UNCONFIGURED|PROVISIONING|ACTIVE|REVOKED|RECOVERY_REQUIRED","generation":1,
 "firmware_version":"0.10.0…"}
```

### `B/health`
```json
{"network":"UNKNOWN","mqtt":"OK|ERROR","mqtt_error":0,"nfc":"OK|ERROR",
 "lock_current":0..255,"lock_target":0..255,"backup":"ok|failed|unknown",
 "certificate":"unknown","firmware_version":"…","uptime":1234,"free_heap":123456,
 "reset_reason":"<int>","security":{"all_ok":true|false,"warnings":"…"}}
```
`network` is always `"UNKNOWN"` (not wired) and `certificate` is always `"unknown"`
(not wired) in the current implementation.

### `B/security`
Plain string: `OK` or `WARNING`. `ERROR` is reserved and never emitted. No numeric score.

### `B/backup/status`
Plain string: `completed` or `failed` (published from the `BACKUP_COMPLETED` /
`BACKUP_FAILED` event subscriptions in `main.cpp`).

### `B/backup/last`
```json
{"status":"completed|failed","timestamp":1760000000}
```
`timestamp` = wall-clock unix seconds when available, else monotonic uptime seconds.
Encrypted backup contents are **never** published here (or anywhere else on MQTT).

### `B/last_auth`
```json
{"type":"HomeKey","result":"SUCCESS|FAILURE","timestamp":1760000000}
{"type":"HomeKey","result":"SUCCESS","timestamp":1760000000,"issuer":"Artur's iPhone"}
```
Safe metadata only. No credential identifiers, APDU, or key material.
`timestamp` format identical to `backup/last`.

`issuer` is present only when the user has named that issuer in the Web UI, and carries the
**name they typed** — never the issuer id. Naming an issuer is the opt-in that allows the
name to be published; a device with unnamed issuers publishes exactly the first form.

### `B/lock/last`
```json
{"current":0,"target":0,"source":"homekit","timestamp":1760000000}
```
What asked for the most recent lock change. Published immediately **before** the state it
explains, so a client that wants to name a cause has it on hand by the time the change
arrives.

`source` ∈ `homekit` | `homekey` | `mqtt` | `api` | `device` | `unknown` (the firmware's
`LockManager::sourceName()`, shared verbatim with the HTTP API so a client needs one
vocabulary for both).

`current`/`target` use the same numbering as `lock_current`/`lock_target` in `B/health` and
are what ties the entry to a state change — a client must not apply a cause to a change it
does not describe. No identifiers: a source is one of six fixed words.

---

## 5. Home Assistant entity matrix (exact discovery payloads)

Legacy entities use `unique_id` = `deviceID` and `device.identifiers` =
`[deviceID, macStr]`. Household entities use `unique_id` = `<hid>_<nid>_<entity>`.

| Entity | Type | Discovery topic | unique_id | state_topic | value_template / payload | command_topic | availability_topic | json_attributes |
|---|---|---|---|---|---|---|---|---|
| Lock | `lock` | `homeassistant/lock/<P>/lock/config` | `<deviceID>` | `P/homekit/state` | payload_lock=`1` unlock=`0`; state_locked=`1` unlocked=`0` locking=`5` unlocking=`4` jammed=`2` | `P/homekit/set_target_state` | `P/status` | — |
| HomeKey Issuer | `tag` | `homeassistant/tag/<P>/hk_issuer/config` | `<deviceID>` | `P/homekey/auth` | `{{ value_json.issuerId }}` | — | — | — |
| HomeKey Endpoint | `tag` | `homeassistant/tag/<P>/hk_endpoint/config` | `<deviceID>` | `P/homekey/auth` | `{{ value_json.endpointId }}` | — | — | — |
| NFC Tag | `tag` | `homeassistant/tag/<P>/rfid/config` | `<deviceID>` | `P/homekey/auth` | `{{ value_json.uid }}` | — | — | — |
| Node online | `binary_sensor` | `homeassistant/binary_sensor/<hid>_<nid>/config` | `<hid>_<nid>_online` | `B/status` | payload_on=`online`, off=`offline` | — | `P/status` (shared LWT) | — |
| Node health | `sensor` | `homeassistant/sensor/<hid>_<nid>_health/config` | `<hid>_<nid>_health` | `B/health` | `{{ value_json.mqtt }}` | — | — | — |
| Backup status | `sensor` | `homeassistant/sensor/<hid>_<nid>_backup/config` | `<hid>_<nid>_backup` | `B/backup/last` | `{{ value_json.status }}` | — | — | `B/backup/last` (full JSON) |
| Security status | `sensor` | `homeassistant/sensor/<hid>_<nid>_security/config` | `<hid>_<nid>_security` | `B/security` | (raw) | — | — | — |
| Firmware version | `sensor` | `homeassistant/sensor/<hid>_<nid>_firmware/config` | `<hid>_<nid>_firmware` | `B/state` | `{{ value_json.firmware_version }}` | — | — | — |
| Last HomeKey auth | `sensor` | `homeassistant/sensor/<hid>_<nid>_last_auth/config` | `<hid>_<nid>_last_auth` | `B/last_auth` | `{{ value_json.result }}` | — | — | `B/last_auth` (full JSON) |

Each household entity has a **unique discovery topic** (object id). The object id
equals the unique-id suffix (`<hid>_<nid>_<entity>`) except node online, which keeps
the bare `<hid>_<nid>` object id. Node online also sets `availability_topic` to the
shared broker LWT (see §12).

All discovery payloads share a `device` descriptor:
```json
{"identifiers":["<deviceID>","HK-XXXXXXXX"],
 "name":"<device_name>","manufacturer":"rednblkx","model":"HomeKey-ESP32",
 "sw_version":"<firmware>","configuration_url":"http://HK-XXXXXXXX.local",
 "serial_number":"HK-XXXXXXXX"}
```
Household entities reuse this same `device` descriptor (they are not given a
household- or node-specific device identity).

---

## 6. Command security matrix

### Legacy commands (plain, broker-credential/TLS only — no application auth)

| Topic | Payload | Auth | Replay | Fail-closed | Effect |
|---|---|---|---|---|---|
| `P/homekit/set_target_state` | numeric `0`/`1` | none | no | parses only; invalid → ignored | `LOCK_TARGET_STATE_CHANGED` |
| `P/homekit/set_state` | numeric | none | no | invalid → ignored | `LOCK_OVERRIDE_STATE` |
| `P/homekit/set_current_state` | numeric | none | no | invalid → ignored | `LOCK_UPDATE_STATE` |
| `P/homekit/set_battery_lvl` | numeric | none | no | invalid → ignored | `BTR_PROP_CHANGED` |
| `P/homekit/set_custom_state` | numeric | none | no | invalid → ignored | custom lock events |

### Household commands (HMAC-authenticated)

| Topic | Payload (required) | MAC | Freshness | Replay | Fail-closed | Response |
|---|---|---|---|---|---|---|
| `B/command/lock` | `{"ts":num,"nonce":str,"req_id":str,"mac":"<64 hex>"}` | `HMAC-SHA256(key, "{ts}{nonce}{req_id}lock")` | ±300 s (wall clock only) | bounded 32-entry nonce deque | yes (no unlock on bad MAC/replay) | none (no MQTT ack) |
| `B/command/unlock` | same | `HMAC-SHA256(key, "{ts}{nonce}{req_id}unlock")` | same | same | yes | none |

Key = `BLAKE2b("HK-HOUSEHOLD-CMD-v1", recovery_secret || salt)`.
**The `action` is derived from the topic, not the payload** — the payload `action`
field shown in some docs is ignored by the firmware.

A payload of `unlock=true` is **never** accepted as authorization: no code path
maps a boolean to an unlock; legacy topics parse a numeric byte and the household
topics require a valid HMAC over the topic-derived action.

---

## 7. Telemetry / state audit

| Topic | Trigger | Interval | Retained | Fields | Derived/persisted | NVS write? | MQTT failure affects lock? |
|---|---|---|---|---|---|---|---|
| `B/state` | on connect + every 30 s (`main.cpp` loop) | 30 s | YES | see §4 | derived from managers | NO | NO |
| `B/status` | on connect + every 30 s | 30 s | YES | `online` | constant | NO | NO |
| `B/health` | on connect + every 30 s | 30 s | NO | see §4 | derived (`heap_caps_get_free_size`, `esp_reset_reason`) | NO | NO |
| `B/security` | on connect + every 30 s | 30 s | YES | `OK`/`WARNING` | from `SecurityManager::compute()` | NO | NO |
| `B/backup/status` + `last` | `BACKUP_COMPLETED`/`FAILED` event | on event | YES | see §4 | derived | NO | NO |
| `B/last_auth` | `HOMEKEY_TAP` event (success/fail) | on event | YES | see §4 | derived | NO | NO |
| `P/homekit/state` | `LOCK_STATE_CHANGED` event | on event | YES | numeric | from `LockManager` | NO | NO |
| `P/homekey/auth` | `NFC_TAP_EVENT` | on event | NO | JSON | derived | NO | NO |

Local HomeKey operation is independent of MQTT: the path
`NFC → LockManager → HomeKit` has no MQTT dependency (verified by code inspection;
the MQTT publish path is an event subscriber only). Telemetry publishing does not
write NVS.

---

## 8. Last HomeKey authentication audit

- Topic: `B/last_auth`. Payload: `{type, result, timestamp}`.
- `type` is always `"HomeKey"`; `result` is `"SUCCESS"`/`"FAILURE"`;
  `timestamp` is unix seconds (or monotonic uptime fallback).
- Published from `MqttManager::begin()`'s `NFC_TAP_EVENT` subscriber on every
  HomeKey tap (success and failure), retained, QoS 0.
- **No credential identifiers, no APDU, no cryptographic material** are included.
  (The legacy `P/homekey/auth` topic *does* publish `issuerId`/`endpointId`
  identifiers — a pre-existing, unchanged legacy behavior.)

---

## 9. Backup API audit

- Backup creation is an **HTTP** endpoint (`POST /backup/create`), not MQTT.
- MQTT exposes only metadata: `B/backup/status` (`completed`/`failed`) and
  `B/backup/last` (`{status,timestamp}`).
- The encrypted backup blob is **not** published to any MQTT topic
  (`B/backup/data` is reserved and unimplemented).
- Restore is an **HTTP** endpoint (`POST /backup/restore`); `B/restore/*` topics
  are reserved and unimplemented.
- Verified: no encrypted backup contents are exposed as an HA entity.

---

## 10. Security status audit

- Topic: `B/security`. Values: `OK` / `WARNING` (`ERROR` reserved, never emitted).
- Source: `SecurityManager::compute()` → `HealthManager::snapshot().security_all_ok`.
- Underlying checks: `secure_boot`, `flash_encryption`, `ota_signature`,
  `mqtt_tls`, `https`, `web_auth`, `homespan_ota` (each OK/WARNING/DISABLED).
- No numeric security score is exposed (not on MQTT, not in the Web UI).

---

## 11. Firmware status audit

- Source: `esp_app_get_description()->version` (e.g. `0.10.0-dev+<hash>`).
- Exposed in `B/state.firmware_version` and `B/health.firmware_version`; mapped to
  the firmware sensor via `B/state`.
- No update state and no OTA orchestration over MQTT; HA cannot trigger OTA
  (OTA is HTTP/HomeSpan only).

---

## 12. Error / availability behavior

| Event | Behavior |
|---|---|
| Broker down / TLS failure | `MQTT_EVENT_ERROR` → status set; client auto-reconnects (ESP-IDF default) |
| Disconnect | `MQTT_EVENT_DISCONNECTED` → `m_isConnected=false`; LWT on legacy `P/status` only |
| Malformed command JSON | `handleSecureCommand` returns (handled, fail-closed); no unlock |
| Invalid HMAC | rejected, audit `MQTT_UNLOCK_REQUEST`/`bad_mac`; no unlock |
| Expired timestamp | rejected when wall clock available; no unlock |
| Replayed nonce | rejected via bounded deque (32); no unlock |
| Unknown topic | ignored (no action) |
| Reconnect | `onConnected()` re-subscribes, re-publishes discovery + node status |
| Reboot | retained topics (`B/state`,`B/status`,`B/security`, etc.) persist on broker |
| MQTT failure | **local lock/HomeKey unaffected** (no MQTT dependency) |

Household `B/status` is published `online` (retained) on connect; the household
node-online entity also sets `availability_topic = <CLIENT_ID>/status` (the single
broker LWT, payload `offline`). On an unexpected disconnect the broker publishes
the will, so Home Assistant marks the entity unavailable instead of leaving it
permanently online. MQTT allows one will per connection, so full independence from
the legacy availability topic is not possible; the legacy behaviour is unchanged.
Discrepancy D3 is **resolved**.

---

## 13. Documentation vs implementation discrepancies (post-remediation)

| # | Item | Status | Severity (was) | Resolution |
|---|---|---|---|---|
| D1 | Household sensor discovery topics | **FIXED** | CRITICAL | Each household entity now has its own discovery topic `<hid>_<nid>_<entity>`; unique ids unchanged for backup/security/firmware/last_auth |
| D2 | Command payload | **FIXED (docs)** | HIGH | Payload schema no longer contains `action`; docs state the action is topic-derived |
| D3 | Node online availability | **FIXED** | MEDIUM | Node-online entity sets `availability_topic` to the shared broker LWT |
| D4 | `B/events` + reserved topics | **FIXED (docs)** | LOW | Marked RESERVED / NOT IMPLEMENTED; removed from the telemetry topic list |
| D5 | `B/health.network` / `certificate` | **DOCUMENTED** | LOW | Documented as unwired (`UNKNOWN`/`unknown`) — no fake implementation added |
| D6 | Lock command path | **RESOLVED (contract)** | MEDIUM | HMAC `B/command/*` is the authoritative HA V2 path; legacy numeric topics classified legacy/internal |
| D7 | Legacy `P/homekey/auth` | **DOCUMENTED** | LOW | Classified LEGACY; HA V2 uses `B/last_auth` |

---

## 14. Authoritative source

The **implementation is authoritative** for every item in this matrix. The
firmware was remediated for D1 (discovery topics) and D3 (availability);
`docs/content/mqtt_household_api.md` was corrected for D2/D4/D5/D7 and the
authoritative HA V2 command path (D6) is the HMAC `B/command/*` protocol.

---

## 15. HA V2 Integration readiness

| Capability | ESP32 support | MQTT topic | Payload stable? | Discovery? | HA V2 required? | Gap |
|---|---|---|---|---|---|---|
| household identity | YES | `B/state.household_id` | YES | — | YES | — |
| node identity | YES | `B/state.node_id` | YES | — | YES | changes on replacement |
| node online | YES | `B/status` (+ shared LWT) | YES | YES | YES | — (D3 fixed) |
| health | YES | `B/health` | YES | YES | YES | `network`/`certificate` documented stubs (D5) |
| lock state | YES | `P/homekit/state` | YES | YES | YES | legacy |
| lock command | YES | `B/command/{lock,unlock}` (HMAC) | YES | direct publish (no discovery entity) | YES | HA V2 publishes HMAC directly (D6) |
| HomeKey authentication | YES | `P/homekey/auth` (identifiers) / `B/last_auth` (safe) | YES | YES | YES | use `B/last_auth` |
| last authentication | YES | `B/last_auth` | YES | YES | YES | — |
| backup status | YES | `B/backup/status` + `B/backup/last` | YES | YES | YES | — |
| security status | YES | `B/security` | YES | YES | YES | — |
| firmware version | YES | `B/state.firmware_version` | YES | YES | YES | — |
| provisioning | HTTP only | — | — | — | optional | MISSING over MQTT |
| restore | HTTP only | — | — | — | optional | MISSING over MQTT |
| replacement node | YES | documented limitation | — | — | YES | re-provision Apple Home |
| audit | HTTP only | — | — | — | optional | MISSING over MQTT |

---

## 16. Protocol stability classification

| Topic / group | Class |
|---|---|
| `B/state`, `B/status`, `B/health`, `B/security`, `B/backup/status`, `B/backup/last`, `B/last_auth`, `B/command/lock`, `B/command/unlock` | **STABLE** |
| Household discovery (`homeassistant/{binary_sensor,sensor}/<hid>_<nid>[_<entity>]/config`) | **STABLE** (unique topics; D1 fixed) |
| `B/events`, `B/backup/{request,data}`, `B/restore/{request,status}` | **RESERVED / NOT IMPLEMENTED** (do not consume) |
| `P/*` legacy topics | **LEGACY / INTERNAL** (functional; HA V2 must not depend on them, except the shared availability topic noted in §12) |
| provisioning / restore / audit | **HTTP ONLY** (not exposed over MQTT) |
| (no topic) | **UNSAFE** — none: no private keys, APDU, backup contents, provisioning tokens or recovery secrets are published over MQTT |

---

## 17. Security findings

1. **Legacy lock command is unauthenticated (by design, unchanged).** The HA lock
   entity still uses the plain `P/homekit/set_target_state` path; this is classified
   legacy/internal. The household integration uses the HMAC `B/command/*` topics.
2. **Node presence availability** now uses the shared broker LWT (`availability_topic`);
   a single MQTT will per connection is a protocol limit, documented in §12.
3. **No private keys, APDU data, backup contents, provisioning tokens, or recovery
   secrets are exposed over MQTT.** Verified.
4. `unlock=true` is never sufficient to unlock (verified in `handleSecureCommand`
   and `onData`).
5. Legacy `P/homekey/auth` publishes HomeKey `issuerId`/`endpointId` identifiers
   (identifiers only, no key material). Classified LEGACY; HA V2 uses `B/last_auth`.

---

## 18. Recommended next step

**MQTT CONTRACT STABLE — PROCEED TO HA V2**

All audit blockers are resolved: D1 (unique household discovery topics, firmware),
D2 (command payload docs), D3 (household availability, firmware), D4/D5/D7
(documented). The HA V2 integration must:

- consume only the household namespace (`homekey/household/<hid>/nodes/<nid>/...`)
  and the documented discovery entities;
- issue lock/unlock via the HMAC `B/command/lock` / `B/command/unlock` topics
  (never the legacy numeric topics);
- treat `B/last_auth` as the HomeKey authentication state source.

Do not start HA V2 in this task.

Non-blocking (document or implement later): D3 (availability), D5 (stub fields),
D4/D7 (documentation notes).

---

## 19. Verification

`tests/test_mqtt_command_auth.py` — **7/7 passed** (HMAC command construction,
wrong key, modified payload, missing req_id, provisioning hash, replay). The
command MAC mirror matches `MqttManager::makeCommandMac` and
`handleSecureCommand`.
