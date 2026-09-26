---
title: "Updates"
weight: 5
---

# Keeping Your HomeKey-ESP32 Fresh!

> [!IMPORTANT]
> **There is no over-the-air firmware update any more. Firmware is installed over serial only.**
>
> The device uses a single-slot flash layout (`no_ota.csv`): one `factory` application
> partition instead of two OTA slots, and no `otadata`. The freed space went to the
> application (3.3% free → 53% free) and to `nvs` (24 KiB → 92 KiB). Guidance below that
> describes updating over the network, from the Web UI or from GitHub no longer applies.
>
> A partition table cannot be delivered over the air - the node writes a new image into a
> slot that the *old* table describes - so moving an existing device onto this layout needs
> one serial flash. See **[Single-slot layout](single_slot_layout)**.

This document outlines different methods for updating the firmware on your HomeKey-ESP32 device. Keeping your device up-to-date ensures you have the latest features, bug fixes, and security enhancements.

> [!NOTE]
> If you are satisfied with your current setup, you probably don't need to update your firmware.
>
> However, if you're interested in what the new version brings, this guide is for you.

## Security hardening is available but not enabled

Version `0.10.0` (this fork) **implements** flash encryption, Secure Boot V1 and NVS
encryption, but ships with them **disabled** so the board stays fully reversible.
Nothing is destroyed on upgrade and no eFuses are burned.

Enabling the hardening is a **separate, deferred, one-way decision**. If and when you
take it, these consequences apply:

* **OTA is not possible from an older build.** The partition table moves to `0xD000`, an `nvs_keys` partition is added and the app partitions are realigned, so a network update will not boot. **A serial flash (`idf.py flash` / `esptool`) is required.**
* **Existing device data is erased.** Wi-Fi credentials, HomeKit pairing and HomeKey reader enrolment stored on the device are lost when the flash is first encrypted; the device must be re-provisioned from scratch.
* **Every future image must be signed.** Generate a Secure Boot signing key once and keep it safe - losing it means the device can no longer be updated:

  ```bash
  espsecure.py generate_signing_key --version 1 keys/secure_boot_signing_key.pem
  ```

* **Back up first.** Export the household recovery secret and note your configuration before upgrading.

Do not improvise this. Follow
**[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)**, which stages the
changes (flash encryption → NVS encryption → Secure Boot → release mode) and verifies
each one on hardware before the next.
* **Flash with the right path.** If the device is built with flash encryption enabled, `idf.py flash` writes a plaintext image and the app can boot-loop with `Flash encryption eFuse bit was not enabled in bootloader but CONFIG_SECURE_FLASH_ENC_ENABLED is on`. Once a key is actually burned, plaintext re-flashing and `idf.py encrypted-app-flash` are the supported routes in Development mode. See [Security](security) for the details.

### Pick a path before you flash

This fork currently ships with flash encryption and Secure Boot **disabled**
("Path 1") so that the board stays fully reversible while the firmware is
validated. Enabling them is **Path 2**, a deferred one-way rollout.

| Path | What it does | Reversible? |
| --- | --- | --- |
| **Path 1 — current** | No eFuses burned, no encryption. Behaves like upstream; plaintext flashing works normally. | ✅ Yes |
| **Path 2 — deferred** | Burns the eFuses, encrypts the flash, Secure Boot locks the device to your signing key. Encrypted + signed images only. | ❌ **Permanent** |

