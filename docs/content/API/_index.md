---
title: "API Documentation"
weight: 100
---

This section provides an overview of the public APIs implemented in the HomeKey-ESP32 project. It details the classes responsible for managing various aspects of the system, their functionalities, and how to interact with them.

## Key Subsystems

*   **[AppEventLoop](../api/appeventloop/):** Decoupled event bus wrapper around ESP-IDF native `esp_event`.
*   **[ConfigManager](../api/configmanager/):** JSON-based NVS/SPIFFS configuration persistence and schema validation.
*   **[HardwareManager](../api/hardwaremanager/):** Hardware abstraction layer with `GPIOAllocator` thread-safe pin leasing and strapping pin protection.
*   **[HomeKitLock](../api/homekitlock/):** HomeSpan HomeKit accessory implementation.
*   **[LockManager](../api/lockmanager/):** Lock state machine managing target vs current states.
*   **[MqttManager](../api/mqttmanager/):** Async MQTT client, TLS management, and HASS Auto-Discovery.
*   **[NfcManager](../api/nfcmanager/):** PN532 NFC driver (SPI), ECP frame broadcasting, and DigitalDoorKey integration.
*   **[ReaderDataManager](../api/readerdatamanager/):** Storage for Apple HomeKey reader keys and issuer endpoint data.
*   **[WebServerManager](../api/webservermanager/):** Async HTTP/HTTPS web server, Svelte 5 WebUI with `sv-router`, WebSockets, certificate management, and the OTA endpoints.

> [!NOTE]
> **Two upstream modules do not exist in this fork:**
> * **`EthernetDriver`** — removed; this fork is Wi-Fi only, so the whole Ethernet
>   subsystem (SPI modules and RMII PHYs) is gone.
> * **The PN7160/PN7161 and ST25R3916 reader backends** — removed; `NfcManager` drives
>   the **PN532 only**.
>
> See [Fork vs Upstream](../fork-vs-upstream) for why.

## Event System (AppEventLoop)
The project uses the `AppEventLoop` system for internal communication between components. This is a modern C++ wrapper around ESP-IDF's native event loop. See [AppEventLoop](../api/appeventloop/) for details.

---
