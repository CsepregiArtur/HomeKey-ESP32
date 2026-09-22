# HomeKey-ESP32 → HomeKey Household — Architecture Report

This report is the inspection deliverable required before implementing the multi-node
"HomeKey Household" architecture. It documents what exists today, what must be
preserved, and how the new modules map onto the current codebase.

## 1. Current architecture (verified in source)

### 1.1 Build & framework
- ESP-IDF ≥ 5.5.4 (CI: 5.5.5), C++17/20, `main/CMakeLists.txt` uses `idf_component_register`.
- `main` PRIV_REQUIRES: `HomeSpan pn532_hal pn7160 DigitalDoorKey esp_https_server mqtt libsodium msgpack-c json loggable loggable_espidf esp_wifi dns_server`.
- `fmt` fetched via `FetchContent`; `MINIMAL_BUILD` enabled.
- PlatformIO build also exists (`platformio.ini`, local ESP-IDF checkout).

### 1.2 Event bus (`app_event_loop.hpp/.cpp`, `app_events.hpp/.cpp`)
- Thin wrapper over `esp_event`. `AppEventLoop::publish(base, id, data, size)` prepends a
  `uint16_t` length; `subscribe` returns a RAII `SubscriptionHandle`.
- Event bases: `LOCK_EVENT`, `NFC_EVENT`, `HK_EVENT`, `HW_EVENT`, `MQTT_EVENT`, `ETH_APP_EVENT`.
- Payload structs live in `eventStructs.hpp` and are (de)serialized with **alpaca**
  (`espp/serialization`), e.g. `EventLockState`, `EventHKTap`, `EventMqttStatus`, `NfcEvent`, `HomekitEvent`.

### 1.3 NVS namespaces & keys
- Single namespace **`SAVED_DATA`** is used by both `ConfigManager` and `NvsCredentialStore`.
- `ConfigManager` keys: `MQTTDATA`, `MQTTSSLDATA`, `MISCDATA`, `HTTPSDATA`
  (each a MessagePack blob of the corresponding struct; `MISCDATA` carries misc + actions).
  `hasStoredConfig()` = presence of non-empty `MISCDATA`.
- `NvsCredentialStore` (`ReaderDataManager`) key: `READERDATA` (MessagePack blob) in `SAVED_DATA`.
- HomeSpan keeps its own NVS namespace for HAP pairing/SRP data.

### 1.4 Configuration structures (`config.hpp`, `namespace espConfig`)
- `mqttConfig_t` (broker, creds, topics composed from `platform_create_id_string()`, flags, SSL).
- `mqtt_ssl_t` (caCert/clientCert/clientKey).
- `https_certs_t` (serverCert/privateKey/caCert).
- `misc_config_t` (device name, setup code, OTA pwd, HomeKey color, reader type/pins, eth, web auth, HTTPS, AP password, …).
- `actions_config_t` (NeoPixel, GPIO feedback, relay, alt action, dumb-switch).
- Secret masking: `espConfig::isSecretKey()` (any key containing `Password`/`Passwd`),
  `MASKED_SECRET = "********"`. Write path refuses to store the mask.

### 1.5 HomeKey credential storage (device-specific vs restorable)
`NvsCredentialStore` implements `ddk::CredentialStore`:
- `ReaderIdentity`: `private_key`, `public_key`, `public_key_x`, `group_identifier`, `sub_identifier`.
- `Issuer`: `id`, `public_key`, `public_key_x`, `endpoints[]`.
- `Endpoint`: `id`, `public_key` (65B), `public_key_x`, `persistent_key` (32B), `used_at`, `counter`, `key_type`.

**Classification (drives the backup format):**
| Field | Category | Restorable? | Reason |
|---|---|---|---|
| `reader_private_key` | D | **No** | device-specific; cloning = key-clone of a reader |
| `reader_public_key` / `public_key_x` | C | No (re-derived) | derived from private key |
| `group_identifier` / `sub_identifier` | C | No | reader identity; re-issued on re-provisioning |
| `issuer.public_key` | A/B | **Yes** | public trust data |
| `issuer.id` | A/B | **Yes** | public identifier |
| `endpoint.public_key(_x)` | A/B | **Yes** (public) | public |
| `endpoint.persistent_key` | D | **No** | per-pairing shared secret |
| `endpoint.counter` / `used_at` | C | No | anti-replay state tied to reader key |

**Consequence:** replacing a node always issues a **new reader identity**, which
**invalidates all enrolled HomeKey devices**. Those must be re-provisioned through the
Apple Home app. This cannot be safely automated and is documented as a hard limitation.

### 1.6 MQTT (`MqttManager`, `docs/content/mqtt.md`)
- Topics are composed from `<CLIENT_ID>` (`ESP_XXXXXXXX`):
  `.../homekit/state`, `set_state`, `set_current_state`, `set_target_state`,
  `set_battery_lvl`, `homekit/custom_state`, `set_custom_state`, `homekey/auth`,
  `alt_action`, `status` (LWT).
- Home Assistant MQTT Discovery (`publishHassDiscovery`): lock, issuer tag, endpoint tag, NFC tag.
- TLS support via `mqtt_ssl_t`; warns on non-TLS.