See **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)** for the
staged procedure, prerequisites and irreversible consequences, and
[Security](security#choosing-how-to-enable-it-path-1-or-path-2) for the config
options.

**Required Files for Updates:**

*   `*.firmware.bin`: The main application firmware file.
*   `littlefs.bin`: Contains the web interface files (LittleFS filesystem).

## Which version am I running?

Three places report it, and they agree:

* **Web UI → OTA** shows the firmware version as `Current Version`.
* **Web UI → device info** shows the firmware version and the UI (web interface) version.
* **Apple Home → accessory settings** shows the firmware revision.

How to read the value:

| Value | Meaning |
| --- | --- |
| `v0.10.0` | A tagged release. |
| `0.10.0-dev+1a2b3c4` | Built from a branch, `0.10.0` being the version it is based on and `1a2b3c4` the exact commit. |
| `0.10.0-dev+1a2b3c4-dirty` | Same, but the worktree had uncommitted changes - not a release. |

The UI version is reported separately as `<app version>+<commit>` (for example `0.10.0+1a2b3c4`), because the web interface can be updated on its own.

## 1. Over-The-Air (OTA) Updates

The primary method for Over-The-Air (OTA) updates is through the WebUI. This allows you to update your device wirelessly.

### 1.1. WebUI Updates

> [!NOTE]
> This is available starting with version `v0.7`.

The easiest way to update your device is through the web interface. Simply navigate to the device's IP address in your web browser and navigate to the "OTA Update" page.

1.  **Access the Web Interface:** Navigate to the device's IP address in your web browser.
2.  **Navigate to OTA Update section:** Click the "OTA Update" button on the left-hand side of the page.
3.  **Select Firmware File:** Select the `*-firmware.bin` file you downloaded earlier.
4.  **Select LittleFS File:** Select the `littlefs.bin` file you downloaded earlier.
5.  **Flash Firmware and LittleFS:** Click the "Upload Both" button to initiate the update process.
6.  **Reboot:** The device will automatically reboot after the OTA process is complete.

If everything went smoothly, you should see the "Current Version" and "Running Partition" fields update (once it reconnected) to reflect the new firmware version and partition.

### 1.2. `espota` Updates

### 1.2.1. Requirements

*   Your HomeKey-ESP32 device connected to your Wi-Fi network.
*   `espota` tool (available as a Windows executable or a Python script for Linux/macOS, both available [here](https://github.com/espressif/arduino-esp32/tree/master/tools)).
*   The `*-firmware.bin` file for your ESP32 chip (e.g., `esp32-firmware.bin`, `esp32c3-firmware.bin`, or `esp32s3-firmware.bin`) from the [GitHub Releases page](https://github.com/rednblkx/HomeKey-ESP32/releases).
*   The `littlefs.bin` file from the [GitHub Releases page](https://github.com/rednblkx/HomeKey-ESP32/releases).
*   The IP address of your HomeKey-ESP32 device.
*   (Optional) The OTA password, if you've set one in the [Configuration Guide](../configuration#524-homespan-settings).
    *   The shipped default (`homespan-ota`) is treated as "not configured": the `espota` service is **disabled** until you set your own password under `Misc → HomeSpan`. This is intentional, because that service accepts firmware uploads over the network and the default password is public. The boot log states this explicitly.
    *   If `espota` reports "No response from Device", check that a custom OTA password is set.

### 1.2.2. Update

1.  **Download `espota`:** Get the `espota` tool from the provided link.
2.  **Open Terminal/Command Prompt:** Navigate to the directory where you downloaded `espota` and your firmware files.
3.  **Flash Firmware:** Use the following command to flash the main firmware:
    *   **Windows:**
        ```bash
        espota.exe -r -i <address_of_device> -a <ota_password> -f <esp32xx-firmware.bin>
        ```
    *   **Linux/macOS:**
        ```bash
        python espota.py -r -i <address_of_device> -a <ota_password> -f <esp32xx-firmware.bin>
        ```
    *   Replace `<address_of_device>` with your device's IP address, `<ota_password>` with your OTA password (if set), and `<esp32xx-firmware.bin>` with the path to your `*-firmware.bin` file.
4.  **Flash LittleFS:** After the firmware is flashed, you must flash the `littlefs.bin` file using a similar command, but with the `-s` flag added:
    *   **Windows:**
        ```bash
        espota.exe -r -i <address_of_device> -a <ota_password> -f <littlefs.bin> -s
        ```
    *   **Linux/macOS:**
        ```bash
        python espota.py -r -i <address_of_device> -a <ota_password> -f <littlefs.bin> -s
        ```
5.  **Reboot:** The device will automatically reboot after the OTA process is complete.

## 2. Manual Update via USB (`esptool.py`)

If OTA updates aren't working, or if you prefer a wired connection, you can always update your device via USB using `esptool.py`. This method is similar to the initial flashing process.

### 2.1 Requirements

*   Your HomeKey-ESP32 device.
*   A USB cable to connect your ESP32 to your computer.
*   `esptool.py` installed on your computer (see [Prerequisites Guide](../prerequisites/#1-essential-software)).
*   The `esp32XX-firmware.bin` and `littlefs.bin` files.

### 2.2. Update

1.  **Connect ESP32:** Connect your ESP32 development board to your computer using a USB cable.
2.  **Identify Serial Port:** Find the serial port your ESP32 is connected to (refer to [Setup Guide](../setup#3-flash-the-firmware) for details).
3.  **Open Terminal/Command Prompt:** Navigate to the directory where you downloaded the `esptool.py` script and your firmware files.

4.  **Flash Firmware and LittleFS Separately (Advanced):**
    If you need to flash the application and filesystem separately (e.g., for specific development or recovery scenarios), use the following command. **Note the different flash addresses.**

    ```bash
    esptool.py --port YOUR_PORT write_flash 0x20000 <firmware.bin> 0x200000 <firmware.bin> 0x3e0000 <littlefs.bin>
    ```
    *   Replace `<firmware.bin>` and `<littlefs.bin>` with the paths to your respective files.
    *   Replace `YOUR_PORT` with your serial port assigned to your ESP32.

5.  **Initiate Flash Mode:** If the flashing doesn't start automatically, you might need to manually put your ESP32 into flash mode (refer to [Setup Guide](../setup#3-flash-the-firmware) for details).
6.  **Wait for Completion:** The flashing process will take a few moments. Once complete, you'll see a "Hash of data verified" message.
7.  **Reboot:** Disconnect and reconnect your ESP32 from USB to reboot the device.

## 3. Important Notes on Updates

*   **Check Release Notes:** Always check the release notes on the [GitHub Releases page](https://github.com/rednblkx/HomeKey-ESP32/releases) before updating. These notes will inform you about new features, bug fixes, and any potential breaking changes or special migration steps required between versions.
*   **Power Stability:** Ensure a stable power supply during the update process. Interrupting power during a flash can corrupt the firmware and require a full re-flash via USB.
*   **Web UI login:** devices set up from a version that generates its own credentials ask for a username and password on the Web UI. The credentials are shown when the setup portal saves the Wi-Fi configuration and again in the boot log; see [Security]({{< ref "security" >}}) for what to do if they are lost.
*   **Signed OTA images (optional):** releases can be built so the device only accepts signature-verified OTA images. That needs a signing key, so it is off by default - see [Security]({{< ref "security" >}}).

## 4. Breaking changes

These affect how an existing device is updated or accessed. None of them requires re-pairing, and none of them touches a device's stored configuration.

### 4.1. Update the firmware before the filesystem image

The web UI assets are brotli-compressed from this version on, because they no longer fit into the 128 kB filesystem partition as gzip. The firmware serves brotli, gzip or uncompressed assets - whichever the flashed image contains - so:

* **Firmware first, then the filesystem image**: works. This is the recommended order.
* **Filesystem image only, on older firmware**: the web UI will not load, because older firmware only looks for `.gz` assets. Recovery needs USB.
* **Firmware only, on an older filesystem image**: works; gzip assets are still served.

### 4.2. New devices ask you to choose their credentials

**What changed:** a device that has never been configured no longer generates credentials and prints them to the serial log. Instead it comes up with Web UI authentication **off** and shows a blocking **first-run setup** screen, where you choose your own Web UI username and password, HomeKit Setup Code, OTA password and setup AP password.

**Why:** the old values were printed **once**, during the hard reset that `idf.py flash` performs - before a monitor can attach - so they were easy to miss, and a missed setup AP password could only be recovered by dumping flash. Choosing the values removes that trap and means nothing has to be logged.

**What to do:** after the device joins your network, open its Web UI and complete the setup screen. Until you save it the Web UI has **no login**, so only do this on a trusted network.

**If a credential is lost:** see [Recovering from a lost credential]({{< ref "security" >}}) - the setup portal does not require a login, and erasing NVS returns the device to the first-run screen. Devices that are already configured are unaffected and keep their stored values.

While a new device has no network it may show two access points - its own `HK_XXXXXX` captive portal and HomeSpan's `HomeSpan-Setup`. Both use the setup AP password, which is `HomeKey$123$` until you set your own on the setup screen.

### 4.3. `espota` is off until an OTA password is set (security default change)

**What changed:** the `espota` service no longer starts while the OTA password is empty or still the shipped default (`homespan-ota`), because that password is public and the service accepts firmware uploads over the network.

**Who is affected:** anyone who updates over the network with `espota` (Arduino-IDE style) instead of through the Web UI. The Web UI firmware uploader is unaffected.

**Fix (one line of configuration):** `Web UI → Misc → HomeSpan → OTA Password` → set an OTA password → save. The device reboots, and `espota` accepts that password from then on.

### 4.4. State-changing endpoints are POST-only

`/reset_hk_pair`, `/reset_wifi_cred` and `/start_config_ap` reject GET requests now. The Web UI sends POST; scripts that used GET need updating.

### 4.5. Requests must use the device's IP address or its mDNS name

The `Host` header is validated to block DNS rebinding, so reaching the Web UI through a custom host name, a reverse proxy or a hostname alias returns 401. Use the device's IP address or `<hostname>.local`.

---

Keeping your HomeKey-ESP32 updated is key to a smooth and secure smart home experience. Happy updating!
