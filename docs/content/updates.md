---
title: "Updating firmware"
weight: 5
---

# Updating firmware

The device uses a **dual-slot** layout (`with_ota.csv`): two application
partitions, `ota_0` and `ota_1`, plus an `otadata` partition that records which
one to boot. An update is written into the slot the device is *not* running
from, so the running firmware is never overwritten while it is executing.

There are two ways to install an image, and they are not interchangeable.

| | Over the cable | Over the network |
|---|---|---|
| Writes the partition table | **yes** | no |
| Writes the bootloader | **yes** | no |
| Writes the web UI filesystem | **yes** | no |
| Works on a device that has never been OTA-flashed | **yes** | no |
| Can update several devices in one run | one at a time | yes |
| Needs physical access | yes | no |

**Moving an existing device onto this layout needs one serial flash.** A
partition table cannot be delivered over the air: the old table is what tells
the device where to write the new image, and it does not describe the new slots.
After that one cable, every later firmware update can be wireless.

## The layout, and why `nvs` did not shrink

| Partition | Offset | Size | |
|---|---|---|---|
| `nvs` | 0x9000 | 92 KiB | unchanged from the single-slot layout |
| `otadata` | 0x20000 | 8 KiB | which slot to boot |
| `app0` (`ota_0`) | 0x30000 | 1856 KiB | |
| `app1` (`ota_1`) | 0x200000 | 1856 KiB | |
| `spiffs` | 0x3E0000 | 128 KiB | the web UI filesystem |

`nvs` keeps the **size and offset** the single-slot layout gave it instead of
going back to 24 KiB. NVS is a log-structured store that only moves forward:
entries written since the partition grew may sit anywhere in those 92 KiB, so
shrinking it back could silently drop Wi-Fi credentials, HomeKit pairing or
reader enrolment. The 68 KiB that buys comes out of the application slots, which
are 1856 KiB each instead of 1920 KiB.

Two small holes are unavoidable: application partitions must start on a 64 KiB
boundary, so the slot after `otadata` begins at 0x30000 and a 64 KiB tail before
`spiffs` cannot be used.

Measure it yourself with:

```bash
python "$IDF_PATH/components/partition_table/gen_esp32part.py" with_ota.csv
```

## Safety: rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A freshly installed image is marked
*pending verify*, and the bootloader abandons it for the previous slot unless the
running image confirms itself. `setup()` in `main/main.cpp` confirms once
HomeKit, the web server and the reader are all up, so an image that crashes
before that point is rolled back instead of kept. **Without that confirmation an
update would appear to succeed and then silently revert on the next power cycle.**

The cost is honest: a device that boots a *good* image but fails later - a
watchdog reset in `loop()`, say - is still rolled back, and the update looks like
it never happened. Check the boot log's `Running partition` line after an update
to see which slot is live.

## 1. From the Web UI

**Update** in the navigation menu. The page reports the running version, which
partition it booted from, and the size of the application slot, then takes a
`.bin` file and installs it.

The page sends the image in a single request and shows upload progress, because
1.7 MB over Wi-Fi takes long enough that a silent bar looks like a hang. On
success the device reboots into the new image after about a second.

The page needs **HTTPS** and refuses over plain HTTP with an explanatory message.
An image is the most valuable thing a caller can send, and letting it cross the
LAN in the clear would let anyone on the network read it and swap it for their
own.

## 2. From the command line

`scripts/ota_update.py` drives both paths and is the way to update several
devices at once.

```bash
./scripts/ota_update.py                 # discover, ask, update
./scripts/ota_update.py --list          # just show what was found
./scripts/ota_update.py --port /dev/cu.usbmodem1101 --with-fs
./scripts/ota_update.py --prepare-for-ui
```

With no arguments it looks for an attached device first. If it finds one it
offers to flash over the cable; otherwise it discovers devices over mDNS and asks
which to update - one of them, or all of them.

### Preparing a file for the web UI

Passing `--prepare-for-ui`, or answering yes when the script asks, copies the
built image to `homekey-ota-<size>-0x30000.bin` next to a `.sha256` sidecar file.

```bash
./scripts/ota_update.py --prepare-for-ui
```

The number in the name is the `app0` offset from `with_ota.csv`. It exists because
a build output that is correct for `esptool write_flash` and one that is correct
for the web UI's file picker are the same bytes under different names, and
choosing the wrong `.bin` by hand is an easy mistake that only shows up as a
failed install. These files are gitignored.

### Where the password lives

The device's Web UI password is read from the macOS Keychain, service
`homekey-esp32-ota`, account `web-ui`. If it is not there the script asks for it
once and stores it. It is never written to a file, and never printed.

```bash
./scripts/ota_update.py --forget-password           # delete it
./scripts/ota_update.py --keychain-account other    # use a different account
```

`--password` exists for scripting but shows up in the process list; prefer the
Keychain.

### What it verifies

