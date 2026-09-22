# HomeKey Household — Hardening & Validation Plan

Tracks the multi-phase hardening/validation of the HomeKey Household multi-node
architecture. Status legend: `[x]` done, `[~]` in progress, `[ ]` pending.

## Current verified state (2026-09-22)

- ESP-IDF 5.5.5 build: **PASS** (0 errors; pre-existing warnings remain — see Phase 20)
- Svelte build: **PASS**
- Svelte type check: **PASS** (0 errors, 0 warnings)
- Backup crypto host tests: **8/8 PASS** (`tests/test_backup_crypto.py`)
- MQTT command authentication tests: **7/7 PASS** (`tests/test_mqtt_command_auth.py`)
- NVS migration reviewed
- Node identity reviewed
- Backup crypto reviewed
- MQTT security reviewed
- Audit logging implemented
- Health manager implemented
- MQTT API documented (`docs/content/mqtt_household_api.md`)
- Node online + health MQTT discovery implemented

## Remaining items (source of truth)

1. **Phase 9** — HomeKey data classification + Apple Home re-provisioning limitation
2. **Phase 12** — MQTT discovery for backup / security / firmware / last-auth
3. **Phase 13** — Manual mobile responsiveness review of the new Web UI pages
4. **Phase 17** — Hardware tests remain ESP32-only; do not mark as executed
5. **Phase 18** — RAM/flash review, large allocations, leak risks
6. **Phase 19** — Host tests: node identity, provisioning expiry/replay, migration
7. **Documentation + plan.md update**

## Build & toolchain (verified)

| Tool | Status | Notes |
|---|---|---|
| ESP-IDF (`idf.py`) | ✅ v5.5.5 | `~/esp/esp-idf`, xtensa-esp-elf-gcc 14.2.0 |
| PlatformIO (`pio`) | ❌ not installed | `idf.py build` is the primary path |
| Python | ✅ 3.14.6 | |
| bun | ✅ 1.4.2 (via npm) | |
| node | ✅ v26.3.0 | |

---

## Phase 1 — Build the existing project
- [x] Detect tools
- [x] `idf.py set-target esp32`
- [x] `idf.py build` → **SUCCESS** (0 errors; pre-existing warnings remain, `HomeKey-ESP32.bin` ~0x1bda80 bytes, 7% free)
- [x] Fixed: `bun: command not found` (installed bun), `setupAuditHooks` corruption, `BackupManager.cpp` corruption
- [ ] `pio run` — skipped: `pio` not installed (documented blocker)

## Phase 2 — Frontend build & type check
- [x] `bun run build` → **SUCCESS** (201 modules, brotli assets)
- [x] `bun run check` → **SUCCESS** (0 errors, 0 warnings)
- [x] Fixed 11 pre-existing type errors: `WebSocketConnectionState` export, `NodeJS.Timeout`,
      `route.meta.*Data?.x` prop nullability (widened `AppMisc`/`AppMqtt`/`AppActions`/`HKInfo` props)
- [x] Fixed 4 a11y warnings in `provision/index.svelte` (label association)

## Phase 3 — Review the new architecture
- [x] Verify manager init/lifecycle/event-bus wiring (build + inspection)
- [x] Confirm LockManager remains single source of truth for lock state
- [x] Confirm no manager bypasses LockManager / ConfigManager
- [x] Confirm no cyclic dependencies

## Phase 4 — NVS / migration safety
- [x] Review all NVS writes; confirm no auto-erase, HomeKey/config preserved
- [x] Migration idempotency + reboot-during-migration safety (NVS atomic blob writes)
- [x] Version marker review (`CONFIG_VERSION_CURRENT`)

## Phase 5 — Node identity security
- [x] Confirm new keypair per device; replacement never clones private key
- [x] Duplicate-node scenario review (`createReplacementIdentity` refuses if an identity exists)

## Phase 6 — Cryptography review
- [x] Document exact constructions in `docs/content/security.md`
- [x] Nonce uniqueness, key derivation, salt, tags, constant-time compares, zeroization (`sodium_memzero`)

## Phase 7 — Backup security review
- [x] Explicit A/B/C/D classification table (docs/content/household.md)
- [x] Confirm versioned format, no raw NVS dumps; unknown/future versions rejected

