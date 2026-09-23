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
| Web UI pages | Misc, MQTT, OTA, Logs, Actions… | **+ household, node, health, security, audit, backup, recovery, provision** |
| Flash encryption | Disabled (deliberate) | **Enabled** |
| Secure Boot | Disabled | **Enabled (V1, ECDSA-P256)** |
| NVS encryption | Disabled | **Enabled** (`nvs_keys` partition) |
| Partition table | `0x8000`, no `nvs_keys` | **`0xD000`, with `nvs_keys`** |
| OTA sources | ArduinoOTA / HomeSpan / Web UI | Same, **plus Secure Boot signing required** |
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
> **This fork enables flash encryption, Secure Boot V1 and NVS encryption.
> Upstream deliberately does not.** This is irreversible and destroys data on
> existing devices.

| Protection | Upstream `0.9.0` | This fork `0.10.0` |
| --- | --- | --- |
| Flash encryption | **No** — flash is plaintext; the reader keys, HAP pairing keys and Wi-Fi credentials can be read over serial | **Yes** — app, NVS and LittleFS are AES-encrypted with a per-device eFuse key |
| Secure Boot | No — arbitrary firmware can be flashed | **Yes, V1 (ECDSA-P256)** — only signed firmware boots |
| NVS encryption | No (`nvs_keys` partition absent) | **Yes** — via a new `nvs_keys` partition |
| Physical access outcome | Total compromise: secrets readable, firmware swappable | Secrets unreadable; firmware must be signed. DoS remains possible |

Upstream's reasoning was that enabling these would force every existing user to
re-flash and reconfigure. **This fork accepts that cost** in exchange for
at-rest protection — which means:

- **OTA from upstream/older builds will not boot.** The partition table moved
  (`0x8000` → `0xD000`), an `nvs_keys` partition was added, and app partitions were
  realigned to 64 KiB boundaries. A **serial flash is required**.
- **Existing device data is erased** when the flash is first encrypted: Wi-Fi
  credentials, HomeKit pairing and HomeKey reader enrolment.
- Every future image must be signed with the same key. Generate it once and keep it
  safe, or the device can never be updated again.

See [Security](security#flash-encryption-secure-boot-and-nvs-encryption) and
[Updates](updates).

## 5. What is unchanged from upstream

To be explicit, this fork does **not** touch:

- The HomeKey / NFC authentication protocol (`DigitalDoorKey`), except for new
  storage/serialization around it.
- `LockManager` lock logic — still the single source of truth for lock state.
- The HomeSpan / HomeKit accessory model.
- The existing Web UI pages (Misc, MQTT, OTA, Logs, Actions) — only new pages were
  added.
- The existing MQTT topic names and payloads — the household namespace is additive.
- Backup cryptography is new, so there is no upstream behaviour to preserve.

## 6. Migrating from upstream

1. **Back up** your household recovery secret and note your configuration.
2. Generate a Secure Boot signing key:
   `espsecure.py generate_signing_key --version 1 keys/secure_boot_signing_key.pem`
3. Flash over **serial** (not OTA) — see [Updates](updates).
4. Re-provision: Wi-Fi, HomeKit pairing and HomeKey enrolment are gone.
5. Re-add HomeKey credentials in the Apple Home app.

## Related pages

- [Household & Node Architecture](household)
- [MQTT Household API](mqtt_household_api)
- [MQTT API Contract Matrix](mqtt_api_contract_matrix)
- [Security](security)
- [Updates](updates)
