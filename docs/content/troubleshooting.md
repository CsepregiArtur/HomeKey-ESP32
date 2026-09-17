---
title: "Troubleshooting"
weight: 7
---

## Locked out of the Web UI

The Web UI password is required as soon as `webAuthEnabled` is on (the default for devices that were set up from a version with generated credentials). Because the setup portal does not require that login, you can always get back in:

1. Reset the Wi-Fi credentials (`Web UI → Misc` if another browser session is still authenticated, otherwise over serial) so the device falls back to its setup access point.
2. Join the AP with the password printed at first boot, open `http://192.168.4.1`, and set a new Web UI password on the setup page.

The password is also printed on the serial console at first boot, and reported on the setup page when the Wi-Fi configuration is saved. If none of that is available, erase NVS over USB (`pio run -t erase` or `esptool.py erase_flash`) - the device then generates fresh credentials and prints them - and re-pair it.

## HomeSpan `espota` uploads are rejected or time out

The `espota` service is disabled while the OTA password is unset or still the shipped default (`homespan-ota`). Set your own password under `Misc → HomeSpan` and reboot; the boot log reports whether the service was started. The Web UI OTA uploader works regardless, and is protected by the Web UI authentication setting.

## Requests are rejected with 401 even though the password is correct

The Web UI rejects requests whose `Host` header does not name the device (this blocks DNS rebinding). Use the device's IP address or its `.local` name. Access through a reverse proxy or a custom domain that rewrites the `Host` header will be rejected by design.

## `espota.py` - "No response from Device" or "No response from the ESP"

`espota.py` starts by listening on a random port between 10000-60000(definable through option `-P`) and then sends an invitation for connection to the ESP32 on the port 3232.
This means that not only your PC has to be able to reach the ESP32 but it also has to work the other way around.

Make sure your network configuration allows for a connection between the ESP32 and the PC to be established. Host IP and Port can be defined using the `-I` and `-P` options respectively.

## HomeKey not working on Apple Watch

Make sure HomeKey is present in the Watch's wallet app. If it is not, wait until you get a notification that HomeKey is available.

If you don't get the notification, try to restart the Watch.

If the accessory was previously unpaired in Home and the Card did not get removed after unpairing, it will not work, you have to wait until the Watch syncs with iCloud and updates the Wallet. If that doesn't happen, try to restart the Watch.

## Sharing HomeKey

HomeKit accessories are synced with iCloud which enables your other devices tied to your account to pair with the accessory and also configure HomeKey if compatible.

To share HomeKey with someone else, you need to invite them to your Apple Home, see https://support.apple.com/en-us/102386 for more information.