## Phase 8 — Backup round-trip test
- [x] `tests/test_backup_crypto.py` (8/8): round-trip, wrong key, corrupted ciphertext/tag/signature, truncated, unsupported/future version

## Phase 9 — HomeKey data review (detailed per-field classification)
- [x] Inspected actual implementation (not inferred from names): `ReaderDataManager`
      (`NvsCredentialStore`, NVS `READERDATA` msgpack), `DigitalDoorKey`
      (`set_reader_key` RKR provisioning), HomeKit credential provisioning
      (`controllerCallback` issuer sync), reader identity, issuer storage,
      endpoint persistent keys — full generate→store→read→use→regenerate trace.
- [x] Detailed table in `docs/content/household.md` with the exact 9 columns
      (`Field | Storage | Purpose | Scope | Can backup? | Can restore to replacement node? | Must regenerate? | Must re-provision in Apple Home? | Reason`)
- [x] Every field classified A/B/C/D; reader keys + endpoint persistent keys = D
      (never cloned/backed up); issuer LTPK = A (public); recovery secret = C.
- [x] Restore model documented: configuration restore vs household membership restore
      vs HomeKey credential restoration (the last is not possible by design).
- [x] Web UI recovery warning updated to distinguish configuration restore from
      HomeKey credential restoration (see Phase 13).

## Phase 10 — MQTT security
- [x] Review TLS, per-node identity, ACL, HMAC command auth, nonce/timestamp/req-id replay protection
- [x] Fail-closed behavior; reject `unlock=true`; `tests/test_mqtt_command_auth.py` (7/7)

## Phase 11 — MQTT topic documentation
- [x] `docs/content/mqtt_household_api.md` (direction/topic/payload/retain/QoS/auth)

## Phase 12 — Home Assistant MQTT discovery
- [x] Node online + Node health entities (stable ids `<hid>_<nid>_<entity>`)
- [x] **Each household entity has its own discovery topic** `<hid>_<nid>_<entity>`
      (object id == unique-id suffix, except node-online which keeps `<hid>_<nid>`);
      fixed the post-audit collision (D1) where all sensors shared one topic
- [x] Household node-online availability via the shared broker LWT
      (`availability_topic`, D3 fix)
- [x] Preserve existing lock/tag discovery; no duplicate entities
- [x] No duplicate entities after reconnect/reboot (stable unique ids, retained config)
- [x] Backup status — `<hid>_<nid>_backup` (sensor on `.../backup/last`; state + timestamp
      attributes; encrypted contents never exposed)
- [x] Security status — `<hid>_<nid>_security` (sensor on `.../security`; OK/WARNING/ERROR,
      no numeric score; no per-finding entity explosion)
- [x] Firmware version — `<hid>_<nid>_firmware` (sensor on `.../state` → firmware_version;
      no OTA orchestration)
- [x] Last HomeKey authentication — `<hid>_<nid>_last_auth` (sensor on `.../last_auth`;
      type/result/timestamp only; no credential ids/APDU/crypto)
- [x] `publishNodeStatus()` now actually invoked (on connect + 30 s loop) — previously dead
      code; `/security`, `/backup/last`, `/last_auth` topics added
- [x] Existing MQTT Discovery conventions preserved; legacy discovery unbroken

## Phase 13 — Web UI review
- [x] Pages build + type check; error/loading/success states present; secrets masked
- [x] Recovery page distinguishes CONFIGURATION RESTORE vs HOMEKEY CREDENTIAL RESTORATION
- [~] Mobile responsiveness reviewed by layout inspection (all pages use responsive
      `max-w-3xl` DaisyUI layouts, `overflow-x-auto` tables, labeled forms); no
      physical-device rendering performed
- [x] Real issues fixed: recovery warning clarified; provisioning expiry now displayed
      (backend returns `ttl_seconds`, UI shows it). No accidental sensitive data rendered
      (only public keys/fingerprints; secrets never sent to UI).

## Phase 14 — Security warnings
- [x] Security page lists HTTPS / MQTT TLS / mTLS / OTA signature / Secure Boot / Flash encryption / Web auth / HomeSpan OTA (no numeric score)

## Phase 15 — Audit log
- [x] Event coverage (HomeKey auth, lock/unlock, MQTT command, provisioning, backup, restore, enrollment)
- [x] Bounded storage (256 fixed-size records), no secrets logged

