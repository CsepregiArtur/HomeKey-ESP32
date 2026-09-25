---
title: Security
weight: 9
---

HomeKey-ESP32 controls a door lock, so it is worth being explicit about what it protects, what it does not, and how to deploy it safely. This page is the reference for that: threat model, the hardening that ships with the firmware, and the decisions that are deliberately left to you.

> [!IMPORTANT]
> **The security model differs from upstream.** [rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32)
> deliberately ships with **flash encryption and Secure Boot disabled** so existing
> users never have to re-provision. **This fork implements flash encryption, Secure
> Boot V1 and NVS encryption, but also ships with them disabled by default** so the
> board stays fully reversible. Turning them on changes the physical-access threat
> model and makes a serial re-flash mandatory; it is a deferred, staged, one-way
> rollout described in
> [Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT). See also
> [Fork vs Upstream](fork-vs-upstream#4-security-model--the-biggest-difference).

## Threat model

| Attacker has... | Can they get in? | Notes |
| --- | --- | --- |
| Physical access to the device (USB/serial) | **Depends on the rollout.** | With the default (Path 1) configuration, flash is plaintext exactly like upstream, so secrets are readable. Once Path 2 is executed, the stored reader keys, HAP pairing keys, Wi-Fi credentials and NVS contents become ciphertext and only signed firmware boots. Physical access is a **denial-of-service** risk either way. See [Physical access](#physical-access). |
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

### Verify OTA images

OTA image signature verification is **enabled by default** in this fork, as part of Secure Boot V1 (see below). Every image the device accepts - whether flashed or uploaded over the network - must be signed with the Secure Boot signing key.

```ini
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V1_ENABLED=y
CONFIG_SECURE_BOOT_SIGNING_KEY="keys/secure_boot_signing_key.pem"
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
```

Generate the key **once** and never lose it - the same key has to sign every future update, and losing it means the device can no longer be updated:

```bash
# Secure Boot V1 on the original ESP32 uses an ECDSA-P256 key (not RSA).
espsecure.py generate_signing_key --version 1 keys/secure_boot_signing_key.pem
```

`keys/` and `*.pem` are gitignored; **never commit the private key.** Back it up
somewhere safe (password manager, encrypted volume) - it exists nowhere else.

### Which key signed a given build?

Every CI build logs a **public-key fingerprint**, so you can tell which key was
used without ever exposing the private one:

```bash
openssl pkey -in keys/secure_boot_signing_key.pem -pubout | openssl dgst -sha256
```

In the build log, look for one of exactly two mutually exclusive messages:

| Log message | Meaning |
| --- | --- |
| `SIGNED WITH YOUR KEY - using the provided keys/secure_boot_signing_key.pem` | The real key from the `SECURE_BOOT_SIGNING_KEY` secret was used. The build is flashable. |
| `THROWAWAY SIGNING KEY ... only boots on a device with unburned eFuses` | `SECURE_BOOT_SIGNING_KEY` is not set. **Do not publish this build.** |

The workflow prints the whole script before running it, so the *unused* branch's
text also appears in the log. Trust the `::notice`/`::warning` annotations (they
are prefixed and only one of them can be emitted at runtime) and the fingerprint.

## Flash encryption, Secure Boot and NVS encryption

This fork enables all three, unlike upstream:

| Protection | Setting | Effect |
| --- | --- | --- |
| Flash encryption | `CONFIG_SECURE_FLASH_ENC_ENABLED` | The whole flash (app, NVS, LittleFS) is AES-encrypted with a per-device key in eFuse BLK1, unreadable from software. |
| Encrypted NVS | `CONFIG_NVS_ENCRYPTION` + `CONFIG_SECURE_FLASH_ENC_USE_ENCRYPTED_NVS` | NVS keys live in the dedicated `nvs_keys` partition. Wi-Fi credentials, HomeKey reader material and all configuration are ciphertext. |
| Secure Boot V1 | `CONFIG_SECURE_BOOT` + `CONFIG_SECURE_BOOT_V1_ENABLED` | The bootloader and app must be signed with the key whose digest is burned into eFuse BLK2. Only signed firmware boots. |

> [!IMPORTANT]
> **All three of these are currently DISABLED in `sdkconfig.defaults`.** The
> settings below describe what enabling them does; the actual staged procedure
> lives in **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)**.

The original ESP32 only supports **Secure Boot V1**, which requires an **ECDSA-P256** key; RSA-based Secure Boot V2 is not available on this chip.

> [!CAUTION]
> **This is irreversible and destroys existing device data.**
>
> - The eFuses are **one-time programmable**. There is no way back.
> - On first boot after flashing, the ESP32 encrypts the flash **in place**. Any
>   Wi-Fi credentials, HomeKit pairing and HomeKey reader enrolment already on the
>   device **are lost and cannot be recovered** - the device must be fully
>   re-provisioned.
> - The partition layout changed (`nvs_keys` added, partition table moved to
>   `0xD000`, app partitions realigned to 64 KiB), so **an OTA update from an older
>   build is not possible**. A serial flash is mandatory.
> - After the eFuses are burned, the device only accepts firmware signed with your
>   signing key. If you lose that key you can no longer update the device.
>
> Back up your household recovery secret and configuration before flashing.

### Choosing how to enable it: Path 1 or Path 2

Enabling this is a **one-time decision per device**, and the mode you pick decides
how much freedom you keep. Pick deliberately — the eFuse burn cannot be undone.

> [!IMPORTANT]
> **This fork currently ships with both features DISABLED ("Path 1").** The board
> is fully reversible and no eFuse has been burned. Turning them on is **Path 2**,
> a deferred one-way rollout with its own staged procedure and prerequisites:
> see **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)**.
> Do not enable anything below until that document's prerequisites are met.

