---
title: "Household & Node Architecture"
weight: 5
---

# Household & Node Architecture

> [!NOTE]
> **Fork-only feature.** Upstream [rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32)
> has **no** household, node identity, backup or recovery concept — there a device is
> standalone. This entire page describes behaviour that exists **only in this fork**.
> See [Fork vs Upstream](fork-vs-upstream).

HomeKey-ESP32 can run as a single device or as part of a **Household**: a group of
nodes (gate, main house, small house, garage, workshop) that share trust and recovery
material while each node keeps its own device-specific identity.

> [!IMPORTANT]
> Local HomeKey authentication and physical lock control work entirely on the node.
> **Home Assistant, MQTT and the internet are not required** for a tap to unlock the
> local lock. If Home Assistant/MQTT/internet or another node is offline, the node
> keeps authenticating HomeKey and operating its own lock.

## Model

- **Household** — `household_id`, `household_name`, state, trust info, recovery
  metadata, provisioning info, configuration version.
- **Node** — `node_id`, `node_name`, `node_role`, a device-specific Ed25519 keypair
  and certificate fingerprint, plus the node's local HomeKey reader identity and
  local lock configuration.

Example: household `HOUSE-7F42` with nodes `GATE-001`, `HOUSE-001`, `SMALLHOUSE-001`.

## Security invariants

- **Node identity is never cloned.** A replacement device restoring a node receives a
  **new** node id (`GATE-001` → `GATE-002`) and a fresh keypair/certificate.
- **The HomeKey reader identity is never cloned either.** Replacing a node issues a
  new reader key, which invalidates all enrolled HomeKey devices — those must be
  re-provisioned in the Apple Home app. This cannot be safely automated.
- Household membership and trust data are restored separately from node identity.

## Modules

`HouseholdManager`, `NodeIdentityManager`, `ProvisioningManager`, `BackupManager`,
`RestoreManager`, `SecurityManager`, `AuditManager`, `HealthManager`. All communicate
through the existing event bus (`app_events.hpp`).

## Enrollment (provisioning)

1. Factory-new device boots into temporary setup mode.
2. User opens the Web UI and chooses **Join Household**.
3. A single-use, expiring provisioning code is issued (never logged, stored only as a
   SHA-256 hash, replay-protected).
4. The node receives the household configuration and mints its own node identity.
5. The node becomes `ACTIVE`.

## Backup format

Backups are versioned, encrypted and signed before they leave the device:

```
HomeKeyHouseholdBackup {
  header (format_version, household_id, node_id, node_role, generation,
          timestamp, firmware_version, node_public_key, recovery_salt)
  nonce || ciphertext || tag        # XChaCha20-Poly1305 (libsodium)
  signature                         # Ed25519 over header || ciphertext
}
Key = BLAKE2b("HK-HOUSEHOLD-BACKUP-v1", recovery_secret || salt)
```

Classification: **A** household data (restored), **B** node configuration (restored,
inside the AEAD payload), **C** regenerated node identity (excluded), **D** sensitive
crypto material such as the reader private key and endpoint persistent keys (excluded).

### Field classification

Every stored field is explicitly classified and traced through its full lifecycle
(generated → provisioned → stored → read → used → regenerated/deleted). Classes:

- **A** = safe configuration (public or low-sensitivity, portable)
- **B** = sensitive configuration (must be encrypted in transit/storage)
- **C** = secret (never leaves the device / regenerated)
- **D** = device-specific cryptographic identity (never cloned to another ESP32)

