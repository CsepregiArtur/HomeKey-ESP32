---
title: Security
weight: 9
---

HomeKey-ESP32 controls a door lock, so it is worth being explicit about what it protects, what it does not, and how to deploy it safely. This page is the reference for that: threat model, the hardening that ships with the firmware, and the decisions that are deliberately left to you.

## Threat model

| Attacker has... | Can they get in? | Notes |
| --- | --- | --- |
| Physical access to the device (USB/serial) | **Yes** | The flash is readable and writable over the serial bootloader, so the HomeKey reader keys, the HAP pairing keys and the Wi-Fi credentials can be extracted, and arbitrary firmware can be flashed. See [Physical access](#physical-access-is-a-total-compromise). |
| A device on the same network (LAN/Wi-Fi/guest VLAN) | **Depends on your settings** | With Web UI authentication enabled, reading/writing the configuration requires the Web UI password. Without it, everything below is open. |
| Access to your MQTT broker | **Potentially, yes** | Anyone who can publish to the lock's command topics can unlock the door unless the broker enforces authentication and per-device ACLs. See [MQTT](#mqtt-is-an-unlock-path-treat-it-like-one). |
| A browser on the same network (malicious web page) | **No** | Requests are rejected unless the `Host` header names the device (DNS rebinding / CSRF protection), and all state-changing endpoints are POST-only. |
| Internet access to the device (port forwarding) | **No, because you should not do it** | Do not expose the Web UI, MQTT broker or device to the internet. If you need remote access, use a VPN. |
| Radio range only (no credentials) | **No** | The setup AP uses WPA2/WPA3 and a per-device password. NFC/HomeKey requires a provisioned key in the Secure Enclave of an authorised device. |

## What this firmware does by default

These protections are active without any configuration:

* **Per-device generated credentials on first boot.** The values compiled into `main/include/defaults.h` (Setup Code, setup AP password, OTA password, Web UI password) are published in this repository, so a factory-fresh device replaces them with random ones. They are printed **once, on first boot, to the serial log at 115200 baud** (`pio device monitor`, or any serial terminal) - that log is the only place they appear:

  ```
  HomeKit Setup Code : 123-45-678
  Setup AP password  : <16 random characters>
  Web UI login       : admin / <16 random characters>
  OTA password       : <20 random characters>
  ```

  (shown without the `W (nnnn) Security:` log prefix). Open the serial console before powering a new device on, or read the Web UI login again from the setup portal after it saves. The values are stored in NVS and never regenerated afterwards, so nothing changes for a device that is already set up; clearing the configuration (`Web UI → Misc → Clear`, or erasing NVS) makes the next boot generate a fresh set. If they are lost: [Recovering from a lost credential](#recovering-from-a-lost-credential).

* **Web UI authentication is on for new devices.** `webAuthEnabled` defaults to *on* with a generated password. Devices that were configured before this change keep whatever they have stored - check `Misc → Security` in the Web UI and turn it on if it is off.

* **Secrets are never sent to the browser.** Configuration reads return `********` for every `*Password`/`*Passwd` field, and the write path refuses to store that placeholder, so a stale form cannot overwrite a real password with the mask.

* **The HomeSpan OTA service stays off until you set an OTA password.** Its only protection is that password, and the shipped value (`homespan-ota`) is public, so the service is skipped while the password is empty or unchanged. The fix is one line of configuration: set any password under `Web UI → Misc → HomeSpan → OTA Password` and save - the device reboots and `espota` accepts that password. The Web UI OTA uploader (`/ota/*`) is unaffected.

* **Neither setup access point uses a published password.** Two APs can appear while a device has no network: the project's own `HK_XXXXXX` captive portal and HomeSpan's `HomeSpan-Setup`. The project AP uses the per-device password generated on first boot, and HomeSpan's is aligned with it at every boot (`homeSpan.setApPassword()`), so the values printed in this repository (`HomeKey$123$`, `homespan`) do not open either one on a device set up from this version on.

* **The setup AP is temporary by nature.** It is only started when the device has no working network connection, it accepts at most two clients, it uses WPA2/WPA3 with a per-device password, and it is restarted after 10 minutes with nobody connected (`AP_IDLE_CYCLE_MIN` in `defaults.h`).

* **The setup portal works without a Web UI login.** The portal is reached before the device has any network, and it is already gated by the AP password. The Web UI password is reported on the portal's success screen (and the serial log) so you can log in afterwards. Use that to recover a lost password: [Locked out of the Web UI](#locked-out-of-the-web-ui).

* **State changes are POST-only and `Host`-validated.** A malicious page you visit cannot reset the pairing, force setup mode or read responses via DNS rebinding.

* **Repeated failed logins are slowed down** (2 s delay after five failures until the next success or reboot). There is no permanent lockout to get stuck in.

* **MQTT without TLS logs a warning on every connection**, because those topics can drive the lock.

* **No Bluetooth stack is compiled in.** HomeKit pairing is Wi-Fi/SRP only, which removes the entire BLE attack surface.

## What you should enable yourself

### Web UI authentication

The Web UI can read the configuration, reset the HomeKit pairing, force setup mode, and upload firmware. Enable authentication under `Misc → Security` unless the device is on a network you fully trust.

HTTPS is available too (`Misc → Security → HTTPS`) by uploading your own certificate and key. Basic authentication alone sends the password base64-encoded, which is only safe on a network where nobody can observe traffic, so combine the two if the lock shares a network with untrusted devices.

### MQTT is an unlock path - treat it like one

The lock subscribes to command topics such as `homekit/set_target_state` and `homekit/set_custom_state`, and it publishes HomeKey taps. Consequences:

* Use TLS with certificate validation (`MQTT SSL` + CA certificate, and leave "allow insecure" off). Without it, credentials are sniffable and commands are forgeable by anything on the path.
* Create a dedicated broker user per device and an ACL that limits it to its own topic namespace:
  ```
  # mosquitto acl_file example
  user homekey-lock
  topic read  homekit/state
  topic read  status
  topic write homekit/set_target_state
  topic write homekit/set_custom_state
  ```
  A broker user that can `write homekit/#` on every device's topics can open every door.
* Never expose the broker to the internet, and never reuse the device's Wi-Fi password as the MQTT password.

### Network placement

Put the lock on an IoT SSID or VLAN that cannot reach your computers, and block client-to-client traffic if your AP supports it. The device speaks plain HTTP on the local network by default, so a segmented network is what keeps that acceptable.

### Verify OTA images (optional, recommended for releases)

ESP-IDF can require a signature on every OTA image *without* secure boot, which needs no eFuses and no re-provisioning. It protects against a spoofed or tampered firmware upload reaching the device over the network. Signed builds need a key, so it is not enabled in the default `sdkconfig.defaults` - add the following to your `sdkconfig.defaults` (see the commented block at the end of that file):

```ini
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_SIGNING_KEY="/absolute/path/to/my_signing_key.pem"
```

Generate the key once and never lose it - the same key has to sign every future update:

```bash
espsecure.py generate_signing_key --version 2 my_signing_key.pem
```

Keep `CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` disabled (the default). Verifying on update is where the value is; verifying on boot without hardware secure boot mostly adds a way to brick a device that was just flashed over USB.

Note the second-order effect: once the feature is on, *every* OTA image the device accepts must be signed, including community builds. If you publish unsigned binaries, do not enable this.

## Physical access is a total compromise

This firmware does **not** enable flash encryption or secure boot, and that is a deliberate project decision:

* Both are one-way eFuse burns that require erasing the whole flash first. Every deployed device would lose its Wi-Fi credentials, its HomeKit pairing and its enrolled keys, i.e. every user would have to reconfigure from scratch.
* An encrypted device also cannot be reflashed over USB without the key, which does not fit a project where users build and flash their own hardware.
* NVS encryption would additionally need an `nvs_keys` partition; the partition table has no spare space.

So assume that anyone who can hold the device for a minute can read its secrets and replace its firmware. Practical consequences:

* Do not reuse the device's Wi-Fi password anywhere else.
* Rotate the MQTT password if a device is sold, given away or returned.
* Unpair the accessory in the Home app before handing the device on (this also erases the HomeKey reader data), or erase NVS.
* Physically secure the device so that its USB port is not reachable without opening the enclosure.

If you are building hardware for others, the alternative is a factory image with flash encryption and Secure Boot v2 enabled from the first boot - new devices only, keeping the current scheme for existing ones.

## Recovering from a lost credential

### Locked out of the Web UI

The setup portal does not require the Web UI login, so:

1. `Web UI → Misc → Reset Wi-Fi credentials` (or erase NVS over serial) to make the device fall back to its setup AP.
2. Join the AP with the password printed at first boot (or the last one you set). If you no longer know it, erase NVS - see below.
3. Open the portal and either set a new Web UI password there, or read the current one from the success message.

If all of that fails, erase NVS over USB and start fresh - the device then generates new credentials and reports them on the serial console:

```bash
pio run -t erase   # or: esptool.py erase_flash
pio run -t upload
```

Erasing NVS removes the Wi-Fi credentials, the HomeKit pairing and the HomeKey data, so only do it when you have physical access and can re-pair.

### HomeSpan `espota` stopped working

**What happened:** the `espota` service no longer starts while the OTA password is empty or still the shipped default (`homespan-ota`). That password is published in this repository and the service accepts firmware uploads over the network, so leaving it enabled was a way in.

**Who is affected:** anyone who updates over the network with `espota` (Arduino-IDE style) rather than through the Web UI.

**Fix (one line of configuration):** `Web UI → Misc → HomeSpan → OTA Password` → enter any password → save. The device reboots and `espota` works with that password. The Web UI firmware uploader is unaffected.

### The Web UI shows nothing after an update

The assets in the filesystem partition and the firmware that serves them are updated
separately, and older firmware only understands gzip assets while the current image ships
brotli. If the filesystem image was flashed without updating the firmware first, the Web
UI returns 404 for everything. The device itself is fine - flash the current firmware over
USB (`pio run -t upload`, or `esptool.py` with the release binaries) and the UI comes back.
See [Breaking changes]({{< ref "updates" >}}) for the update order.

## Reporting a vulnerability

Please open a private report (GitHub Security Advisory) rather than a public issue, and include the firmware version, the affected endpoint or component, and a reproduction. Fixes for anything in the [threat model](#threat-model) that can be reached over the network are handled as security releases.

## Household cryptography (multi-node)

The Household extension uses only established primitives from the already-linked
libsodium (`espressif/libsodium`) and mbedTLS. No custom algorithms are used.

| Purpose | Construction |
|---|---|
| Node identity keypair | Ed25519 (`crypto_sign_keypair`); private key stored in NVS, never serialized/exported |
| Node certificate fingerprint | SHA-256(`node_id || public_key`) — placeholder until a household CA exists |
| Backup key derivation | `BLAKE2b-256("HK-HOUSEHOLD-BACKUP-v1", recovery_secret || salt)` (`crypto_generichash`, keyed) |
| Backup encryption | XChaCha20-Poly1305-IETF AEAD (`crypto_aead_xchacha20poly1305_ietf_*`); 24-byte random nonce; header bound as additional data |
| Backup authentication | Ed25519 detached signature over `header || ciphertext` (`crypto_sign_verify_detached`) |
| Provisioning token at rest | SHA-256 of the code only (never the code); comparison via `sodium_memcmp` |
| MQTT command authentication | HMAC-SHA256 (`crypto_auth_hmacsha256`) over `{ts}{nonce}{req_id}{action}`; key = `BLAKE2b-256("HK-HOUSEHOLD-CMD-v1", recovery_secret || salt)` |
| Constant-time comparisons | `sodium_memcmp` for provisioning hash and command MAC |

Key hygiene:

- Nonces for every backup are 24 random bytes (`randombytes_buf`), so reuse is
  cryptographically negligible.
- The recovery secret, node private key and derived keys are zeroized with
  `sodium_memzero` on destruction.
- Secret material is never written to the Web UI, MQTT telemetry or logs; the
  recovery secret is exportable exactly once.

Known limitations (kept explicit, not hidden):

- The node "certificate" is currently a fingerprint, not a CA-signed leaf; there
  is no household CA yet.
- MQTT commands rely on a shared household command key derived from the recovery
  secret rather than per-node signed commands.
- Flash encryption and secure boot remain off by design (see
  [Physical access](#physical-access-is-a-total-compromise)).

