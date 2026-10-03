---
title: "Troubleshooting"
weight: 17
---

## Locked out of the Web UI

The Web UI password is required once `webAuthEnabled` is on, which first-run setup switches on. Because the setup portal does not require that login, you can always get back in:

1. Reset the Wi-Fi credentials (`Web UI → Misc` if another browser session is still authenticated, otherwise over serial) so the device falls back to its setup access point.
2. Join the AP, open `http://192.168.4.1` and set a new Web UI password on the setup page.

If you never changed the setup AP password from the shipped value, it is `HomeKey$123$`. If you do not know it, erase NVS over USB (`pio run -t erase` or `esptool.py erase_flash`) - the device returns to a factory-fresh state and shows the first-run setup screen again - then re-pair it. Erasing NVS also removes Wi-Fi credentials, the HomeKit pairing and every HomeKey enrolment.

## Over-the-air uploads are refused

The update endpoints require **HTTPS** and Web UI authentication. If the Update page
refuses immediately, check that you opened the device over `https://`, and that you
are logged in. A plain-HTTP upload is rejected on purpose: an image is the most
valuable thing a caller can send. See [Updates](updates#1-from-the-web-ui).

If the upload starts and then fails, the image probably does not fit — the page shows
the slot size, and the C3 build has only ~30 KB of headroom. Use a smaller image, or
switch to the single-slot `no_ota.csv` layout. Flashing over the cable always works:
`./scripts/ota_update.py --port <port>`.

## The update succeeded but the device is still on the old version

This is **rollback**, working as designed. A newly installed image is marked *pending
verify*; if it does not confirm itself (see `setup()` in `main/main.cpp`), the
bootloader abandons it for the previous slot on the next reset. Check the boot log's
`Running partition` line to see which slot is live. A device that boots a good image
but resets later - in `loop()`, for example - is also rolled back. See
[Updates → Safety: rollback](updates#safety-rollback).

## Requests are rejected with 401 even though the password is correct

The Web UI rejects requests whose `Host` header does not name the device (this blocks DNS rebinding). Use the device's IP address or its `.local` name. Access through a reverse proxy or a custom domain that rewrites the `Host` header will be rejected by design.

## `espota.py` - "No response from Device" or "No response from the ESP"

`espota.py` is **not** the update mechanism in this fork, and the `espota`/ArduinoOTA service is never enabled. Updates go over the LAN through the Web UI's Update page, or over the cable:

```bash
./scripts/ota_update.py            # detects the chip, then asks how to update
./scripts/ota_update.py --port /dev/cu.usbserial-XXXX
```

See [Updates](updates).

## HomeKey not working on Apple Watch

Make sure HomeKey is present in the Watch's wallet app. If it is not, wait until you get a notification that HomeKey is available.

If you don't get the notification, try to restart the Watch.

If the accessory was previously unpaired in Home and the Card did not get removed after unpairing, it will not work, you have to wait until the Watch syncs with iCloud and updates the Wallet. If that doesn't happen, try to restart the Watch.

## Sharing HomeKey

HomeKit accessories are synced with iCloud which enables your other devices tied to your account to pair with the accessory and also configure HomeKey if compatible.

To share HomeKey with someone else, you need to invite them to your Apple Home, see https://support.apple.com/en-us/102386 for more information.
