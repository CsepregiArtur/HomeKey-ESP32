---
title: "Fork vs Upstream"
weight: 2
---

# This fork vs. the upstream project

This documentation site describes **this fork**. It is built on
[rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) (MIT), which is
the original project and remains the reference for the core HomeKey / HomeKit / NFC
functionality. Everything below explains **only what this fork changes**.

- **This fork:** https://github.com/CsepregiArtur/HomeKey-ESP32
- **Upstream:** https://github.com/rednblkx/HomeKey-ESP32
- **Fork version in this document:** `0.10.0` (forked from upstream `0.9.0`)

> [!IMPORTANT]
> If you are running upstream firmware, **use the upstream documentation** — the
> household features, MQTT contract and security model described here do not exist
> there. For upstream bugs, please report them upstream.

## At a glance

| Area | Upstream `0.9.0` | This fork `0.10.0` |
| --- | --- | --- |
| Scope | Single device | **Household of multiple nodes** |
| Node identity | — | Ed25519 keypair per node, never cloned |
| Backup | — | Encrypted (XChaCha20-Poly1305) + signed (Ed25519) household backup |
| Provisioning | — | Single-use, expiring, replay-protected join codes |
| MQTT | Single-device legacy topics | **Additive** structured household namespace + HA discovery |
| MQTT commands | Plain numeric payloads | **HMAC-SHA256 authenticated** `command/lock` \| `command/unlock` |
| Web UI pages | Misc, MQTT, OTA, Logs, Actions… | **+ household, node, health, security, audit, backup, recovery, provision**; the OTA page is **removed** (no over-the-air update) |
| Flash encryption | Disabled (deliberate) | **Implemented, off by default** |
| Secure Boot | Disabled | **Implemented, off by default** (V1, ECDSA-P256 when enabled) |
| NVS encryption | Disabled | **Implemented, off by default** (`nvs_keys` partition when enabled) |
| Partition table | `0x8000`, no `nvs_keys` | Single-slot `no_ota.csv`: one 3840 KiB `factory` app slot, no `otadata`; moves to `0xD000` with `nvs_keys` when hardening is enabled |
| OTA sources | ArduinoOTA / HomeSpan / Web UI | **None** — the single-slot layout has nowhere to write an image; firmware is installed over serial |
| Audit log | — | Bounded 256-record NVS-backed log |
| Health reporting | — | Aggregated health snapshot |

## 1. Household / multi-node architecture

**New.** Upstream has no concept of a household; each device is independent.

- A **household** groups several nodes (gate, main house, garage, workshop) that
  share trust and recovery material.
- Each node keeps its **own Ed25519 identity** and a **non-cloneable** HomeKey
  reader identity.
- Nodes can be enrolled (`provision`), replaced (`recovery`) and reported on
  (`household`, `node`, `health`).

New firmware modules (none of these exist upstream):

| Module | Role |
| --- | --- |
| `HouseholdManager` | Household id/name/state, trust anchor, recovery secret |
| `NodeIdentityManager` | Per-device Ed25519 identity, generation counter, signing |
| `ProvisioningManager` | One-time enrollment codes (SHA-256 stored, single-use) |
| `BackupManager` | Encrypted + signed household backup export |
| `RestoreManager` | Decrypt, verify and restore onto a replacement node |
| `SecurityManager` | Read-only security posture (no numeric score) |
| `AuditManager` | Bounded 256-record security event log |
| `HealthManager` | Aggregated health snapshot |

See [Household & Node Architecture](household).

## 2. Encrypted, signed backups

**New.** Upstream has no backup/restore at all.

