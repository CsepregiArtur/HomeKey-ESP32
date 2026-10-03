# Changelog

Notable changes per release. User-facing detail lives in the docs:
[Security](docs/content/security.md) and [Updates / breaking changes](docs/content/updates.md).

## 0.12.0 - 2026-10-03

Firmware updates over the network come back, and the two features that were paying for the
second application slot are removed to make room for it. **Moving an existing device onto this
layout needs one serial flash**; after that, updates are wireless.

### Added

* **ESP32-C3 support.** The firmware builds and runs on an ESP32-C3, and the update script
  detects the chip on the cable and flashes the right images for it. Two things are chip
  specific and both are handled:
  * **The bootloader offset is not the same.** A classic ESP32 puts it at `0x1000`, a C3 at
    `0x0`. Writing a C3 bootloader to `0x1000` leaves the chip printing
    `invalid header: 0xffffffff` forever. The script reads the offset from the build's own
    `flash_args` instead of assuming.
  * **The NFC pins differ.** The classic ESP32 uses VSPI on GPIO18/19/23/5, but a C3 only has
    GPIO0-21, so three of those pins do not exist there. The C3 defaults are `SCK=4, MISO=5,
    MOSI=6, SS=7`. The values come from the Arduino core's variant for the selected chip, so
    they follow the target automatically - but wiring must not be copied between boards.
  * `scripts/build_esp32c3.sh` works around a **macOS/Homebrew host problem** where IDF's
    `riscv32-esp-elf-as` (a Rust dispatcher) fails to resolve its real assembler and the OS
    falls back to Apple's `as`, which rejects RISC-V flags with
    `clang: error: unknown argument: '--traditional-format'`. The cause is the assembler, not
    the compiler, despite what the message implies. The helper puts a correct `as` on `PATH`.
  * **The C3 image is ~118 KB bigger** (1,870,176 B vs 1,752,256 B), so its dual-slot headroom
    is thin: 30,368 B (1.60%) against the classic ESP32's 148,288 B (7.80%).

* **Firmware updates over the network are back, without a web UI for them.** The flash layout
  returns to two application slots (`ota_0`/`ota_1`) plus `otadata`, so an image is written into
  the slot the device is *not* running from and a corrupt upload leaves the running firmware
  untouched. `nvs` keeps the 92 KiB the single-slot layout gave it rather than shrinking back to
  24 KiB, because NVS only moves forward and shrinking it could silently drop Wi-Fi credentials,
  HomeKit pairing or reader enrolment - the 68 KiB comes out of the two application slots instead
  (1856 KiB each). **Every existing device needs one serial flash to move onto this table**: the
  old table is what describes where an image may be written, and it does not know about the new
  slots. After that one cable, updates are wireless.
  * `POST /api/ota/firmware` streams an image straight into the inactive slot in 4 KiB chunks.
    POST-only, requires the Web UI credentials, and refuses over plain HTTP - an image is the most
    valuable thing a caller can send, and letting it cross the LAN in the clear would let anyone
    on the network read it and substitute their own.
  * `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` with a confirmation at the end of `setup()`. Without
    that explicit confirmation a freshly installed image would be abandoned on the next reset and
    an update would look like it silently undid itself.
  * **`scripts/ota_update.py`** drives both paths from one command: it flashes over the cable when a
    device is attached, otherwise discovers devices over mDNS and asks which to update - one, or
    all of them for a mass update. The device certificate is **pinned by fingerprint** (the
    certificate is self-signed with a fixed validity, so neither the chain nor the hostname can be
    checked), the Web UI password lives in the **macOS Keychain**, and every secret is masked before
    anything is printed. `--prepare-for-ui` writes the ready-to-upload image next to a `.sha256`
    checksum for the web UI's Update page.
  * **The web UI has an Update page again** - see below - which is why the script asks whether to
    prepare a file for it.

* **A firmware update page in the web UI.** *Update* in the navigation menu shows the running
  version and partition and the application slot size, takes a `.bin` file, and reports upload
  progress (1.7 MB over Wi-Fi is long enough that a silent bar looks like a hang). It requires
  HTTPS, and refuses over plain HTTP with an explanation. A device on the single-slot layout
  reports that it has nowhere to write an update instead of failing later.

### Changed

* **Ethernet was removed.** The driver, its configuration, its `/eth_get_config` endpoint, the
  `ethernetEnabled` field in the Web UI and the captive-portal Ethernet save path are gone; the
  transport is Wi-Fi only. The configuration fields remain so an existing NVS blob still
  deserializes, but nothing reads them. Measured saving: **~100 KB of flash**.