- **The certificate is pinned to the device's advertised fingerprint.** The
  device's certificate is self-signed with `CN=HK` and a fixed validity (it is
  generated on a device with no clock, at an address that changes with DHCP), so
  neither the CA chain nor the hostname can be checked. The SHA-256 of the
  certificate is checked immediately after the handshake, before anything is
  sent, and a mismatch aborts. That is the actual verification, and it is what
  makes discovery safe: a device that answers must present the expected
  certificate.
- **The image fits.** The size is compared with the `app0` size before anything
  is uploaded, so an oversized image fails in a second instead of after a minute
  of Wi-Fi.
- **Every secret is masked before printing.** Anything printed from a device's
  JSON passes through a redactor that replaces values whose key contains
  `password`, `passwd`, `secret`, `token` or `psk`.

### Known limitation

`dns-sd`, macOS's mDNS client, frequently produces no output when its stdout is
not a terminal. When that happens discovery falls back to probing a short list of
addresses in the same /24, and only hosts running this device's update endpoint
answer at all. If neither finds anything, pass `--port` and use the cable.

## Compile targets

The script detects the chip on the other end of the cable and refuses to write an
image built for a different one, because that produces a reset loop rather than an
error message. **Only `esp32` (the original, Xtensa) is buildable from this tree as
it is configured.**

An **ESP32-C3** is detected correctly - the script reports
`ESP32-C3 (QFN32) (revision v0.4) 4MB flash` - but then stops with an explanation,
because the host build does not match:

* The RISC-V toolchain is not part of a default ESP-IDF install:
  `python "$IDF_PATH/tools/idf_tools.py" install riscv32-esp-elf`.
* Even installed, the compile fails in this environment. `tools/cmake/toolchain-esp32c3.cmake`
  sets `_CMAKE_TOOLCHAIN_PREFIX riscv32-esp-elf-` and IDF's `toolchain.cmake` then
  assigns the bare name (`set(CMAKE_C_COMPILER riscv32-esp-elf-gcc)`). CMake 4.4
  (Homebrew) resolves that to Apple's `clang` and the compile dies with
  `clang: error: unknown argument: '--traditional-format'`. Forcing `CC`/`CXX` to
  full paths does not override it, because the toolchain file sets them itself.

So a C3 build needs a host-side fix first (a CMake that resolves the RISC-V prefix,
or an `-DCMAKE_C_COMPILER=<absolute path>` passed into the configure step). The
board's own flash is not the constraint: its 4 MB matches `with_ota.csv` exactly.

To add a target once that is sorted out, add it to `SUPPORTED_TARGETS` in
`scripts/ota_update.py`, which currently gates `--target` and the automatic rebuild.


## 3. From a build

```bash
source ~/esp/esp-idf/export.sh
export CI=true CMAKE_POLICY_VERSION_MINIMUM=3.5
(cd data && npm install && npm run build && find dist \( -name '*.css' -o -name '*.js' \) -delete)
idf.py build
./scripts/ota_update.py --port /dev/cu.usbmodemXXXX --with-fs   # first time
./scripts/ota_update.py                                         # after that
```

`idf.py flash` and `esptool.py` still work directly; the offsets for the current
table are printed by the build, or read them from `with_ota.csv`.

## Going back to a single slot

`no_ota.csv` is still in the tree. It gives one 3840 KiB `factory` application
slot and no `otadata`, which is ~55% free instead of the ~8% the dual-slot layout
leaves, and removes the ability to update over the network entirely.

Switching back is another serial flash, and `nvs` and `spiffs` keep their offsets
in both tables, so credentials and the UI survive it. Change
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in `sdkconfig.defaults` and
`board_build.partitions` in `platformio.ini`, then reflash the bootloader and the
partition table.

## Which version am I running?

| Where | Shows |
|---|---|
| Web UI → Update | firmware version, and which partition booted |
| Web UI → Info | firmware version and UI version |
| Apple Home → accessory settings | firmware revision |
| Boot log | `Running partition` line |

How to read the value:

| Value | Meaning |
|---|---|
| `v0.11.0` | a tagged release |
| `0.11.0-dev+1a2b3c4` | built from a branch, at that commit |
| `0.11.0-dev+1a2b3c4-dirty` | as above, with uncommitted changes - not a release |

> **Version reporting caveat.** `HK_APP_VERSION` in the root `CMakeLists.txt`
> only reaches the image when the tree is not descended from a tag; otherwise the
> build reports `git describe --tags`. Bumping the constant is not enough on its
> own.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Update page says HTTPS is required | Enable HTTPS under Misc → Security. The endpoint refuses plain HTTP on purpose. |
| `401` from the script | The Keychain password does not match the device. `--forget-password`, then run again. |
| Upload starts, then the connection drops | Wi-Fi interference, or the device rebooting mid-write. An interrupted upload is **not** applied; the device is still on the firmware it had. |
| Device reboots but runs the old version | The new image did not confirm itself and was rolled back. Check the boot log for a panic before the confirmation point. |
| Device reboots in a loop | It is in the OTA slot and failing before confirming. Flash over the cable; the previous image is in the other slot. |
| `single-slot firmware, nothing to update into` | The device is on `no_ota.csv`. It needs one serial flash first. |
