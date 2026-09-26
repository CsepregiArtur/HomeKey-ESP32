---
title: "Troubleshooting"
weight: 7
---

## Locked out of the Web UI

The Web UI password is required once `webAuthEnabled` is on, which first-run setup switches on. Because the setup portal does not require that login, you can always get back in:

1. Reset the Wi-Fi credentials (`Web UI → Misc` if another browser session is still authenticated, otherwise over serial) so the device falls back to its setup access point.
2. Join the AP, open `http://192.168.4.1` and set a new Web UI password on the setup page.

If you never changed the setup AP password from the shipped value, it is `HomeKey$123$`. If you do not know it, erase NVS over USB (`pio run -t erase` or `esptool.py erase_flash`) - the device returns to a factory-fresh state and shows the first-run setup screen again - then re-pair it. Erasing NVS also removes Wi-Fi credentials, the HomeKit pairing and every HomeKey enrolment.

## Over-the-air uploads are refused

There is no OTA path. The single-slot layout has no second application slot and no `otadata`, so `espota`, the Web UI uploader and the GitHub updater are all gone. Install firmware over serial (`idf.py -p <port> flash` or `pio run -t upload`). See [Single-slot layout](single_slot_layout).

## Requests are rejected with 401 even though the password is correct

The Web UI rejects requests whose `Host` header does not name the device (this blocks DNS rebinding). Use the device's IP address or its `.local` name. Access through a reverse proxy or a custom domain that rewrites the `Host` header will be rejected by design.

## `espota.py` - "No response from Device" or "No response from the ESP"

`espota.py` is not applicable to this firmware: it uploads over the air, and there is no OTA path on the single-slot layout. Use a serial flash. If you are reading this while debugging an older build, the usual cause is that the PC and the ESP32 cannot reach each other on the invitation port (3232) or the listening port (10000-60000, `-P`).

## HomeKey not working on Apple Watch

Make sure HomeKey is present in the Watch's wallet app. If it is not, wait until you get a notification that HomeKey is available.

If you don't get the notification, try to restart the Watch.

If the accessory was previously unpaired in Home and the Card did not get removed after unpairing, it will not work, you have to wait until the Watch syncs with iCloud and updates the Wallet. If that doesn't happen, try to restart the Watch.

## Sharing HomeKey

HomeKit accessories are synced with iCloud which enables your other devices tied to your account to pair with the accessory and also configure HomeKey if compatible.

To share HomeKey with someone else, you need to invite them to your Apple Home, see https://support.apple.com/en-us/102386 for more information.
