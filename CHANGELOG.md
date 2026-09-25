# Changelog

Notable changes per release. User-facing detail lives in the docs:
[Security](docs/content/security.md) and [Updates / breaking changes](docs/content/updates.md).

## Unreleased

### Added

* **Update from GitHub.** The OTA page can now download and install a published release
  directly: pick **Production** or **Development**, check what the channel points at, then
  install. The device performs the GitHub API call and the download itself (TLS verified
  against the bundled CA roots), so the browser needs no token and no route to the
  internet. Firmware and filesystem are installed together, because the web UI lives in
  the filesystem image.
  * Production uses `/releases/latest`, which GitHub defines as the newest release that is
    neither a draft nor a pre-release.
  * Development uses the newest entry of `/releases`, because `/releases/latest` silently
    skips pre-releases - so a dev channel cannot be built on it.
  * The manual `.bin` upload stays available below the new card, for flashing a local
    build when GitHub is unreachable.
  * **Not signature-checked.** With the security features still in Path 1 there is no
    Secure Boot, so an image is only validated as a well-formed ESP image over a verified
    TLS connection. See [PATH2 rollout](docs/content/PATH2_SECURITY_ROLLOUT.md).
* **Development builds report a usable version.** A build from the `dev` tag previously
  reported only a bare commit hash; it now reports `<version>-dev+<hash>`, matching every
  other untagged build.

### Changed

* **First-run setup replaces automatic credential generation.** A factory-fresh device keeps
  the shipped placeholders, leaves Web UI authentication off and shows a blocking setup
  screen where you choose the Web UI password, HomeKit Setup Code, OTA password and setup AP
  password. Nothing is generated, printed or logged any more. Devices configured before this
  change are migrated automatically and are not pushed through setup again.
  See [Security](docs/content/security.md) and
  [PATH2 rollout](docs/content/PATH2_SECURITY_ROLLOUT.md).
* **The setup AP uses WPA2-PSK (CCMP)** instead of WPA2/WPA3 mixed mode with AES-CMAC. The
  mixed-mode cipher suite caused association failures ("connection timeout") on a range of
  clients, so the provisioning AP could not be joined at all. WPA3 hardening belongs on the
  station side, not on a short-lived setup AP.

### Fixed

* **The Web UI returned `404 Nothing matches the given URI` for most URLs.** The HTTP server
  was configured with `max_uri_handlers = 22`, but the main route table registers 28
  handlers. The last six silently failed to register - including the catch-all `{"/*"}` -
  so any unmatched path 404'd. Raised the limit to 48, which also covers the captive-portal
  table when both are registered across an AP/STA transition.
* **The setup AP could not be configured over the network.** With the catch-all handler
  missing, `/wifi_scan` and the portal redirect were unreachable from the device's own AP.
* **Enrolling a node in a household panicked the device instead of enrolling it.** The join
  handler read the request body into a 4096-byte buffer on the HTTP task's stack, which is
  6144 bytes, so it overflowed, panicked and rebooted - after writing the household record
  but before completing it. The node came back looking half-provisioned: the household
  stuck at `PROVISIONING`, the node still `UNCONFIGURED`, and no response ever sent to
  whoever asked, so nothing explained what had happened. Request bodies are now read from
  the heap and capped per endpoint, and a body over the cap is refused with `413` instead
  of being silently truncated into an "invalid JSON" error. Backup restore had the same
  fault with a 16 KiB buffer, so restoring a backup panicked the device every time.
* **The household recovery secret was never actually stored.** Its NVS key was
  `HH_RECOVERY_SECRET` - 18 characters - and NVS keys are limited to 15, so every write of
  it was rejected and `HouseholdManager::save()` could never succeed once a secret existed.
  Two consequences, both silent: the household could never reach `ACTIVE`, because the
  update that flips it went through the same failing call, and the recovery secret that
  household backups are encrypted with did not exist on the device at all while the UI
  reported it as stored. The keys are now `HH_REC_SECRET` and `HH_REC_SALT`, guarded by a
  `static_assert`; the secret and salt are written before the record that refers to them,
  so a failure can no longer leave a device claiming a secret it does not have; and a
  record that makes that claim is corrected at load time rather than repeated.
* **Enrollment reported success for writes that did not reach storage.** Every step of the
  join handler discarded its result and replied `{"success":true}` regardless, so a
  failure and a success were indistinguishable from outside. Failures now report the
  reason (`507`), log the NVS entry counts, and write no enrollment audit record; a state
  that will not survive a reboot is no longer announced. The boot log additionally warns
  when NVS is running low, since a full partition fails *every* write - including updates
  to values that already exist, because NVS appends a new entry per change.