- Format: versioned header + XChaCha20-Poly1305 AEAD + Ed25519 signature.
- Key derivation: `BLAKE2b("HK-HOUSEHOLD-BACKUP-v1", recovery_secret || salt)`.
- No raw NVS dumps. The reader private key and endpoint persistent keys are
  **excluded by design** — see the classification table in
  [Household & Node Architecture](household#field-classification).
- Unknown/future format versions are **rejected** (fail closed).

## 3. MQTT: additive household namespace

**Additive and backward compatible.** Upstream's single-device topics are
unchanged; this fork adds a structured namespace on top:

```
homekey/household/<household_id>/nodes/<node_id>/
├── state · status · health · security
├── backup/{status,last}
├── last_auth
└── command/{lock,unlock}      # HMAC-SHA256 authenticated
```

| | Upstream | This fork |
| --- | --- | --- |
| Lock/unlock command auth | Plain numeric payload on `homekit/set_target_state` | **HMAC-SHA256** over `{ts}{nonce}{req_id}{action}`, ±300 s window, nonce replay set, fail-closed |
| `unlock=true` accepted? | n/a | **Never** |
| HA discovery entities | lock + tag | **+ node online/health/backup/security/firmware/last_auth** |
| HomeKey auth payload | — | `{type, result, timestamp}` (no credential ids, no APDU) |

Legacy topics remain functional but are classified **legacy/internal** — the
household integration must not depend on them.

See [MQTT Household API](mqtt_household_api) and
[MQTT API Contract Matrix](mqtt_api_contract_matrix).

## 4. Security model — the biggest difference

> [!CAUTION]
> **This fork *implements* flash encryption, Secure Boot V1 and NVS encryption.
> Upstream deliberately does not.** They are **disabled by default** in this fork
> so the board stays reversible; turning them on is a deferred, one-way rollout
> described in [Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT).
> Once enabled it is irreversible and destroys data on existing devices.

| Protection | Upstream `0.9.0` | This fork `0.10.0` |
| --- | --- | --- |
| Flash encryption | **No** — flash is plaintext; the reader keys, HAP pairing keys and Wi-Fi credentials can be read over serial | **Supported, currently off** — switchable on with a per-device eFuse key; see the rollout plan |
| Secure Boot | No — arbitrary firmware can be flashed | **Supported, currently off** — V1 (ECDSA-P256) when enabled; only signed firmware boots |
| NVS encryption | No (`nvs_keys` partition absent) | **Supported, currently off** — uses a new `nvs_keys` partition when enabled |
| Right now | Plaintext flash, plaintext NVS | **Identical to upstream for day-to-day use** — no eFuses burned, no data loss |

Upstream's reasoning was that enabling these would force every existing user to
re-flash and reconfigure. This fork **agrees that it is not a step to take
casually**, so the hardening is implemented but staged as a deliberate, verifiable
rollout rather than a default:

| Path | What it does | Reversible? |
| --- | --- | --- |
| **Path 1 — current** | No eFuses burned, no encryption. Behaves like upstream; plaintext flashing works normally. | ✅ Yes |
| **Path 2 — deferred** | Burns the eFuses, encrypts the flash, Secure Boot locks the device to your signing key. Encrypted + signed images only. | ❌ **Permanent** |

The full staged procedure, its prerequisites and its consequences are in
**[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)**. Start there
before touching any security config.

When Path 2 is executed, be aware that:

- **Firmware from an older build cannot be installed by any route but serial.** The partition
  table moves (`0x8000` → `0xD000`), an `nvs_keys` partition is added, and app partitions are
  realigned to 64 KiB boundaries. A **serial flash is required** (and there is no OTA path on
  the current single-slot layout in any case).
- **Existing device data is erased** when the flash is first encrypted: Wi-Fi
  credentials, HomeKit pairing and HomeKey reader enrolment.
- Every future image must be signed with the same key. Generate it once and keep it
  safe off-machine, or the device can never be updated again.

See [Security](security#flash-encryption-secure-boot-and-nvs-encryption) and
[Updates](updates).

## 5. What is unchanged from upstream

To be explicit, this fork does **not** touch:

- The HomeKey / NFC authentication protocol (`DigitalDoorKey`), except for new
  storage/serialization around it.
- `LockManager` lock logic — still the single source of truth for lock state.
- The HomeSpan / HomeKit accessory model.
- The existing Web UI pages (Misc, MQTT, Logs, Actions) — only new pages were
  added, and the OTA page was removed along with over-the-air updates.
- The existing MQTT topic names and payloads — the household namespace is additive.
- Backup cryptography is new, so there is no upstream behaviour to preserve.

## 6. Migrating from upstream

1. **Back up** your household recovery secret and note your configuration.
2. Flash over **serial** — see [Updates](updates).
3. Re-provision: Wi-Fi, HomeKit pairing and HomeKey enrolment are reset by the
   partition-layout change.
4. Re-add HomeKey credentials in the Apple Home app.

> Enabling the security features is a **separate, deferred step**. Do not generate a
> signing key or burn eFuses as part of a normal migration — follow
> **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)** when you are
> ready for that.

## Related pages

- [Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)
- [Household & Node Architecture](household)
- [MQTT Household API](mqtt_household_api)
- [MQTT API Contract Matrix](mqtt_api_contract_matrix)
- [Security](security)
- [Updates](updates)
