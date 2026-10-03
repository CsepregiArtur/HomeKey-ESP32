---
title: "Prerequisites"
weight: 2
---

# Gearing Up for HomeKey-ESP32

Before you embark on your HomeKey-ESP32 journey, let's make sure you have the necessary tools and hardware. Think of this as gathering your essentials before a grand adventure! Having these prerequisites in place will ensure a smooth and hassle-free experience.

## 1. Essential Software

### 1.1. `esptool`

You have two options for `esptool` to flash the firmware onto your ESP32:

{{< tabs items="esptool.py,esptool-js" >}}
  {{< tab >}}
    *   **What it is:** A command-line utility from Espressif for flashing firmware.
    *   **Why you need it:** To get the HomeKey-ESP32 firmware onto your device.
    *   **How to get it:**
        *   Download from [esptool.py GitHub Releases](https://github.com/espressif/esptool/releases)
        *   Or install via pip: `pip install esptool`
  {{< /tab >}}
  {{< tab >}}
    *   **What it is:** A browser-based version of `esptool`.
    *   **Why you need it:** For a simple graphical interface without command-line tools.
    *   **How to use it:** Access the [esptool-js demo page](https://espressif.github.io/esptool-js/).
        *   **Important:** Requires a Chromium-based browser (Chrome, Edge, Brave).
  {{< /tab >}}
{{< /tabs >}}

### 1.2. Python 3.x (Only if using esptool.py)

*   **What it is:** A versatile programming language.
*   **Why you need it:** If you choose to use the command-line `esptool.py`, you'll need a compatible Python installation for it to run correctly on your system.
*   **How to get it:** Python usually comes pre-installed on Linux and macOS. For Windows, or if you need a specific version:
    *   [Python Downloads](https://www.python.org/downloads/)

## 2. Hardware You'll Need

> [!TIP]
> Avoid powering from a MacBook as they can sometimes not supply enough current to this kind of devices and can result in unexpected behavior of the ESP32 and/or NFC module.

The required hardware can be obtained either by sourcing all the parts yourself or by using an integrated PCB that has it all on a single board.

### 2.1. Option A - Sourcing parts yourself

> [!TIP]
> It's recommended to solder the wires if you can, as DuPont connectors may cause connectivity issues.

#### 2.1.1. ESP32 Development Board

*   **What it is:** The brain of your HomeKey-ESP32 device! A microcontroller board with Wi-Fi and Bluetooth capabilities.
*   **Why you need it:** This is where our HomeKey magic lives.
*   **Recommendation:** An **ESP32-C3** or a classic **ESP32** — these are the two targets this fork builds for, and both are tested on hardware. Other upstream targets (ESP32-S3, ESP32-C6) are **not built by this fork**.

> [!IMPORTANT]
> **This fork supports a narrower hardware set than upstream.** Ethernet is removed,
> and so are the PN7160/PN7161 and ST25R3916 readers. Choose your board and NFC
> module accordingly — see [Fork vs Upstream](fork-vs-upstream).

##### ESP32 Buyer's Guide

> [!NOTE]
> Both supported targets work. The **ESP32-C3** is smaller and cheaper; the classic
> **ESP32** has more flash headroom for the dual-slot OTA layout (7.80 % free vs
> 1.60 % on the C3).

Generally, any board should be fine. However, some may have non-genuine modules or just cheap flash chips with low endurance. There is no real way of telling which is the better clone. Genuine modules typically have "ESPRESSIF" etched on the metal casing.

> [!TIP]
> Genuine development boards can be ordered from major distributors like Mouser or Digikey, though it's pricey compared to something like AliExpress.

#### 2.1.2. NFC Reader Module — PN532

> [!IMPORTANT]
> **This fork supports the PN532 only.** Upstream also supports the **PN7160/PN7161**
> (SPI, with IRQ/VEN pins) and the **ST25R3916** (I2C). Those backends were removed
> here to free flash for the second OTA application slot. If you already own one of
> them, you need upstream firmware and the
> [upstream documentation](https://rednblkx.github.io/HomeKey-ESP32/prerequisites/).

*   **PN532:**
    *   **Interface:** SPI protocol.
    *   **Recommendation:** Ensure you have a PN532 module that supports SPI communication (for red Elechouse boards/clones, DIP switches must be set to `0` and `1`).
    *   Avoid long jumper wires between the module and ESP32 to maintain signal integrity.
    *   **Wiring differs per chip** — GPIO18/19/23/5 on a classic ESP32, GPIO4/5/6/7 on an ESP32-C3. See [Setup → PN532 Module Wiring](/HomeKey-ESP32/setup/#21-pn532-module-wiring).

##### Choosing Your PN532: A Mini Buyer's Guide

> [!NOTE]
> The information given here won’t guarantee that what you buy will be 100% without issues but aims to guide you toward a better part.

> [!TIP]
> Boards costing around 4-5€ or less are likely using non-genuine ICs.
> These boards will still work but expect worse performance.

When shopping for a PN532, check boards with blueish components instead of black ones. Those blueish components are Wire-wound RF inductors that provide superior antenna impedance matching, ensuring efficient power transfer to the antenna.

{{< cards cols="2" >}}
  {{< card title="SMD Multilayer RF Inductors" subtitle="You’ll mostly find boards with these two black components. They do the job, just not as much as you'd want them to, but they are cheap." image="/images/black_components.jpeg" tag="Meh" tagColor="red" tagIcon="exclamation" >}}
  {{< card title="Wire-wound RF inductors" subtitle="Check for those two blueish components, this is what should be used for impedance matching, which is essential for efficient power transfer to the antenna." image="/images/blue_components.jpeg" tag="Best" tagColor="green" tagIcon="check" >}}
{{< /cards >}}

You can also buy from Elechouse for best quality (original red board designer) on their [official website](https://www.elechouse.com/product/pn532-nfc-rfid-module-v4/).

### 2.2. Option B - Integrated PCB Boards

*   **What they are:** Custom-designed Printed Circuit Boards. These boards integrate the ESP32 and NFC module, along with other necessary components, into a single, compact solution.
*   **Why you need them:** Using an integrated PCB can significantly simplify wiring, reduce clutter, and result in a more robust and professional-looking final product.
*   **Where you can find one:**
    *   **@lollokara's PCB:** Features external NFC antenna, RGB LED, and 48V input (alongside USB-C). Available on [GitHub](https://github.com/lollokara/HomeKey-ESP32-PCB) or [PCBWay](https://www.pcbway.com/project/shareproject/ESP32_Homekey_77a119d7.html).
        *   There's two disconnected pads on the top left to the right of the USB-C that need to be soldered(pad bottom-left SEL1 and pad top-right SEL0) to select SPI mode but a manufacturer like PCBWay can handle this, however, they can sometimes misinterpret, so be prepared to put some solder.
    *   **CASmo-NFC:** Features an integrated NFC Antenna. Manufacturer is located in Germany. Can be ordered from their [website](https://casmo.info/en/shop/casmo-nfc-3).

> [!NOTE]
> The **CASmo-NFC-MB-ETH** board (upstream's Ethernet variant) is **not supported by
> this fork** — the Ethernet driver was removed. Its NFC side is a PN532, so the board
> itself still works, but its Ethernet port will be dead and you must select the
> `CASmo-NFC` preset rather than `CASmo-NFC-MB-ETH`.

> [!NOTE]
> The project and its owner are not affiliated with the aforementioned products nor with their designer/manufacturer or any relevant party, this section is only meant to list solutions and to praise community efforts.