* **The PN7160 and ST25R3916 NFC readers were removed; this build is PN532-only.** The `nfcReaderType`
  setting accepts only `0`, and anything else is rejected with an explanatory message rather than
  silently ignored. Measured saving: **~17 KB**.

  Together these free **~120 KB**, which is what makes the dual-slot layout fit: it takes the
  application slot from 4.8% free to ~7.8%, and in the single-slot layout from 52% to 55%.

  Superseding the previous cycle: that removal made the single-slot layout the only option, and
  this release puts the choice back. `no_ota.csv` stays in the tree and documented for anyone who
  prefers its ~55% free application slot and does not need network updates.

* **Guest NFC tags - temporary access for people without an Apple device.** Teach an
  ordinary NTAG213/215/216 card on a node with a PN532 reader and it unlocks exactly like a
  HomeKey tap: verified and driven **locally**, with no MQTT, Home Assistant or internet
  involved in the tap itself. Each tag has an optional validity window, can be revoked, and
  is distributed across the household so every node verifies the same card offline.
  Configured from Home Assistant over `/api/ha/guest/*` (TLS + device credential) or over
  MQTT; discovery publishes *Guest access* and *Guest tags* sensors.
  * The card holds an XChaCha20-Poly1305 payload keyed by a random per-tag token and bound to
    the card's UID, so it cannot be forged and a payload moved to a different card is refused.
  * **A guest tag is not a HomeKey credential and cannot become one** - HomeKey is signed with
    Apple-issued keys in a secure element. NTAG memory is static, so a person who reads the
    card with any reader can clone it. Guest tags are convenience, not security; use HomeKey
    for anything you rely on. The design and its limits, plus the NTAG 424 DNA upgrade path,
    are written up in [Guest NFC tags](docs/content/guest-tags.md).
  * Off by default, 16 tags per node, omitted from backups (tokens are per-node state).

* **Back up the keys, not just the configuration.** A backup can now carry this device's own
  credentials - the reader credential store (its identity plus the enrolled issuers' endpoint
  keys) and the HomeKit pairing state - so a replacement node comes back as *the same device*:
  no tags re-enrolled, nothing re-paired, the existing Apple Home setup still recognises it. It
  is **off unless asked for**, and it sits inside the encrypted payload with everything else.
  `POST /backup/create` takes `{"include_credentials": true}` and answers with what it actually
  managed to put in (`includes_credentials`), and a restore that applied credentials answers
  `reboot_required: true` and restarts to put them into use.
  * A backup taken with this is the keys to the door, not a copy of the settings: it is worth
    a password manager and an offline copy rather than a chat message.

* **Verify OTA images without burning eFuses.** The "signed apps without Secure Boot"
  configuration - and what it implies for updates - is documented in
  [Security](docs/content/security.md#verify-ota-images), with hardware evidence: enabling it
  took the same device from five security findings to four, `ota_signature` among those
  resolved, with no eFuse burned and USB flashing still working.

* **Restore a backup from the Web UI.** **Recovery → Restore from a backup** takes the backup
  file (or pasted hex) and the offline recovery secret and restores household membership and
  configuration onto a replacement node. The endpoint already existed but had no interface, so
  a "one push restore" was really two API calls. The form states what it needs and what it
  refuses: the recovery secret, and a device that does not already have its own identity
  (restore mints a replacement one, and identities are never cloned).
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

* **Corrected a security doc claim.** The "Verify OTA images" section said image
  verification was enabled by default; a build from the committed defaults verifies
  nothing and reports `ota_signature: WARNING`, which is now what the section says.

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
* **Every timestamp the device reported was seconds since boot.** Nothing ever set the
  clock: there is no RTC, no time source, and `wallClockSeconds()` in `MqttManager` and
  `AuditManager` deliberately falls back to uptime whenever `time()` looks unset. With
  nothing to set it, that fallback was permanent, so lock changes, HomeKey authorisations,
  audit records and backups all carried an uptime where a date belongs - which is why Home
  Assistant showed 1970 for the last authentication and why "when did this door open" had
  no answer. SNTP now starts when the station interface comes up, and the boot log reports
  the first successful sync. Verified on the device: it reports an epoch within a second of
  the host clock.
* **User-typed names could break any document carrying them.** `node_name`, `household_name`
  and the audit metadata were interpolated into JSON with `fmt::format`, so a single quote in
  a device or household name made the whole document unparseable - the `state` topic,
  `/household`, `/node` and `/audit` - and took every field after the name with it, not just
  the name. All of them now go through one `jsonEscape()` helper, and `BackupManager`'s private
  copy of that helper was replaced by the shared one.
* **"Backup completed" could be read as "backup stored".** Nothing keeps a copy - the device
  records only the time and hash - so the Backup page now says so plainly, makes **Download**
  the primary action rather than a secondary one, and warns before the page is closed with a
  backup that was never saved. The `backup/status: completed` topic means *produced*, not
  *saved*, because it is retained and goes on saying so either way.

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
