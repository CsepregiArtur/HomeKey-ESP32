---
title: "Security Rollout Plan: Path 1 → Path 2"
weight: 18
---

# Security Rollout Plan: Path 1 → Path 2

> **Status:** Path 1 active. Path 2 is **deferred** until the firmware is fully
> validated on real hardware.
> **Decision date:** 2026-09-25
> **Owner:** @CsepregiArtur
> **Revised:** 2026-10-03 — the partition layout it refers to is now the dual-slot
> `with_ota.csv`; see [Updating firmware](updates).

This document records the deliberate two-step approach to hardware security on
this fork, why it is split in two, and exactly what changes when Path 2 is
executed.

---

## Why two paths

Flash encryption and Secure Boot on the original ESP32 are **one-way doors**.
They are driven by eFuses, and a burned eFuse cannot be cleared, reset, or
worked around. That makes them fundamentally different from every other setting
in the project: a normal config mistake costs a rebuild, a security config
mistake costs the chip.

Because of that, the rollout is split:

| | Path 1 — **Reversible** (current) | Path 2 — **Permanent** (deferred) |
|---|---|---|
| Flash encryption | Off | On (AES-256, key in eFuse BLK1) |
| Secure Boot | Off | On (V1 / ECDSA-P256, digest in eFuse BLK2) |
| NVS encryption | Off | On (key in `nvs_keys` partition) |
| Partition table offset | `0x8000` (default) | `0xD000` (signed bootloader needs the room) |
| `nvs_keys` partition | Absent | Present, flagged `encrypted` |
| Plaintext flashing | Works | Dev mode only, and only until the flash limit runs out |
| Can be undone? | **Yes — fully** | **No, never** |
| Purpose | Development & hardware validation | Production / deployment |

### The rule

> **Do not start Path 2 until Path 1 is proven working on real hardware.**

That means: the board boots reliably, a **serial** flash cycle works end to end,
provisioning works, HomeKit pairs, the MQTT contract behaves, and the
backup/restore cycle passes — all on an unencrypted build. Only then does it make
sense to make the part that is hard to change permanent.

