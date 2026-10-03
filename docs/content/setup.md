---
title: "Setup"
weight: 4
---

# Bringing Your HomeKey-ESP32 to Life

Welcome to the exciting part! This guide will walk you through the process of getting your HomeKey-ESP32 device up and running. We'll cover everything from getting the firmware to flashing it onto your board and getting it connected to your network.

> [!NOTE]
> **This page describes *this fork*.** Three things differ from the
> [upstream setup guide](https://rednblkx.github.io/HomeKey-ESP32/setup/) and will
> change what you do here:
>
> | | Upstream | This fork |
> | --- | --- | --- |
> | **NFC readers** | PN532, PN7160/PN7161, ST25R3916 | **PN532 only** |
> | **Connectivity** | Wi-Fi **and** Ethernet | **Wi-Fi only** — no Ethernet |
> | **Firmware update** | OTA from the Web UI / GitHub updater | **Dual-slot OTA over the LAN** or serial |
>
> If you are running upstream firmware, use the
> [upstream documentation](https://rednblkx.github.io/HomeKey-ESP32/setup/) — the
> reader options and Ethernet settings below do not exist there, and upstream's OTA
> steps do not apply here. See [Fork vs Upstream](fork-vs-upstream).

## 1. Get the Firmware

Two options: use a pre-compiled binary, or build it yourself.

### 1.1. Pre-compiled binaries

Download from **this fork's** releases page:
{{< badge content="Releases" link="https://github.com/CsepregiArtur/HomeKey-ESP32/releases/latest" icon="github" >}}

Under the "Assets" section you'll find:

* **`*.firmware.factory.bin`**: Used to flash a **new device for the first time**. It contains the bootloader, application and LittleFS filesystem merged into one, ready to be flashed at address `0x0`.
* **`*.firmware.bin`**: The application firmware only. Written at the application partition offset — this is what you feed to the [Web UI Update page](updates#1-from-the-web-ui) or to `esptool` for a single-partition update.
* **`littlefs.bin`**: The web interface files, written to the filesystem partition.

> [!CAUTION]
> **Use this fork's releases, not upstream's.** The two are not interchangeable:
> this fork removed Ethernet and two NFC readers, and uses a different partition
> table. Flashing an upstream binary gives you a device whose configuration options
> do not match this documentation.

More detail in the [Updates Guide]({{< ref "updates" >}}).

### 1.2. Building it yourself

Requires **ESP-IDF v5.4 or newer** (CI builds against v5.5.4). Earlier versions do
not compile — the NFC components use `esp_log_buffer.h` and
`spi_bus_dma_memory_alloc`, both introduced in v5.4.

```bash
git submodule update --init --recursive
. $HOME/esp/esp-idf/export.sh
idf.py build
```

For the **ESP32-C3** you need one extra step on macOS/Homebrew hosts. Use the
wrapper, which handles the toolchain and the assembler workaround:

```bash
./scripts/build_esp32c3.sh set-target   # one time; wipes build/
./scripts/build_esp32c3.sh build
```

See [Compile targets](#7-compile-targets) below for why.

## 2. Connect Your Hardware

Before flashing, connect your **PN532** to your ESP32 or ESP32-C3 board.

> [!IMPORTANT]
> **Only the PN532 is supported in this fork.** The PN7160/PN7161 and ST25R3916
> backends were removed to free flash for the second OTA slot. If you have one of
> those modules, you need upstream firmware — see
> [Fork vs Upstream](fork-vs-upstream).

* **Using Jumper Wires:** Connect the PN532 to the SPI pins for *your* chip — see [PN532 Module Wiring](#21-pn532-module-wiring).
* **Using an Integrated PCB Board:** If you have an [Integrated PCB Board](../prerequisites#22-option-b---integrated-pcb-boards), connections are pre-wired. Select the corresponding hardware preset in the Captive Portal or WebUI.

### 2.1. PN532 Module Wiring

> [!IMPORTANT]
> Both the ESP32 and the PN532 must share a common power supply and ground.

> [!WARNING]
> **The pin numbers differ per chip, and copying them across boards produces a dead
> SPI bus rather than an error.** On a classic ESP32 the PN532 uses VSPI on
> GPIO18/19/23/5. On an ESP32-C3 it uses FSPI on GPIO4/5/6/7 — and the C3 only has
> GPIO0–21, so GPIO18/19/23 **do not exist** on that chip at all.

#### 2.1.1. PN532 Wiring (SPI Mode)

> [!CAUTION]
> The PN532 must be configured for SPI mode.
> On standard red boards, set the DIP switch to `0` and `1` (left switch down towards 1, right switch up away from 2).
> ![PN532 SPI Mode](/images/IMG_4025.jpeg)

**ESP32 (classic) — VSPI:**

| ESP32 Pin | PN532 Pin |
| :-------- | :-------- |
| VCC/3V3   | VCC       |
| GND       | GND       |
| GPIO18    | SCK       |
| GPIO19    | MISO      |
| GPIO23    | MOSI      |
| GPIO5     | SS        |

**ESP32-C3 — FSPI:**

| ESP32-C3 Pin | PN532 Pin |
| :----------- | :-------- |
| VCC/3V3      | VCC       |
| GND          | GND       |
| **GPIO4**    | SCK       |
| **GPIO5**    | MISO      |
| **GPIO6**    | MOSI      |
| **GPIO7**    | SS        |

These defaults come from the Arduino core's per-chip variant, so they are correct
for both chips without changing anything — but they are **not** the same numbers.

> [!NOTE]
> The default GPIO pinout can be seen and changed in the WebUI [System section]({{< ref "configuration" >}}#522-nfc-reader-configuration).

#### 2.1.2. Pins to avoid on an ESP32-C3

The C3 has fewer usable pins than a classic ESP32. Avoid these unless you know what
you are doing:

| Pin | Why |
| --- | --- |
| GPIO9 | Boot mode select |
| GPIO8, GPIO2 | Strapping pins (boot behaviour) |
| GPIO18, GPIO19 | Native USB D-/D+ |
| GPIO20, GPIO21 | Console UART (TX/RX) |

Assigning a strapping pin in the WebUI is rejected unless you enable
`overrideStrappingRestriction` — see [Configuration]({{< ref "configuration" >}}#521-gpio-allocation--safety).

#### 2.1.3. Integrated PCB Board Presets

When using an Integrated PCB or predefined layout, select the hardware preset in the Captive Portal or WebUI:

1. **@lollokara's board (ESP32-C3)** (SPI)
2. **CASmo-NFC** (SPI)

> [!NOTE]
> The upstream `CASmo-NFC-MB-ETH` preset is **not available in this fork**, because
> the Ethernet driver was removed. Use `CASmo-NFC` instead if your board's Ethernet
> port is unused.

## 3. Flash the Firmware

You can flash the firmware with the included script, with command-line `esptool.py`,
or with browser-based `esptool-js`.

{{< tabs items="ota_update.py,esptool.py,esptool-js" >}}
{{< tab >}}

**Recommended.** The script detects the chip, picks the correct bootloader offset,
and offers to flash over the cable or over the network:

```bash
./scripts/ota_update.py                 # discover, ask, update
./scripts/ota_update.py --list          # just show what was found
./scripts/ota_update.py --port /dev/cu.usbmodem1101 --with-fs
```

This is the only path that handles the **ESP32-C3's bootloader offset (`0x0`)**
automatically. See [Updates]({{< ref "updates" >}}).

{{< /tab >}}
{{< tab >}}

1. **Install esptool.py:**

    ```bash
    pip install esptool
    ```

2. **Connect the board:** via USB to your computer.
3. **Identify the serial port:** `/dev/ttyUSB0` or `/dev/ttyACM0` (Linux), `/dev/cu.usbserial-XXXX` (macOS), or `COMx` (Windows).
4. **Run the flash command:**

    ```bash
    esptool.py --port YOUR_PORT write_flash 0x0 *.firmware.factory.bin
    ```

> [!WARNING]
> `write_flash 0x0` is correct for the **merged factory image** only. If you flash
> the individual `bootloader.bin` / `partition-table.bin` / app images by hand, the
> bootloader offset is **`0x1000` on a classic ESP32 but `0x0` on an ESP32-C3**.
> Writing a C3 bootloader to `0x1000` leaves the chip printing
> `invalid header: 0xffffffff` forever. Use `./scripts/ota_update.py` to avoid this.

{{< /tab >}}
{{< tab >}}

1. Connect your board to your computer using USB.
2. Open [https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/) in any Chromium-based browser (e.g. Chrome/Brave/Edge).
3. Select your `*.firmware.factory.bin` file, set Flash Address to `0x0`, click **Connect**, then click **Program**.

> [!NOTE]
> If any issues, you can use the console section to retrieve the logs and reach out to help debug the issue and fix it.

{{< /tab >}}
{{< /tabs >}}

## 4. Wi-Fi Configuration & Initial Setup

After flashing, your HomeKey-ESP32 is ready for initial configuration.

1. **Connect to Wi-Fi Access Point:** On first boot (or when no Wi-Fi credentials are saved), the device hosts an access point:
    * **SSID:** `HK_{XXXXXX}`
    * **Password:** `HomeKey$123$` until you set your own on the first-run setup screen. See [Security]({{< ref "security" >}}) for the full list of credentials first-run setup asks for.
2. **Access the Captive Portal:** The portal is not auto-detected on phones, so open `http://192.168.4.1` manually in your web browser.
3. **Configure Options:**
    * **Wi-Fi & HomeKit:** Scan and select Wi-Fi network, enter password, set 8-digit HomeKit pairing code, select HomeKey pass color (Tan, Gold, Silver, Black), configure AP Access Point Password (`accessPointPassword`), and optionally enable Web UI authentication with your own username/password.
    * **Hardware Tab:** Select the PN532 reader and a preset, assign custom NFC GPIO pins, and see strapping pin restrictions on conflicting assignments. Override them if required by custom hardware (`overrideStrappingRestriction`).
      > [!NOTE]
      > **There is no Ethernet section in this fork.** Upstream's "Enable Ethernet", PHY type and SPI-Ethernet fields do not exist here — see [Fork vs Upstream](fork-vs-upstream).
4. **Save & Connect:** Upon clicking "Save", the captive portal submits configuration diffs and connects to your Wi-Fi network. On successful connection, the interface displays the assigned network IP address before closing.
5. **Finish first-run setup:** Open the device's Web UI on your network. A blocking setup screen asks for your Web UI username and password, HomeKit Setup Code and setup AP password. Until you save it the Web UI has **no login** - see [Security]({{< ref "security" >}}).

## 5. HomeKit Pairing

On a brand-new device the Setup Code is the shipped `466-37-726` until you set your own on the first-run setup screen. Enter it in the Home app. Devices configured before this change keep their existing code. Once connected to your Wi-Fi network, open the Apple Home app, tap **Add Accessory**, and enter or scan the setup code.

## 6. Troubleshooting Common Setup Issues

* **Failed to connect during flashing:** Put board into bootloader mode manually (hold BOOT, tap RESET, release BOOT).
* **`invalid header: 0xffffffff` and the board never boots:** The bootloader was written to the wrong offset for your chip. Classic ESP32 = `0x1000`, ESP32-C3 = `0x0`. Use `./scripts/ota_update.py`, which reads the offset from the build itself.
* **`clang: error: unknown argument: '--traditional-format'` when building for the C3:** The RISC-V assembler, not the compiler. Use `./scripts/build_esp32c3.sh` — see [Compile targets](#7-compile-targets).
* **`fatal error: string.h: No such file or directory` when building for the C3:** An interrupted toolchain install left `riscv32-esp-elf/include/` empty. Reinstall the RISC-V toolchain — see [Compile targets](#7-compile-targets).
* **PN532 Not Detected:** Verify power, confirm the DIP switch is set to SPI mode, and **check the pin numbers against your chip** (GPIO18/19/23/5 on ESP32 vs GPIO4/5/6/7 on ESP32-C3).
* **Strapping Pin Errors:** Assigning a strapping pin in the WebUI (chip-specific; e.g., GPIO 0 and 2 on standard ESP32, GPIO 2/8/9 on ESP32-C3) is rejected with an error unless `overrideStrappingRestriction` is enabled — use it only if your custom hardware requires it.

Full list in [Troubleshooting]({{< ref "troubleshooting" >}}).

## 7. Compile Targets

This fork builds for **two targets**, and flashing auto-detects which one is
attached.

| | `esp32` (classic) | `esp32c3` |
| --- | --- | --- |
| Architecture | Xtensa | RISC-V |
| Bootloader offset | `0x1000` | **`0x0`** |
| PN532 SPI pins | 18 / 19 / 23 / 5 | **4 / 5 / 6 / 7** |
| Image size | 1,752,256 B | 1,870,176 B |
| Free in `ota_0` | 148,288 B (**7.80 %**) | 30,368 B (**1.60 %**) |
| Build command | `idf.py build` | `./scripts/build_esp32c3.sh build` |

### Why the C3 needs a wrapper on macOS

The RISC-V toolchain is **not installed by default**, and even once it is, a
Homebrew/macOS host fails the build with a message that points at the wrong
component:

```
clang: error: unknown argument: '--traditional-format'
```

GCC *is* running and is genuine — the failure is in the **assembly step**. IDF's
`riscv32-esp-elf-as` is not binutils: it is an Espressif **Rust dispatcher** that
selects between `riscv32-esp-elf-as-xespv1`, `-xespv2p1` and `-xespv2p2` (the real
GNU assemblers). When it cannot resolve its variant it exits non-zero, GCC's
`-print-prog-name=as` then reports the bare name `as`, and the OS resolves that to
`/usr/bin/as` — which on macOS is Apple clang, and clang rejects RISC-V flags.

Forcing `CC`/`CXX` to absolute paths, `-B`, and `COMPILER_PATH` all fail to fix it,
because the compiler is not the problem. What works is putting a directory on
`PATH` whose `as` points straight at the real GNU assembler, bypassing the
dispatcher. `scripts/build_esp32c3.sh` does exactly that.

> [!WARNING]
> **An interrupted toolchain install leaves a broken toolchain that fails much
> later.** If `riscv32-esp-elf/include/` is empty (0 headers instead of ~72), the
> build eventually dies with `fatal error: string.h: No such file or directory`.
> Check it, and reinstall if it is empty:
>
> ```bash
> ls ~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/include | wc -l   # must not be 0
> rm -rf ~/.espressif/tools/riscv32-esp-elf
> python "$IDF_PATH/tools/idf_tools.py" --non-interactive install riscv32-esp-elf
> ```

### Flash headroom

The C3 image is about **118 KB larger** than the classic ESP32 build, and it shares
the same 1856 KiB slot, so its dual-slot headroom is thin (**1.60 %**). If size
becomes a problem, `no_ota.csv` is the escape hatch: a single slot with far more
room, but no over-the-air update. See [Updates]({{< ref "updates" >}}).