| # | Option | What it does | Reversible? |
| --- | --- | --- | --- |
| **1** | **Release mode**<br>`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y` | Burns the eFuses, encrypts the flash, and Secure Boot locks the device to your signing key. Encrypted + signed images only. | ❌ **Permanent** |
| **2** | **Development mode**<br>`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y` | Same first-boot eFuse burn and in-place encryption, but plaintext re-flashing stays possible (ESP-IDF warns and re-encrypts). Lets you validate the encrypt → sign → flash pipeline on real hardware. | ⚠️ Flash yes, **key no** |
| **3** | **Back out**<br>`CONFIG_SECURE_FLASH_ENC_ENABLED=n` | No eFuses burned, no encryption. The device behaves like upstream and plaintext flashing works normally — but you lose all the at-rest protection. | ✅ Yes — **this is the current state** |

```ini
# Option 1 — production images
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y

# Option 2 — validate the pipeline first (stage 1 of the Path 2 rollout)
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y

# Option 3 — back out entirely, behave like upstream (CURRENT)
# CONFIG_SECURE_FLASH_ENC_ENABLED is not set
```

> [!WARNING]
> **"Reversible" applies to the flashing workflow, not the eFuse.** Options 1 and 2
> both burn `FLASH_CRYPT_CNT` on first boot, and that cannot be undone. Development
> mode only means you can keep re-flashing **plaintext** images while developing,
> instead of needing a signed+encrypted image every time. Switching from
> development to release mode later does **not** re-enable plaintext flashing once
> the eFuse is spent — the setting only controls what the build and flasher allow.
>
> The flash-encryption key itself is **never** recoverable once burned. Back it up
> off-machine before Stage 1 of the rollout.
>
> Because of this, the rollout in
> **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)** proceeds in
> stages — flash encryption first, then NVS encryption, then Secure Boot, then
> release mode — verifying each before starting the next.

#### Flashing with encryption enabled: a common pitfall

`idf.py flash` writes a **plaintext** image. If the firmware was built with
`CONFIG_SECURE_FLASH_ENC_ENABLED=y` on a device whose `FLASH_CRYPT_CNT` is still
unset, the app aborts during startup:

```
E (682) flash_encrypt: Flash encryption eFuse bit was not enabled in bootloader
but CONFIG_SECURE_FLASH_ENC_ENABLED is on
abort() was called at PC ... esp_flash_encryption_init_checks
```

That is a **precondition check, not corruption** — the chip is fine and the eFuses
are untouched. Confirm with:

```bash
idf.py -p /dev/cu.usbserial-0001 efuse-summary
# FLASH_CRYPT_CNT = 0b0000000   -> nothing burned yet
# BLOCK1 (flash encryption key) -> empty
# ABS_DONE_0 = False           -> no Secure Boot
```

> [!WARNING]
> **`idf.py encrypted-flash` will NOT fix this on a chip with no key burned.** It
> fails with *"Flash encryption key is not programmed"* / *"Can't perform encrypted
> flash write"*, because it needs a key that does not exist yet.
>
> There is also **no `idf.py flash-encrypt` command** — that target does not exist.
> The real ones are `encrypted-flash`, `encrypted-app-flash`, `efuse-burn-key`,
> `efuse-burn`, `efuse-summary` and `secure-generate-flash-encryption-key`.

On a virgin chip, enabling encryption requires the eFuse to be burned first.
Follow **[Security Rollout Plan: Path 1 → Path 2](PATH2_SECURITY_ROLLOUT)** rather
than improvising — it walks through the key generation, the eFuse burn, and the
staged verification in the order ESP-IDF expects.

## Physical access

Flash encryption and Secure Boot protect the **secrets at rest** and the firmware integrity, but they do not make the device tamper-proof:

* An attacker with the device can still cause a **denial of service** (destroy it, glitch it, or trigger repeated reboots).
* Physical access plus a fault-injection/glitching lab is a much higher bar than serial reading, but is not something this project claims to defeat.
* Do not reuse the device's Wi-Fi password anywhere else, and rotate the MQTT password if a device is sold, given away or returned.
* Unpair the accessory in the Home app before handing the device on (this also erases the HomeKey reader data), or erase NVS.

Because the flash is now encrypted, an attacker can no longer simply read the reader keys or the HAP pairing keys over serial, and cannot flash a modified firmware image.


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