### Known issues

* Phones do not auto-open the captive portal. Probe requests (for example
  `netcts.cdn-apple.com`) carry a foreign `Host` header and are rejected by
  `hostHeaderAllowed()` before the portal redirect runs, so the address has to be typed.
* The PN532 reader repeatedly fails to initialise (`Error establishing PN532 connection`).
  Under investigation; see the NFC notes in the docs.

## 0.10.0 - 2026-09-22

Adds the **Household** multi-node architecture and **support for on-device flash
encryption + Secure Boot V1 + NVS encryption**. Read the *Breaking* section before
updating.

### Security

* **Flash encryption, Secure Boot V1 and NVS encryption are implemented but
  DISABLED by default** in this release. The board stays fully reversible: no eFuses
  are burned and no device data is lost on upgrade.
* The hardening is available as a **deferred, staged, one-way rollout** — see
  [Security Rollout Plan: Path 1 → Path 2](docs/content/PATH2_SECURITY_ROLLOUT.md).
  Enable and verify flash encryption, then NVS encryption, then Secure Boot, then
  release mode, one stage at a time.
* When enabled, the app, NVS and LittleFS contents are encrypted at rest and only
  firmware signed with your Secure Boot key boots. The partition layout then
  changes: an `nvs_keys` partition is added, the partition table moves from `0x8000`
  to `0xD000` (the signed bootloader no longer fits in `0x7000`), and the app
  partitions are realigned to 64 KiB boundaries.
* **OTA from an older build will not boot once enabled** — such devices must be
  re-flashed over serial and fully re-provisioned. Wi-Fi credentials, HomeKit
  pairing and HomeKey reader enrolment are erased when the flash is first
  encrypted.
* Generate the signing key once and keep it safe:
  `espsecure.py generate_signing_key --version 1 keys/secure_boot_signing_key.pem`
  (the original ESP32 only supports Secure Boot V1, which needs an ECDSA-P256 key).
* `keys/` and `*.pem` are now gitignored so the private key cannot be committed.

**Path 1 / Path 2:**

| Path | What it does | Reversible? |
| --- | --- | --- |
| **Path 1 — default** | No eFuses burned, no encryption. Behaves like upstream; plaintext flashing works normally. | ✅ Yes |
| **Path 2 — deferred** | Burns the eFuses, encrypts the flash, Secure Boot locks to your signing key. Encrypted + signed images only. | ❌ **Permanent** |

`sdkconfig.defaults` ships with **Path 1**. See
[Security Rollout Plan: Path 1 → Path 2](docs/content/PATH2_SECURITY_ROLLOUT.md)
for the staged procedure and its prerequisites.

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

### Documentation

* **New page: [Fork vs Upstream](docs/content/fork-vs-upstream.md)** — the complete,
  side-by-side comparison of this fork against
  [rednblkx/HomeKey-ESP32](https://github.com/rednblkx/HomeKey-ESP32), including what
  changed (household, MQTT contract, flash encryption / Secure Boot / NVS encryption)
  and what is unchanged. Also summarised in the README and the wiki home page, and
  cross-linked from every affected page.

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
* **New devices ask you to choose their credentials.** A factory-fresh device no longer
  generates credentials and prints them to the serial log. It comes up with Web UI
  authentication **off** and shows a blocking first-run setup screen where you choose the
  Web UI username and password, HomeKit Setup Code, OTA password and setup AP password.
  Until you save it the Web UI has **no login**, so keep a fresh device on a trusted
  network. If a credential is lost, the setup portal does not require a login and erasing
  NVS returns the device to the first-run screen - see
  [Recovering from a lost credential](docs/content/security.md#recovering-from-a-lost-credential).
* **`espota` needs a custom OTA password** (security default change). Impact: anyone who
  updates over the network with `espota` has to set a password once; the Web UI firmware
  uploader is unaffected. One-line fix: `Web UI → Misc → HomeSpan → OTA Password` → set a
  password → save (the device reboots).
* `/reset_hk_pair`, `/reset_wifi_cred` and `/start_config_ap` are **POST-only** now; scripts
  that used GET need updating.
* Requests must use the device's **IP address or mDNS name**; a custom host name, reverse
  proxy or alias now gets a 401.
