# Changelog

Notable changes per release. User-facing detail lives in the docs:
[Security](docs/content/security.md) and [Updates / breaking changes](docs/content/updates.md).

## 0.10.0 - 2026-09-22

Adds the **Household** multi-node architecture: several ESP32 nodes share trust and
recovery material while each node keeps its own device-specific identity.

### Household & multi-node

* **Household / node model** — a device can be enrolled into a household and act as a
  node (`gate`, `main`, …) with its own Ed25519 node identity that is never cloned to
  another device.
* **Encrypted, signed backups** — household and node configuration are exported as a
  versioned, XChaCha20-Poly1305-encrypted and Ed25519-signed blob; no raw NVS dumps.
  Device-specific cryptographic identity (HomeKey reader private key, endpoint
  persistent keys) is excluded from backups.
* **Provisioning** — single-use, expiring, replay-protected join codes (stored only as a
  SHA-256 hash).
* **Recovery** — a one-time-export household recovery secret decrypts backups so a
  replacement node can be enrolled without Home Assistant.
* **MQTT household namespace** — structured telemetry under
  `homekey/household/<household_id>/nodes/<node_id>/…`; lock/unlock commands are
  HMAC-authenticated and replay-protected, and `unlock=true` is never accepted as
  authorization.
* **Home Assistant MQTT discovery** — node online/health entities with stable ids
  (`<household_id>_<node_id>_<entity>`).
* **New Web UI pages** — `/household`, `/node`, `/health`, `/security`, `/audit`,
  `/backup`, `/recovery`, `/provision`.

### Breaking

* Replacing a node's physical HomeKey reader issues a new reader identity, so existing
  HomeKey credentials must be re-provisioned in the Apple Home app — this cannot be
  automated.

## 0.9.0 - 2026-09-17

Security-focused release: it changes some defaults, so read the *Breaking* section below
before updating. To check which build a device runs, see
[Which version am I running?](docs/content/updates.md#which-version-am-i-running) -
a tagged release reports `v0.9.0`, a build from a branch reports
`0.9.0-dev+<commit>[-dirty]`, and the web interface reports `<version>+<commit>`.

### Security

* **New devices generate their own credentials.** A device with no stored configuration
  replaces the Setup Code, setup AP password, OTA password and Web UI password with random
  per-device values, and turns Web UI authentication on. Existing devices keep their stored
  configuration unchanged - nothing has to be re-paired or reconfigured.
* **Both setup access points reject the published passwords.** The project's `HK_XXXXXX` AP
  and HomeSpan's `HomeSpan-Setup` AP both use the per-device password, so the values printed
  in the source tree (`HomeKey$123$`, `homespan`) no longer open either one.
* **`espota` no longer starts while the OTA password is empty or still the shipped default**
  (`homespan-ota`), because that password is published in this repository and the service
  accepts firmware uploads over the network.
* **Web UI hardening:** `Host` header validation (blocks DNS rebinding), POST-only state
  changes (blocks cross-site requests), constant-time credential comparison with a delay
  after repeated failures, and secret fields that can no longer be written back from a
  masked form.
* **MQTT** logs a warning on every connection without TLS, because its command topics can
  unlock the door.
* **Optional OTA image signature verification** is documented and can be enabled from
  `sdkconfig.defaults` - no eFuses and no re-provisioning needed.

### Changed

* The setup AP restarts itself after 10 minutes with no client connected
  (`AP_IDLE_CYCLE_MIN`), so it does not stay on the air indefinitely.
* Web UI assets are **brotli-compressed** (91 kB instead of 110 kB gzipped): the gzip
  payload had 0.1 kB of headroom left in the 128 kB filesystem partition. The firmware
  serves brotli, gzip or uncompressed files, whichever the flashed image contains.

### Breaking

Full list with fixes: [Breaking changes](docs/content/updates.md#4-breaking-changes).

* **Update the firmware before the filesystem image.** Older firmware only looks for `.gz`
  assets, so flashing the new filesystem image on its own leaves the Web UI unable to load
  until the firmware is flashed over USB.
* **New devices ask for a Web UI login.** The credentials are printed once to the
  **first-boot serial log** (115200 baud). If they are lost, the setup portal does not
  require a login and erasing NVS generates a new set - see
  [Recovering from a lost credential](docs/content/security.md#recovering-from-a-lost-credential).
* **`espota` needs a custom OTA password** (security default change). Impact: anyone who
  updates over the network with `espota` has to set a password once; the Web UI firmware
  uploader is unaffected. One-line fix: `Web UI → Misc → HomeSpan → OTA Password` → set a
  password → save (the device reboots).
* `/reset_hk_pair`, `/reset_wifi_cred` and `/start_config_ap` are **POST-only** now; scripts
  that used GET need updating.
* Requests must use the device's **IP address or mDNS name**; a custom host name, reverse
  proxy or alias now gets a 401.
