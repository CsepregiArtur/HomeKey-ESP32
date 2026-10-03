---
title: "Fork vs Upstream"
weight: 3
---

# This fork vs. the upstream project

This documentation site describes **this fork**. It is built on
[rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32) (MIT), which is
the original project and remains the reference for the core HomeKey / HomeKit / NFC
functionality. Everything below explains **only what this fork changes**.

- **This fork:** https://github.com/CsepregiArtur/HomeKey-ESP32
- **This fork's docs:** <https://csepregiartur.github.io/HomeKey-ESP32/>
- **Upstream:** https://github.com/rednblkx/HomeKey-ESP32
- **Upstream docs:** <https://rednblkx.github.io/HomeKey-ESP32/>
- **Fork version in this document:** `v0.12.0`

> [!IMPORTANT]
> If you are running upstream firmware, **use the upstream documentation** — the
> household features, MQTT contract, security model and hardware options described
> here do not exist there. For upstream bugs, please report them upstream.

## At a glance

| Area | Upstream | This fork `v0.12.0` |
| --- | --- | --- |
| Scope | Single device | **Household of multiple nodes** |
| Node identity | — | Ed25519 keypair per node, never cloned |
| Backup | — | Encrypted (XChaCha20-Poly1305) + signed (Ed25519) household backup |
| Provisioning | — | Single-use, expiring, replay-protected join codes |
| MQTT | Single-device legacy topics | **Additive** structured household namespace + HA discovery |
| MQTT commands | Plain numeric payloads | **HMAC-SHA256 authenticated** `command/lock` \| `command/unlock` |
| **Home Assistant** | MQTT discovery only | **+ a custom component** ([`homekey_household`](https://github.com/CsepregiArtur/homekey-household)) with a **broker-less, certificate-pinned HTTPS transport** alongside MQTT |
| **Firmware update** | OTA from the Web UI + GitHub updater | **Dual-slot OTA over the LAN** (rollback-enabled) **or serial** via `scripts/ota_update.py`; no GitHub updater |
| **Connectivity** | Wi-Fi **and Ethernet** (W5500, DM9051, KSZ8851, LAN8720, TLK110, …) | **Wi-Fi only** — the Ethernet driver was removed |
| **NFC readers** | PN532, PN7160/PN7161, ST25R3916 | **PN532 only** (SPI) — the others were removed to free flash for the second OTA slot |
| **Compile targets** | ESP32, ESP32-S3, ESP32-C3, ESP32-C6 | **ESP32 and ESP32-C3**, auto-detected when flashing |
| Web UI pages | Misc, MQTT, OTA, Logs, Actions… | **+ household, node, health, security, audit, backup, recovery, provision, guest tags, update**; the Ethernet settings are removed |
| Flash encryption | Disabled (deliberate) | **Implemented, off by default** |
| Secure Boot | Disabled | **Implemented, off by default** (V1, ECDSA-P256 when enabled) |
| NVS encryption | Disabled | **Implemented, off by default** (`nvs_keys` partition when enabled) |
| Partition table | `0x8000`, no `nvs_keys` | Dual-slot `with_ota.csv`: `app0`/`app1` 1856 KiB each + `otadata`; `no_ota.csv` remains as a single-slot fallback. Moves to `0xD000` with `nvs_keys` when hardening is enabled |
| Audit log | — | Bounded 256-record NVS-backed log |
| Health reporting | — | Aggregated health snapshot |

## 0. Removed features — read this first if you are migrating

> [!WARNING]
> **Three removals make an in-place upgrade from upstream impossible without a
> serial flash and full re-provisioning.** Wi-Fi credentials, HomeKit pairing and
> HomeKey enrolment stored on the device are lost.

| Removed | What it costs you | Why |
| --- | --- | --- |
| **Ethernet** (all SPI modules and RMII PHYs) | Wired networking is gone — the device is Wi-Fi only. Upstream's whole Ethernet configuration section and its `ETH_APP_EVENT` are gone too. | ~100 KB of flash, the largest single removable component, needed for the second OTA slot |
| **PN7160 / PN7161 and ST25R3916 readers** | Only the **PN532 over SPI** works. The `PN7161`/`ST25R3916` reader types, their IRQ/VEN pins and their presets are gone. | Flash, for the same reason |
| **The GitHub OTA updater** | The device never fetches firmware on its own. You push an image over the LAN (Web UI Update page) or over the cable. | Deliberate: it removes a route by which the device could be induced to install firmware without a local, authenticated decision |

If any of these are essential to you, **stay on upstream firmware** and use the
[upstream documentation](https://rednblkx.github.io/HomeKey-ESP32/).

## 1. Firmware updates: dual-slot OTA over the LAN

**Different from upstream, and the reason several features above were removed.**

The device now uses a **dual-slot** layout (`with_ota.csv`): two application
partitions (`ota_0`, `ota_1`) plus an `otadata` selector. An update is written into
the slot the device is *not* running from, so the running image is never overwritten
while it executes, and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` means an image that
fails to confirm itself is abandoned in favour of the previous slot.

| | Upstream | This fork |
| --- | --- | --- |
| Layout | Single slot (no OTA) | **Dual slot** + `otadata` |
| Update route | Web UI OTA page + GitHub updater | **LAN OTA** (`POST /api/ota/firmware`, HTTPS + auth) or **serial** |
| Rollback | n/a | **Yes**, via `markSketchOK()` in `setup()` |
| From the device itself | Could pull from GitHub | **Never** — only an authenticated caller pushes |
| Batch updates | One device at a time | `scripts/ota_update.py` can update **several devices in one run** |
| Chip awareness | — | **Auto-detects ESP32 vs ESP32-C3** and picks the right bootloader offset |

`scripts/ota_update.py` drives both paths: it looks for an attached device first and
offers a cable flash, otherwise it discovers devices over mDNS and asks which to
update. The password lives in the **macOS Keychain**, the device's certificate is
**pinned by fingerprint**, and `--prepare-for-ui` writes a correctly-named image for
the Web UI uploader.

See [Updates](updates).

## 2. Household / multi-node architecture

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

## 3. Encrypted, signed backups

**New.** Upstream has no backup/restore at all.

- Format: versioned header + XChaCha20-Poly1305 AEAD + Ed25519 signature.
- Key derivation: `BLAKE2b("HK-HOUSEHOLD-BACKUP-v1", recovery_secret || salt)`.
- No raw NVS dumps. The reader private key and endpoint persistent keys are
  **excluded by design** — see the classification table in
  [Household & Node Architecture](household#field-classification).
- Unknown/future format versions are **rejected** (fail closed).

## 4. MQTT: additive household namespace

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

## 5. Hardware: fewer options, two tested targets

| | Upstream | This fork |
| --- | --- | --- |
| Connectivity | Wi-Fi + Ethernet | **Wi-Fi only** |
| NFC reader | PN532, PN7160/PN7161, ST25R3916 | **PN532 only** |
| Targets built | ESP32, S3, C3, C6 | **ESP32, ESP32-C3** |
| PN532 SPI pins | 18/19/23/5 | 18/19/23/5 on ESP32, **4/5/6/7 on ESP32-C3** |
| Bootloader offset | `0x1000` | `0x1000` on ESP32, **`0x0` on ESP32-C3** |

The pin and offset differences are **not** configuration — they are per-chip facts,
and getting them wrong produces a device that never boots rather than an error.
`scripts/ota_update.py` handles the offset automatically, and the reader pins come
from the Arduino core's per-chip variant. See
[Setup → Compile Targets](setup#7-compile-targets).

## 6. Security model — the other big difference

> [!CAUTION]
> **This fork *implements* flash encryption, Secure Boot V1 and NVS encryption.
> Upstream deliberately does not.** They are **disabled by default** in this fork
> so the board stays reversible; turning them on is a deferred, one-way rollout
> described in [Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT).
> Once enabled it is irreversible and destroys data on existing devices.

| Protection | Upstream | This fork |
| --- | --- | --- |
| Flash encryption | **No** — flash is plaintext; reader keys, HAP pairing keys and Wi-Fi credentials can be read over serial | **Supported, currently off** — switchable on with a per-device eFuse key |
| Secure Boot | No — arbitrary firmware can be flashed | **Supported, currently off** — V1 (ECDSA-P256) when enabled; only signed firmware boots |
| NVS encryption | No (`nvs_keys` partition absent) | **Supported, currently off** — uses a new `nvs_keys` partition when enabled |
| Right now | Plaintext flash, plaintext NVS | **Identical for day-to-day use** — no eFuses burned, no data loss |

Upstream's reasoning was that enabling these would force every existing user to
re-flash and reconfigure. This fork **agrees that it is not a step to take
casually**, so the hardening is implemented but staged as a deliberate, verifiable
rollout rather than a default:

| Path | What it does | Reversible? |
| --- | --- | --- |
| **Path 1 — current** | No eFuses burned, no encryption. Behaves like upstream; plaintext flashing works normally. | ✅ Yes |
| **Path 2 — deferred** | Burns the eFuses, encrypts the flash, Secure Boot locks the device to your signing key. Encrypted + signed images only. | ❌ **Permanent** |

When Path 2 is executed, be aware that:

- **Firmware from an older build cannot be installed by any route but serial.** The
  partition table moves (`0x8000` → `0xD000`), an `nvs_keys` partition is added, and
  app partitions are realigned to 64 KiB boundaries.
- **Existing device data is erased** when the flash is first encrypted: Wi-Fi
  credentials, HomeKit pairing and HomeKey reader enrolment.
- Every future image must be signed with the same key. Generate it once and keep it
  safe off-machine, or the device can never be updated again.

See [Security](security#flash-encryption-secure-boot-and-nvs-encryption) and
[Updates](updates).

## 7. What is unchanged from upstream

To be explicit, this fork does **not** touch:

- The HomeKey / NFC authentication protocol (`DigitalDoorKey`), except for new
  storage/serialization around it.
- `LockManager` lock logic — still the single source of truth for lock state.
- The HomeSpan / HomeKit accessory model.
- The existing Web UI pages (Misc, MQTT, Logs, Actions) — only new pages were added.
- The existing MQTT topic names and payloads — the household namespace is additive.

## 8. Migrating from upstream

1. **Back up** your household recovery secret and note your configuration.
2. Flash over **serial** — see [Updates](updates). A partition table cannot be
   delivered over the air.
3. Re-provision: Wi-Fi, HomeKit pairing and HomeKey enrolment are reset by the
   partition-layout change.
4. Re-add HomeKey credentials in the Apple Home app.
5. Check your hardware: if you were using Ethernet or a PN7160/PN7161/ST25R3916
   reader, this firmware will not drive it.

> Enabling the security features is a **separate, deferred step**. Do not generate a
> signing key or burn eFuses as part of a normal migration — follow
> **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)** when you are
> ready for that.

## Related pages

- [Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)
- [Household & Node Architecture](household)
- [MQTT Household API](mqtt_household_api)
- [MQTT API Contract Matrix](mqtt_api_contract_matrix)
- [Updates](updates)
- [Setup](setup)
- [Security](security)
