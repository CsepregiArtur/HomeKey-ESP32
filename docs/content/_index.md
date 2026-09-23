---
cascade:
  type: "docs"
---

# HomeKey-ESP32 Documentation 📚

{{< callout type="warning" >}}
This wiki is **versioned** — the version selector in the top bar lets you switch between released versions and the development docs.

You are currently viewing the documentation for the **bleeding edge** (`main` branch), which may describe features and behavior that are not yet available in the latest release. If you are running a released firmware version, pick that version from the selector to see the docs matching your installation.
{{< /callout >}}

{{< callout type="important" >}}
**This is a fork of [rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32)** (MIT). The upstream project is the original work and remains the reference for core HomeKey/HomeKit/NFC behaviour.

**What this fork changes:**

| Area | Original project | This fork |
| --- | --- | --- |
| Scope | Single device | **Household** of multiple nodes |
| Backup | — | **Encrypted + signed** household backup |
| MQTT | Legacy topics | **+ household namespace** and HA discovery |
| MQTT lock/unlock | Plain numeric payloads | **HMAC-SHA256 authenticated** |
| Web UI | Misc, MQTT, OTA, Logs | **+ household, node, health, security, audit, backup, recovery, provision** |
| **Flash encryption** | **Disabled** | **Enabled** |
| **Secure Boot** | **Disabled** | **Enabled** (V1, ECDSA-P256) |
| **NVS encryption** | **Disabled** | **Enabled** |

**Unchanged:** the HomeKey/NFC protocol, lock logic, the HomeKit accessory model, the existing Web UI pages and all existing MQTT topics.

> Flash encryption is **irreversible** and requires a serial re-flash plus full re-provisioning of existing devices. Read **[Fork vs Upstream](fork-vs-upstream)** for the full comparison, and [Security](security) / [Updates](updates) before upgrading.
{{< /callout >}}

Welcome to the HomeKey-ESP32 documentation! This is your one-stop shop for everything you need to know about setting up, configuring, and using your HomeKey-ESP32 device. Whether you're a seasoned ESP32 developer or just starting your smart home journey, we've got you covered.

Use the navigation on the left (or use the top-right menu if you're on a mobile device) to explore the different sections.

## This fork

* **[Fork vs Upstream](fork-vs-upstream):** The complete, side-by-side list of what differs from the original project — household architecture, MQTT contract, and the security (flash encryption / Secure Boot / NVS encryption) differences. **Start here if you are coming from upstream.**

## Getting Started

* **[Prerequisites](prerequisites):** Before you embark on this exciting adventure, make sure you have all the necessary tools and software installed. Think of this as packing your bags before a grand journey!
* **[Setup](setup):** Ready to bring your HomeKey-ESP32 to life? This guide will walk you through the initial setup, from wiring your hardware to flashing the firmware. It's like giving your ESP32 its first breath!

## Configuration & Customization

* **[Configuration](configuration):** Every smart home is unique, and so should be your HomeKey-ESP32. Learn how to customize its settings to perfectly fit your needs, whether it's Wi-Fi credentials, MQTT broker details, or HomeKit parameters. This is where you make it truly *yours*.
* **[MQTT Integration](mqtt):** Want your HomeKey-ESP32 to chat with your smart home hub? This section details how to integrate your device with MQTT, allowing for seamless communication and automation. Get ready for some serious smart home synergy!

## Maintenance & Troubleshooting

* **[Updates](updates):** Keep your HomeKey-ESP32 running smoothly with the latest features and bug fixes. This guide explains how to update your device's firmware, including the magic of Over-The-Air (OTA) updates. Stay fresh, stay secure!
* **[Troubleshooting](troubleshooting):** If you encounter any issues or have questions that aren't covered in this documentation, don't hesitate to reach out! You can open an [issue](https://github.com/rednblkx/HomeKey-ESP32/issues) or join the Discord server [here](https://discord.com/invite/VWpZ5YyUcm).

## API Documentation

* **[API Reference](api):** Dive into the codebase and explore the various classes and functions that make up the HomeKey-ESP32 project. Learn how to use them to customize your device's behavior and functionality.

## Need Help?

If you encounter any issues or have questions that aren't covered in this documentation, don't hesitate to reach out! You can open an [issue](https://github.com/rednblkx/HomeKey-ESP32/issues) or join the Discord server [here](https://discord.com/invite/VWpZ5YyUcm).
