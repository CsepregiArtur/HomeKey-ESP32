---
title: "Single-slot layout (no OTA)"
weight: 12
---

# Single-slot layout (no OTA)

This page is the plan and the migration procedure for the change from a
dual-slot (OTA) flash layout to a **single application slot**. It is a
**one-way door for the node**: after it, a device can no longer update itself
over the air, and the change itself cannot be delivered over OTA either.

## Why

The application partition was the tightest resource on the device, and it was
tight *because half the flash was reserved for the copy of the firmware used
only during an update*:

| | with_ota.csv | no_ota.csv | change |
|---|---|---|---|
| `nvs` | 0x9000, 24 KiB | 0x9000, **92 KiB** | +68 KiB |
| `otadata` | 0xF000, 8 KiB | — | removed |
| `app0` | 0x20000, 1920 KiB (`ota_0`) | 0x20000, **3840 KiB** (`factory`) | +1920 KiB |
| `app1` | 0x200000, 1920 KiB (`ota_1`) | — | removed |
| `spiffs` | 0x3E0000, 128 KiB | 0x3E0000, 128 KiB | unchanged |

Measured before the change (`idf.py build`):

- app image `1,900,532 B` of `1,966,080 B` → **65,548 B free (3.3 %)**
- NVS reported `FreeEntries` uncomfortably close to the audit ring's live
  footprint (see [Configuration]({{< relref "configuration.md" >}}))

So the app had 3 % headroom and NVS had the audit ring permanently resident.
After the change the app has ~50 % headroom and NVS has room to breathe. Both
problems were the same problem: a second application slot that only an OTA
needed.

## What it costs

Be honest about the trade, because it is not reversible in the field:

1. **No more OTA updates.** There is no second slot to write to and no OTA data
   partition to switch to. `POST /ota/*`, the OTA page and HomeSpan's
   ArduinoOTA have nothing to write to on this layout.
2. **Every existing device needs one serial flash.** A partition table cannot be
   delivered over OTA - the node writes the new image into a slot the old table
   describes. Changing the table means a cable.
3. **A bricked OTA is no longer survivable by rollback.** With one slot there is
   no previous image to fall back to; a bad flash is recovered by re-flashing
   over serial, not by the bootloader.

If any of those is unacceptable for a given deployment, stay on
`with_ota.csv` - both tables ship in the tree on purpose.

## What survives the change

The reflash writes the bootloader, the partition table, the application and the
UI image (`spiffs`). It does **not** erase `nvs`, and `nvs` keeps its offset, so
these are still there afterwards:

- HomeKit pairing (`hapNVS`), Wi-Fi credentials (`wifiNVS`)
- the reader identity and enrolled credentials (`READERDATA`)
- household membership, recovery secret and salt
- MQTT / Web UI configuration, audit log, node identity

> Take a backup before flashing anyway
> (`POST /backup/create`, or the Web UI's Recovery/Backup page). The reflash
> should not touch any of it, but a partially written flash is not something to
> discover without one.

## Procedure: existing device → single slot

1. **Take a backup** and keep the recovery secret somewhere real. See
   [Backup and restore]({{< relref "household.md" >}}).
2. **Record the current firmware version** so the result is comparable.
3. **Serial-flash the whole layout**, not just the app. The partition table and
   the bootloader are both written, so `idf.py flash` (or `pio run -t upload`)
   is the right command - a partial "app only" flash would leave the device
   describing slots that are not there.
4. **Confirm on the boot log** that the device came up on the new table:
   the boot loader line reports `factory` rather than `ota_0`, and the app's
   free-space line is about 3840 KiB rather than 1920 KiB.
5. **Verify the parts that live in `nvs`** survived: HomeKit still paired,
   Wi-Fi joined, reader enrolled, household still `ACTIVE`.
6. **Verify the API** the integration actually uses:
   `POST /backup/create` with **no body** must answer
   `{"success":true,"includes_credentials":false,"backup":"…"}`. This is the
   call that regressed on firmware `8fdc005` and is fixed in `e8e7190`.

## Rolling back to OTA

Not a one-way burn: it is another serial flash.

1. Restore `board_build.partitions` / `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`
   to `with_ota.csv` and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
2. Rebuild, then serial-flash the bootloader **and** the partition table
   (`idf.py flash`).
3. `nvs` and `spiffs` are at the same offsets in both tables, so they are kept
   again. `otadata` is recreated empty, which simply means "boot `ota_0`".

## Follow-up work — DONE (2026-09-26)

All of the below landed in the same change as the layout:

- [x] `main/WebServerManager.cpp`: the `/ota/*`, `/ota/release` and `/ota/install` routes
      and every handler behind them, plus the GitHub updater (`httpGetToString`,
      `resolveRelease`, `streamIntoOta`, `streamIntoFilesystem`, `githubOtaTask`, `otaTask`,
      `getOTAInfo`, `broadcastOTAStatus`). 819 lines. A comment where the code was records why.
- [x] `main/HomeKitLock.cpp`: `homeSpan.enableOTA(...)` and the whole OTA-password path.
- [x] `data/src/routes/ota/` and `data/src/lib/components/AppOTA.svelte` deleted, with the
      navigation entry, the `getReleaseInfo`/`installRelease` API client and the `OTAStatus`
      type.
- [x] `sdkconfig.defaults`: the signed-OTA block is off; `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`
      is explicitly `not set`.
- [x] Docs: this page, `updates.md` (a prominent notice), and the layout references.
- [x] Version: `HK_APP_VERSION` and `data/package.json` to `0.11.0`, `CHANGELOG.md` updated.

Verified on hardware: `ota_status`, `ota_info`, `/ota/install`, `/ota/release`,
`gh_ota_task`, `esp32.firmware.bin` and `otaPasswd` are all **absent from the built image**,
`otaPasswd` is gone from `GET /config?type=misc`, no `ota_*` finding remains in
`GET /security`, and `POST /backup/create` still answers.

The only surviving `homespan-ota` string is HomeSpan's own default inside the vendored
upstream component; nothing enables its OTA service, so it is inert.

> **Version reporting caveat.** `HK_APP_VERSION` only reaches the image when the tree is
> not descended from a tag - the root `CMakeLists.txt` prefers `git describe --tags` and
> falls back to `HK_APP_VERSION-dev+<hash>`. Until a `v0.11.0` tag exists the device still
> reports `v0.10.0-<n>-g<hash>`, so tag the release as well as bumping the constant.