## Phase 16 — Health manager
- [x] All fields reported (network/mqtt/nfc/lock/backup/cert/firmware/uptime/heap/reset/security), best-effort non-blocking

## Phase 17 — Failure-mode testing
- [~] Scenarios 1–15 reasoned by code-path inspection; scenario 15 (local HomeKey with MQTT/HA offline) verified by inspection — NFC→LockManager→HomeKit has no MQTT dependency
- [ ] Hardware-in-the-loop tests (require ESP32) — must remain explicitly marked
      ESP32-only / HARDWARE_ONLY; do not pretend they were executed

## Phase 18 — Memory / performance review (findings by inspection)
- [x] Inspected `BackupManager`, `RestoreManager`, `MqttManager`, `AuditManager`,
      `HealthManager`, `WebServerManager`.
- [x] Findings: no unbounded vectors (audit ring 256, nonce deque 32); backup plaintext is
      `sodium_memzero`'d after encryption; only bounded transient buffer copies (config-sized);
      no leaks identified; no blind optimization performed.
- [x] Heap APIs already used where practical: `HealthManager::snapshot()` reports
      `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` (free_heap in `/health`). No invasive
      instrumentation added; free/min-free heap is exposed via the existing health snapshot.
- [x] Flash wear: telemetry (health/status/security) stays RAM/MQTT and does NOT write NVS.
      Audit writes are bounded (256 × 59-byte records in 8 NVS slots) but each record is one
      NVS commit — acceptable for user-driven events, not telemetry.
- [~] WebServerManager reviewed at handler level only (very large file; no large
      allocations observed in the reviewed paths).

## Phase 19 — Unit tests
- [x] Host-side: `tests/test_backup_crypto.py` (8/8), `tests/test_mqtt_command_auth.py` (7/7)
- [x] `tests/test_node_identity.py` (10/10) — unique/deterministic identity, replacement
      identity, duplicate rejection, private-key non-cloning (signature cross-verification)
- [x] `tests/test_provisioning.py` (17/17) — generation, hashing, expiry, single-use,
      replay, wrong token, wrong household (cross-household replay), malformed token
- [x] `tests/test_migration.py` (12/12) — fresh install, old config, migration, already
      migrated, repeated migration, invalid version, unsupported future version
- [x] Hardware-dependent tests marked explicitly `HARDWARE_ONLY` / ESP32-only
- [x] Firmware fail-closed version guard added to `HouseholdManager` (load + migrate) so
      "unsupported future version" is actually rejected (not just a test mirror)

## Phase 20 — Real build (final)
- [x] `idf.py build` after all fixes (0 errors; pre-existing warnings remain —
      missing-field-initializers / unused-variable across `eth_structs.hpp`,
      `WebServerManager.cpp`, `pn7160`, `dns_server`, `esp-modbus`, `DigitalDoorKey`;
      + 2 unknown kconfig symbols). None introduced by this phase's changes.
- [x] `bun run build` + `bun run check` (0 errors, 0 warnings)

## Phase 21 — Documentation
- [x] `ARCHITECTURE_REPORT.md`, `docs/content/security.md`
- [x] `docs/content/mqtt_household_api.md` — new topics + full discovery entity table
- [x] `docs/content/household.md` — detailed 9-column classification table + restore model
- [x] `plan.md` statuses kept accurate; hardware-only tests not marked complete

## Phase 22 — NOT in scope this phase
- Home Assistant custom integration, HACS, mobile app, cloud backend

---

## Security consistency review (final)

- [x] No plaintext secrets in logs (provisioning code never logged; recovery secret
      exported once and never logged; node/reader private keys never logged). First-boot
      credential display is an intentional one-time exception.
- [x] No private node keys cloned (`generateIdentity`/`createReplacementIdentity` refuse
      if an identity exists; private key never serialized/backed up).
- [x] No sensitive HomeKey data over MQTT: `last_auth` = type/result/timestamp only;
      `backup/last` = status/timestamp only; encrypted backup contents never published.
- [x] Invalid unlock commands rejected (HMAC auth + fail-closed; plain `unlock=true` never
      accepted; legacy numeric topics unchanged).
- [x] MQTT replay protection intact (bounded 32-entry nonce deque + ±300 s window).
- [x] TLS/security assumptions documented (`docs/content/security.md`).
- [x] Local HomeKey independent of MQTT/HA (NFC→LockManager→HomeKit path).
- [x] LockManager remains single source of truth; no manager bypasses it; no cyclic deps.
- [x] No numeric security score (security page shows OK/WARNING/DISABLED badges only).