> [!IMPORTANT]
> **This layout has over-the-air firmware update**, which changes what Path 2 costs
> — for the better. The device uses the **dual-slot** `with_ota.csv`
> (`ota_0` + `ota_1` + `otadata`) with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, so
> an image that fails to confirm itself is rolled back automatically. Encrypted and
> signed images can therefore be delivered **over the LAN** as well as over the
> cable, and a bad one does not brick the device. Read
> [Stage 4](#stage-4--release-mode) before starting anything below, and see
> [Updating firmware](updates).

---

## Path 1 — what is in the tree right now

The board is in a **fully reversible** state. No eFuse has been touched.

`sdkconfig.defaults`:

```
# CONFIG_SECURE_FLASH_ENC_ENABLED is not set
# CONFIG_NVS_ENCRYPTION is not set
# CONFIG_SECURE_BOOT is not set
# CONFIG_SECURE_BOOT_V1_ENABLED is not set
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set
```

- `CONFIG_PARTITION_TABLE_OFFSET` is **not** set → ESP-IDF default `0x8000`.
- `with_ota.csv` (the **active** table) has **no** `nvs_keys` partition. It is
  `nvs` at `0x9000` (92 KiB), `otadata` at `0x20000` (8 KiB), `app0`/`ota_0` at
  `0x30000` and `app1`/`ota_1` at `0x200000` (1856 KiB each), then `spiffs` at
  `0x3E0000`. The device updates itself over the air, with automatic rollback.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is **on**, and must stay on. Rollback
  needs the `otadata` partition to record which slot is pending, and
  `main/main.cpp` calls `homeSpan.markSketchOK()` once the system is up so a good
  image is confirmed instead of abandoned.
- `no_ota.csv` (a single 3840 KiB `factory` slot, no `otadata`) still exists as a
  **fallback** if the dual-slot headroom ever becomes a problem, but it is **not**
  the active table and moving to it is a separate, deliberate change.

### Verifying no eFuses are burned

Before starting Path 2, confirm the chip is still virgin:

```
idf.py -p /dev/cu.usbserial-0001 efuse-summary
```

Expected (all pristine):

| eFuse | Expected value |
|---|---|
| `FLASH_CRYPT_CNT` | `0 R/W (0b0000000)` |
| `FLASH_CRYPT_CONFIG` | `0 R/W (0x0)` |
| `BLOCK1` (`flash_encryption`) | all `00`s |
| `ABS_DONE_0` | `False` |
| `JTAG_DISABLE` | `False` |

If any of those differ, **stop** — Path 2 has already partially begun and the
plan below needs to be revised before proceeding.

### Returning to Path 1 at any time

Path 1 is the fallback for everything. To get back to it:

1. Confirm `CONFIG_SECURE_FLASH_ENC_ENABLED` / `CONFIG_SECURE_BOOT` are disabled
   in `sdkconfig.defaults`.
2. `idf.py fullclean`
3. `idf.py build flash monitor`

> `idf.py fullclean` deletes `build/_deps` and has been observed to leave
> `managed_components/` incomplete (a missing header under `espp__serialization`).
> If the build then fails on a component header, move that component directory
> aside and run `idf.py reconfigure` so the component manager re-downloads it
> whole.

This only works while the eFuses are untouched. **Once Path 2 is executed, this
fallback no longer exists.**

---

## Path 2 — the deferred, permanent rollout

> **Read this section completely before running any command in it.**
> Every step below burns an eFuse. Nothing here can be undone.

### Prerequisites

All must be true before starting:

- [ ] Path 1 build is validated on hardware: boots, provisioning works, HomeKit
      pairs, MQTT contract verified, and both a **serial** and an **OTA** flash
      cycle have been done end to end. The OTA cycle matters here: on this layout
      it is the route you will depend on after Release mode, and rollback is what
      saves you if a signed image is bad.
- [ ] `idf.py efuse-summary` confirms **all** eFuses still pristine (table above).
- [ ] `keys/secure_boot_signing_key.pem` exists and is **backed up off-machine**.
      Losing it means the device can never be re-flashed or updated again.
- [ ] A **second, spare ESP32** is available. Do the first Path 2 attempt on the
      spare, not on the board you depend on.
- [ ] You accept that the device will need to be re-provisioned: **Wi-Fi
      credentials, HomeKit pairing, and HomeKey enrolments are all lost.** The
      layout also moves `nvs` from `0x9000` to `0xE000`, so nothing in the
      existing NVS partition carries over even in principle.
- [ ] The board is on a **stable, uninterrupted power supply**. Cutting power
      during the first-boot encryption pass corrupts flash.

### Order of operations

Enable the features **one at a time**. Do not jump straight to everything-on.
Each stage should be flashed and boot-verified before the next begins, so that
if something fails you know which change caused it.

#### Stage 1 — Flash encryption only

1. Generate the flash encryption key on the host:

   ```
   idf.py secure-generate-flash-encryption-key keys/flash_encryption_key.bin
   ```

   **Back this file up off-machine immediately.** Without it the device can
   never be re-flashed in Release mode.

2. Burn the key into eFuse BLK1. The ESP-IDF wrapper is `idf.py efuse-burn-key`:

   ```
   idf.py -p /dev/cu.usbserial-0001 efuse-burn-key flash_encryption keys/flash_encryption_key.bin
   ```

   By default this **read- and write-protects the key**: after this command the
   key cannot be read back or changed, by software or otherwise.
   (`--no-protect-key` disables that protection. Do **not** use it in
   production; it is only useful for lab work.)

3. In `sdkconfig.defaults`, enable **only** flash encryption:

   ```
   CONFIG_SECURE_FLASH_ENC_ENABLED=y
   CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y
   ```

   Keep Development mode for this stage. Release mode permanently disables
   UART download mode and you will want that escape hatch until everything is
   proven.

4. Build and flash:

   ```
   idf.py build flash
   ```

   The first boot encrypts the flash in place. **This can take up to a minute
   for large partitions — do not cut power.**

5. Verify: the boot log should show `flash_encrypt: flash encryption is enabled`
   and the app should start.

#### Stage 2 — NVS encryption

Only after Stage 1 is verified.

1. Enable in `sdkconfig.defaults`:

   ```
   CONFIG_SECURE_FLASH_ENC_USE_ENCRYPTED_NVS=y
   CONFIG_NVS_ENCRYPTION=y
   ```

2. Add an `nvs_keys` partition and move the partition table to `0xD000`. The
   signed bootloader no longer fits in the 28 KiB window below `0x8000`, so the
   table has to move to leave it room:

   ```
   CONFIG_PARTITION_TABLE_OFFSET=0xD000
   ```

   Keep the **dual-slot** shape — `ota_0` + `ota_1` + `otadata` — so over-the-air
   updates and rollback keep working. Only add `nvs_keys` and move the table:

   ```
   # Name,   Type, SubType,  Offset,   Size,     Flags
   nvs,      data, nvs,      0xE000,   0x17000,
   nvs_keys, data, nvs_keys, 0x25000,  0x1000, encrypted
   otadata,  data, ota,      0x26000,  0x2000,
   app0,     app,  ota_0,    0x30000,  0x1D0000,
   app1,     app,  ota_1,    0x200000, 0x1D0000,
   spiffs,   data, spiffs,   0x3E0000, 0x20000,
   ```

   Notes:
   - `nvs_keys` must exist and be flagged `encrypted`, otherwise the device
     fails to boot.
   - Keep the app partitions on **64 KiB boundaries** (`0x30000`, `0x200000`) and
     keep `spiffs` at `0x3E0000` — the layout still ends exactly at `0x400000`
     for a 4 MB chip. Verify any edit with
     `python "$IDF_PATH/components/partition_table/gen_esp32part.py" with_ota.csv`
     before flashing; a table that does not fit the flash builds cleanly and
     fails only on the device.
   - `otadata` **must** be present. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` needs
     it to record which slot is pending, and without it
     `bootloader_utility_get_selected_boot_partition()` has no slot to select.
     Dropping `otadata` while leaving `ota_0`/`ota_1` produces a device that
     resets in a loop, and the partition tool will not warn you — it validates
     structure, not bootability.
   - `nvs` moves from `0x9000` to `0xE000`. Its **size is kept at 92 KiB** rather
     than shrunk: NVS is log-structured and only moves forward, so entries
     written since the partition grew may sit anywhere in it, and shrinking it
     could silently drop Wi-Fi credentials, HomeKit pairing or reader enrolment.
     The move is itself destructive to whatever is currently stored, so this
     stage costs a full re-provisioning regardless.
   - The `nvs` partition itself **cannot** be flash-encrypted; NVS has its own
     encryption layer. Adding `encrypted` to `nvs` breaks it.
   - See [Updating firmware](/HomeKey-ESP32/updates/#the-layout-and-why-nvs-did-not-shrink) for how
     the sizes in the current table were arrived at.

3. Re-flash. The device wipes NVS and re-initialises it with the new key.

#### Stage 3 — Secure Boot V1

Only after Stages 1 and 2 are verified.

1. Generate the signing key (original ESP32 V1 requires **ECDSA-P256**; RSA is
   not supported here — `--scheme rsa3072` fails with
   *"V1 only supports ECDSA256"*):

   ```
   espsecure.py generate_signing_key --version 1 keys/secure_boot_signing_key.pem
   ```

2. **Back it up off-machine.** Then enable in `sdkconfig.defaults`:

   ```
   CONFIG_SECURE_BOOT=y
   CONFIG_SECURE_BOOT_V1_ENABLED=y
   CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
   CONFIG_SECURE_BOOTLOADER_ONE_TIME_FLASH=y
   CONFIG_SECURE_BOOT_SIGNING_KEY="keys/secure_boot_signing_key.pem"
   ```

   `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` is deliberately **not** set.
   It would add signature verification on top of Secure Boot for over-the-air
   images without Secure Boot being enabled; with Secure Boot on, the ROM and
   bootloader already authenticate what is loaded, so it is redundant here.

3. Build, then flash. The first boot burns `ABS_DONE_0` and the key digest. From
   that point the device only boots bootloaders signed with that key.

   > `CONFIG_SECURE_BOOTLOADER_ONE_TIME_FLASH=y` means, in ESP-IDF's own words,
   > *"the bootloader cannot be changed after the first time it is booted."*
   > If you need to re-flash the bootloader over serial later you must instead
   > select the **Re-flashable** mode.

4. Confirm the boot log reports Secure Boot enabled.

#### Stage 4 — Release mode (production only)

Last, and only for devices that are being deployed:

```
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

This permanently disables UART download mode and write-protects
`FLASH_CRYPT_CNT`.

> [!CAUTION]
> **On this layout, Stage 4 removes your last recovery route.** After Release mode
> the serial port can no longer write to this device, so **network OTA is the only
> way to change firmware from then on** — and it only accepts images signed with
> your Secure Boot key. There is no route back to a previous firmware unless you
> kept a signed copy of it, and no way to re-key the device. Treat Stage 4 as the
> decision to ship this firmware to this hardware, permanently.
>
> This is a real change from the single-slot layout, where Release mode left no
> update path at all and a bad image was unrecoverable. Here, rollback plus signed
> OTA means a faulty update can be replaced — **provided you still hold the signing
> key**. Lose that key and the device is frozen exactly as it is.
>
> If that is not what you want, **stop at Stage 3.** Secure Boot plus flash
> encryption in Development mode already gives you an encrypted, authenticated
> image. You keep serial re-flashing, and give up only the guarantee that the
> UART port is closed.

### Post-rollout

- Store both keys off-machine, in separate locations.
- Record the device MAC and the eFuse summary as the device's security baseline.
- Update this document's status line to "Path 2 active".

---

## Things that are true in both paths

These were wrong in earlier revisions of the docs and are recorded here to stop
them being repeated:

| Claim | Reality |
|---|---|
| `idf.py flash-encrypt` exists | **False.** Not a target. The real ones are `idf.py encrypted-flash`, `idf.py encrypted-app-flash`, `idf.py efuse-burn-key`, `idf.py secure-generate-flash-encryption-key`. |
| "Development mode makes flash encryption reversible" | **Misleading.** Dev mode keeps *plaintext re-flashing* possible; the burned key and `FLASH_CRYPT_CNT` are still permanent. |
| "The first boot always burns the eFuse" | **Only if** the build has flash encryption enabled *and* reaches the encryption step. A chip with encryption enabled in config but no key burned will abort in `esp_flash_encryption_init_checks` before encrypting. |
| Secure Boot V1 supports RSA-3072 | **False.** V1 on original ESP32 is ECDSA-P256 only. RSA-3072 is Secure Boot V2, which ESP32 does not support. |

---

## Related documents

- [Security](security) — security feature overview
- [Fork vs Upstream](fork-vs-upstream) — fork vs. upstream differences
- [Updating firmware](updates) — serial and over-the-air update procedure, and breaking changes
- [Single-slot layout](single_slot_layout) — the alternative layout this plan can target
