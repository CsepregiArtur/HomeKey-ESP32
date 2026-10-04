---
title: "EthernetDriver (removed)"
weight: 2
---

## This module no longer exists in this fork

The `EthernetDriver` namespace and the entire Ethernet subsystem were **removed**
from this fork. This page is kept only so that links from older documentation and
search results land somewhere that explains the change instead of a 404.

> [!IMPORTANT]
> **This fork is Wi-Fi only.** There is no Ethernet bring-up, no Ethernet event
> handler, no Ethernet configuration in the Web UI, and no `ETH_APP_EVENT` on the
> application event bus. `HomeKitLock` no longer calls `initializeETH()`.

### Why it was removed

The driver cost roughly **100 KB of flash** — the largest single removable
component. That space was needed for the **second application slot** in the new
dual-slot OTA layout, which is what makes over-the-air firmware updates possible.
Ethernet was the least-used of the removable features: the overwhelmingly common
deployment is a Wi-Fi device near the door.

### Do you need it?

| If you… | Then… |
| --- | --- |
| Use Wi-Fi, or have never configured Ethernet | **Nothing to do.** This fork is what you want. |
| Rely on wired Ethernet for reliability | Use **upstream firmware**, which still has this module. See [Fork vs Upstream](/HomeKey-ESP32/fork-vs-upstream/). |

### What upstream's version did

For reference, upstream's `EthernetDriver` owned Ethernet bring-up and the Arduino
`ETH` event handler, extracted from `HomeKitLock` so that network initialization,
GPIO pin leasing and lifecycle handling lived in one place. It supported SPI modules
(W5500, DM9051, KSZ8851) on any target and RMII PHYs (LAN8720, TLK110, RTL8201,
DP83848, KSZ8041, KSZ8081) on chips with an internal Ethernet MAC, configured either
by a board preset (`ethActivePreset`) or custom pin arrays
(`ethSpiConfig` / `ethRmiiConfig`). The upstream documentation for it is at
<https://rednblkx.github.io/HomeKey-ESP32/api/ethernetdriver/>.

### See also

- [Fork vs Upstream](/HomeKey-ESP32/fork-vs-upstream/) — the full list of removals and why
- [Updates](/HomeKey-ESP32/updates/) — the dual-slot layout that the removal paid for
- [Configuration](/HomeKey-ESP32/configuration#522-nfc-reader-configuration) — the current hardware settings