## Home Assistant V2 readiness — final decision (Phase K)

Declare **READY FOR HOME ASSISTANT V2** only when ALL of the following are satisfied:

- [x] stable MQTT namespace
- [x] stable payload schemas
- [x] stable node identity
- [x] stable health/state reporting
- [x] stable backup status
- [x] stable security status
- [x] stable last-auth reporting
- [x] secure lock/unlock command format
- [x] no secrets exposed through MQTT
- [x] documented HomeKey replacement limitation
- [x] successful firmware build
- [x] successful frontend build
- [x] host tests passing

**Decision: READY FOR HOME ASSISTANT V2** (all criteria satisfied; firmware build
succeeds with pre-existing warnings only, unrelated to the household API). Do NOT
implement the Home Assistant V2 custom integration, HACS, mobile app, or cloud
storage in this phase.

## Final acceptance criteria

- [x] ESP32 project builds successfully (`idf.py build`, 0 errors; pre-existing warnings remain)
- [~] PlatformIO build — not available (`pio` not installed)
- [x] Svelte build succeeds
- [x] Svelte type check succeeds (0 errors/0 warnings)
- [x] NVS migration reviewed
- [x] Node identity reviewed
- [x] Backup crypto reviewed
- [x] Backup round-trip tested
- [x] Corrupted backup rejected
- [x] Wrong-key backup rejected
- [~] Provisioning expiry — host logic tested (`test_provisioning.py`); wall-clock source is firmware-side (untested on HW)
- [x] Provisioning replay rejected (logic tested)
- [x] MQTT replay rejected (logic tested)
- [x] Invalid unlock rejected
- [x] Local HomeKey works independently of MQTT (by code-path inspection)
- [x] MQTT discovery extended (node online/health + backup/security/firmware/last-auth)
- [x] Audit log reviewed
- [x] Security warnings reviewed
- [x] Documentation updated
- [x] No plaintext secrets logged
- [x] No node private keys cloned
- [x] HomeKey replacement limitation documented (household.md + Web UI)

## Progress log

- 2026-09-22 — Phase 1/2/20 done: `idf.py build` green (0 err/0 warn), `bun run build` green, `bun run check` green (0/0). Fixed pre-existing frontend type errors + a11y warnings.
- 2026-09-22 — Security hardening: `sodium_memzero` zeroization, backup version check (reject unknown/future), fixed `v < 0` warning, added node online/health HASS discovery entities, added HomeKey re-provisioning Web UI warning.
- 2026-09-22 — Host tests: `tests/test_backup_crypto.py` 8/8, `tests/test_mqtt_command_auth.py` 7/7 (real libsodium via ctypes).
- 2026-09-22 — Docs: `security.md` (crypto constructions), `mqtt_household_api.md`, `household.md` (field classification table).
- 2026-09-22 — `plan.md` restructured: added "Current verified state", "Remaining items", and "Home Assistant V2 readiness" decision criteria; expanded Phases 9/12/13/17/18/19/21 with the full remaining-work detail and corrected over-claimed acceptance items.
- 2026-09-22 — Completed remaining hardening: Phase 9 classification table (9 columns) + restore model in `household.md`; Phase 12 backup/security/firmware/last-auth discovery entities + `/security`, `/backup/last`, `/last_auth` topics + wired `publishNodeStatus` (on connect + 30 s loop); Phase 13 recovery warning clarified + provision expiry display; Phase 18 memory/flash review (no unbounded allocs; audit bounded; telemetry RAM/MQTT-only); Phase 19 host tests 10/10 + 17/17 + 12/12 (54/54 total). Added fail-closed future-version guard to `HouseholdManager`. Corrected the "0 warnings" build claim (pre-existing warnings remain). Final decision: READY FOR HOME ASSISTANT V2.
- 2026-09-22 — MQTT contract audit + remediation: created `docs/content/mqtt_api_contract_matrix.md`; fixed D1 (per-entity discovery topics), D3 (household availability via shared LWT), D2/D4/D5/D6/D7 (docs). Tests 54/54, firmware/frontend builds pass. MQTT contract: STABLE → proceed to HA V2.