### 1.7 Web server (`WebServerManager`)
- `esp_http_server` / `esp_https_server`; static LittleFS assets (brotli/gzip), WebSocket (`/ws`).
- Routes: `/config`, `/config/save`, `/config/clear`, `/eth_get_config`, `/nfc_get_presets`,
  `/reboot_device`, `/reset_hk_pair`, `/reset_wifi_cred`, `/start_config_ap`, `/ota/*`,
  `/certificates` (POST/GET/DELETE), catch-all `/*`.
- Captive portal route set under `setupCaptivePortalRoutes()`.
- Hardening: `hostHeaderAllowed()` (DNS rebinding), POST-only state changes, constant-time auth.

### 1.8 Certificates & OTA
- Certificates stored via `ConfigManager` (`saveCertificate`/`loadCertificate`/`getCertificatesStatus`),
  validated with mbedTLS; used for HTTPS (server cert/key + optional CA for mTLS) and MQTT TLS.
- OTA: `handleOTAUpload` (firmware + LittleFS), streaming task, progress broadcast.
  Optional image signature verification via `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`.

## 2. Target design (Household / Node)

### 2.1 Two-level model
- **Household**: `household_id`, `household_name`, state, trust info, recovery metadata,
  provisioning info, configuration version.
- **Node**: `node_id`, `node_name`, `node_role`, node cryptographic identity
  (Ed25519 keypair via libsodium), node certificate/fingerprint, hardware identity,
  local config, local HomeKey reader identity, local lock config.

### 2.2 Security invariants
- Node identity is **never cloned**. A replacement node gets a new `node_id`
  (generation bump) and a new keypair/certificate.
- Household membership/trust data is separately restorable.
- Local HomeKey unlock works with **no** Home Assistant, MQTT or internet.

### 2.3 New modules (event-driven, no tight coupling)
| Module | Responsibility |
|---|---|
| `HouseholdManager` | household id/name/state/provisioning/trust/recovery/config-version |
| `NodeIdentityManager` | node_id, keypair, certificate fingerprint, generation, conflict detection |
| `ProvisioningManager` | single-use, expiring, replay-protected enrollment tokens |
| `BackupManager` | versioned encrypted backup (A/B/C/D classification), integrity/signature |
| `RestoreManager` | replacement-node workflow, never clones node/reader identity |
| `SecurityManager` | security posture (named warnings, never a numeric score) |
| `AuditManager` | bounded security audit log |
| `HealthManager` | aggregated health snapshot |

### 2.4 New event bases
`HOUSEHOLD_EVENT`, `NODE_EVENT`, `BACKUP_EVENT`, `AUDIT_EVENT`, `HEALTH_EVENT`,
`PROVISION_EVENT`, `SECURITY_EVENT` (added to `app_events.hpp/.cpp`).

### 2.5 Storage plan (same `SAVED_DATA` namespace, distinct keys)
- Node: `NODE_META` (blob), `NODE_PRIVKEY` (blob — never exposed).
- Household: `HH_META` (blob), `HH_RECOVERY_SECRET` (blob), `HH_RECOVERY_SALT` (blob), `HH_RECOVERY_EXPORTED` (u8).
- Provisioning: `PROV_HASH` (blob), `PROV_EXPIRY` (u64), `PROV_USED` (u8).
- Audit: `AUDIT_<slot>` chunked blobs (bounded ring).
- Backup last-run: `BK_LAST_TIME` (u64), `BK_LAST_HASH` (blob).
- Restore: `RESTORE_STATE` (u8).

### 2.6 Backup format (versioned, encrypted, signed)
```
HomeKeyHouseholdBackup {
  header: format_version, household_id, node_id, node_role, generation,
          timestamp, firmware_version, node_public_key, recovery_salt
  nonce (24B) + ciphertext + tag        // XChaCha20-Poly1305 (libsodium)
  signature (64B Ed25519 over header + nonce + ciphertext + tag)
}
Key = BLAKE2b("HK-HOUSEHOLD-BACKUP-v1" || recovery_secret || salt)
```
Plaintext payload classification:
- **A** restorable household data (id, name, trust, recovery metadata, config version)
- **B** restorable node configuration (config blobs; secrets inside are covered by AEAD)
- **C/D** excluded: node private key, node cert, reader private key, endpoint persistent keys
- Issuer **public** keys/endpoints are included (safe); reader identity is re-issued on restore.

### 2.7 Migration (never erase NVS)
On first boot after upgrade, if `HH_META` is absent: create household config
(state `UNCONFIGURED`), generate a node identity (generation 1) and record
`CONFIG_VERSION_CURRENT`. Existing HomeKey/config/NVS data is preserved untouched.

## 3. Remaining risk / open items
- `node certificate` signing: no CA exists in-tree today; the fingerprint is a
  SHA-256 over `node_id + public_key` placeholder until a household CA is provisioned.
- HomeKey re-provisioning after node replacement is a manual step (documented).
- Audit/backup metadata bounded per flash constraints; full history is external.
