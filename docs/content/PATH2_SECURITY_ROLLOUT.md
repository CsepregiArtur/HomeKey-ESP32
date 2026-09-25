# Security Rollout Plan: Path 1 → Path 2

> **Status:** Path 1 active. Path 2 is **deferred** until the firmware is fully
> validated on real hardware.
> **Decision date:** 2026-09-25
> **Owner:** @CsepregiArtur

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

That means: the board boots reliably, OTA works, provisioning works, HomeKit
pairs, the MQTT contract behaves, and the backup/restore cycle passes — all on
an unencrypted build. Only then does it make sense to make the part that is hard
to change permanent.

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
- `with_ota.csv` has **no** `nvs_keys` partition; first partition is `nvs` at
  `0x9000`.

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

This only works while the eFuses are untouched. **Once Path 2 is executed, this
fallback no longer exists.**

---

## Path 2 — the deferred, permanent rollout

> **Read this section completely before running any command in it.**
> Every step below burns an eFuse. Nothing here can be undone.

### Prerequisites

All must be true before starting:

- [ ] Path 1 build is validated on hardware: boots, OTA works, provisioning
      works, HomeKit pairs, MQTT contract verified.
- [ ] `idf.py efuse-summary` confirms **all** eFuses still pristine (table above).
- [ ] `keys/secure_boot_signing_key.pem` exists and is **backed up off-machine**.
      Losing it means the device can never be re-flashed or updated again.
- [ ] A **second, spare ESP32** is available. Do the first Path 2 attempt on the
      spare, not on the board you depend on.
- [ ] You accept that the device will need to be re-provisioned: **Wi-Fi
      credentials, HomeKit pairing, and HomeKey enrolments are all lost.**
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

2. Add `nvs_keys` to `with_ota.csv` and move the partition table to `0xD000`:

   ```
   CONFIG_PARTITION_TABLE_OFFSET=0xD000
   ```

   ```
   nvs,      data, nvs,      0xE000,   0x6000,
   nvs_keys, data, nvs_keys, 0x14000,  0x1000, encrypted
   otadata,  data, ota,      0x15000,  0x2000,
   app0,     app,  ota_0,    0x20000,  0x1E0000,
   app1,     app,  ota_1,    0x200000, 0x1E0000,
   spiffs,   data, spiffs,   0x3E0000, 0x20000,
   ```

   Notes:
   - `nvs_keys` must exist and be flagged `encrypted`, otherwise the device
     fails to boot.
   - App partitions must be 64 KiB aligned — `app0` at `0x20000`, not `0x17000`.
   - The `nvs` partition itself **cannot** be flash-encrypted; NVS has its own
     encryption layer. Adding `encrypted` to `nvs` breaks it.

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
   CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
   ```

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
`FLASH_CRYPT_CNT`. After this, new firmware can **only** be installed via OTA
signed with the Secure Boot key. There is no serial recovery.

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

- [`security.md`](./security.md) — security feature overview
- [`fork-vs-upstream.md`](./fork-vs-upstream.md) — fork vs. upstream differences
- [`updates.md`](./updates.md) — OTA and update behaviour