| Field | Storage | Purpose | Scope | Can backup? | Can restore to replacement node? | Must regenerate? | Must re-provision in Apple Home? | Reason |
|---|---|---|---|---|---|---|---|---|
| Household `household_id`, `household_name`, `state`, `config_version` — **A** | NVS `SAVED_DATA` / `HH_META` | Identify the household | household-wide, public | Yes (header + payload) | Yes | No | No | Public identifiers; portable. |
| Household `trust_public_key` — **A** | NVS `HH_META` | Cross-node trust anchor | household-wide, public | Yes (payload) | Yes | No | No | Public key material. |
| Household `recovery_metadata` (16 B KDF fingerprint) — **A** | NVS `HH_META` | Verify the correct recovery secret | household-wide, public | Yes (payload) | Yes | No | No | Public fingerprint; never the key. |
| Household recovery `secret` (32 B) — **C** | NVS `HH_REC_SECRET` | Encrypt backups / authenticate MQTT commands | household-wide, secret | No (it *is* the backup key source) | Yes — user re-supplies the offline copy | No (regenerating loses access to existing backups) | No | One-time export; decrypts all household backups. |
| Household recovery `salt` (16 B) — **A** | NVS `HH_REC_SALT` | KDF salt | household-wide, public | Yes (backup header) | Yes | No | No | Salt is not secret; carried in the header. |
| Node config (device name, MQTT broker/port/client/username/password, `use_ssl`/`allow_insecure`, web auth enabled/username/password, AP password) — **B** | NVS `SAVED_DATA` / `MISCDATA` + `MQTTDATA` | Device operation & connectivity | per-node, sensitive | Yes (encrypted AEAD payload) | Yes | No | No | Restorable config; must stay encrypted. |
| Node `node_id`, `node_role`, `generation` — **C** | NVS `NODE_META` | Household node identity label | per-node | Yes (plaintext header metadata) | No — replacement mints a new `node_id` (generation+1) | Yes | No | Ids are never reused or cloned. |
| Node `node_name` — **C** | NVS `NODE_META` | Human-readable node name | per-node | No (re-entered at restore) | Re-entered at restore | Yes (user re-enters) | No | Cosmetic; not persisted in backup. |
| Node Ed25519 `public_key` / `cert_fingerprint` — **A** | NVS `NODE_META` | Verify node signatures | per-node, public | Yes (`public_key` in header) | Yes (new key replaces it) | Yes (new keypair) | No | Public; signature verification needs it. |
| Node Ed25519 `private_key` — **C** | NVS `NODE_PRIVKEY` | Sign backups / prove node identity | per-node, secret | **No** | **No** | Yes (new keypair) | No | Never serialized, logged or backed up. |
| Provisioning code + its SHA-256 `PROV_HASH` — **C** | NVS `PROV_HASH`/`PROV_EXPIRY`/`PROV_USED` | Single-use enrollment token | per-node, transient | **No** | **No** | Yes (per issuance) | No | Code is secret; only the hash is stored; never logged. |
| Reader `private_key` — **D** | NVS `READERDATA` (msgpack) | Sign HomeKey auth transactions | reader-specific, secret | **No** | **No** | Yes — re-issued by Apple Home | **Yes** | Cloning it onto another ESP32 breaks the HomeKey security model. |
| Reader `public_key` / `public_key_x` — **D** | NVS `READERDATA` | Public counterpart of the reader key | reader-specific | **No** | **No** | Yes (derived from new key) | **Yes** | Derived from the private key; changes with it. |
| Reader `group_identifier` (GID, SHA-256(reader key)[:8]) — **D** | NVS `READERDATA` | Reader group identifier advertised over NFC | reader-specific, public but key-derived | **No** | **No** | Yes (derived from new key) | **Yes** | Derived from the reader key; broadcast, not secret. |
| Reader `sub_identifier` — **D** | NVS `READERDATA` | Reader unique identifier (assigned at provisioning) | reader-specific | **No** | **No** | Yes | **Yes** | Reader identity; replaced with the new reader key. |
| Issuer `id` (SHA-256 of HAP LTPK) + `public_key` (LTPK) — **A** | NVS `READERDATA` | Trusted HomeKit admin controller | controller-specific, public | Yes (payload: id + public key only) | Yes (also auto re-derived from HomeKit pairing) | No | No (re-pairing repopulates issuers) | Controller public keys; not reader-specific. |
| Issuer `public_key_x` — **A** | NVS `READERDATA` | Wire-fidelity field; unused by auth | controller-specific | Yes | Yes | No | No | Unused placeholder. |
| Endpoint `id`, `public_key`, `public_key_x` — **D** | NVS `READERDATA` | Enrolled HomeKey device identifiers/keys | reader+device-specific | **No** | **No** | Yes (re-enrollment) | **Yes** | Tied to both the reader key and the device; re-established by re-adding the device. |
| Endpoint `persistent_key` (32 B) — **D** | NVS `READERDATA` | Shared reader↔device secret | reader+device-specific, secret | **No** | **No** | Yes (re-enrollment) | **Yes** | Secret shared with the enrolled device; cannot be safely transferred. |
| Endpoint `counter`, `used_at` — **D** | NVS `READERDATA` | Anti-replay / last-use state | reader+device-specific | **No** | **No** | Yes (re-enrollment) | **Yes** | Replay-protection state tied to the reader key. |
| Endpoint `aliro` state — **D** | RAM only (not serialized) | Aliro step-up state | reader+device-specific | **No** | **No** | Yes | **Yes** | Never persisted; rebuilt per session. |
| HomeKit pairing (HAP `LTPK`/`LTSK`, SRP data in `hapNVS`/`charNVS`) — **D** | HomeSpan NVS (separate namespaces) | Pair the Apple Home controller | device-specific | **No** | **No** | Yes (re-pairing) | **Yes** | Owned by HomeSpan; never part of the household backup. |

### Restore model — three distinct operations

A backup is an **encrypted configuration container**, not a HomeKey credential
clone. Restoring a node is deliberately split into three separate operations:

1. **Configuration restore** — the A + B fields (household metadata, node config)
   are restored from the encrypted, signed backup. This brings back connectivity,
   names and credentials, not cryptographic identity.
2. **Household membership restore** — the user supplies the offline **recovery
   secret**; a replacement node re-joins the household under a **new** node id
   (`GATE-001` → `GATE-002`) with a freshly minted Ed25519 keypair.
3. **HomeKey credential restoration** — **not possible by design.** The reader
   private key and every endpoint persistent key are device-specific and excluded
   from backups. A replacement physical reader receives a **new** reader identity
   (re-issued by Apple Home), which invalidates all previously enrolled HomeKey
   devices. Those devices must be re-provisioned in the Apple Home app.

> [!WARNING]
> Replacing the NFC/HomeKey reader means existing HomeKey credentials stop
> working. Re-add them in the Apple Home app after the replacement. This is a
> protocol/security constraint, not a storage limitation — encrypted backup
> storage does **not** imply that HomeKey credentials can be restored.

## Recovery secret

The household recovery secret is generated once and exported **once** by the user for
offline storage. It is never displayed again, and it decrypts household backups so a
replacement node can be enrolled without Home Assistant.

## MQTT namespace

When a node is enrolled, structured telemetry is published to:

```
homekey/household/<household_id>/nodes/<node_id>/
    state, status, health, events,
    backup/status, backup/request, backup/data,
    restore/request, restore/status,
    command/lock, command/unlock
```

Lock/unlock commands are authenticated with an HMAC over `{ts, nonce, req_id, action}`
using a key derived from the household recovery secret (BLAKE2b with a separate label),
with a bounded nonce replay window. Plain `unlock=true` is never accepted as
authorization. Backup payloads sent over MQTT are always encrypted.

## Web UI

`/household`, `/node`, `/backup`, `/recovery`, `/security`, `/audit`, `/health`,
`/provision`. Secret values are never exposed; recovery material is shown only on an
explicit one-time export.

## Threat model & limitations

See [Security](security) for the full threat model. Additional notes for households:

- Physical access to a node is still a total compromise of that node (flash is not
  encrypted by design; see [Security](security#physical-access-is-a-total-compromise)).
- Node authentication across MQTT uses a shared household command key derived from the
  recovery secret; a full household CA/leaf-certificate scheme is future work (the
  current `cert_fingerprint` is a SHA-256 over `node_id || public_key`).
- HomeKey device re-provisioning after node replacement is a manual step.
