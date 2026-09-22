#include "BackupManager.hpp"
#include "AuditManager.hpp"
#include "ConfigManager.hpp"
#include "HouseholdManager.hpp"
#include "NodeIdentityManager.hpp"
#include "ReaderDataManager.hpp"
#include "app_event_loop.hpp"
#include "config.hpp"
#include "eventStructs.hpp"
#include "household_types.hpp"
#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <fmt/format.h>
#include <sodium.h>
#include <time.h>

const char *BackupManager::TAG = "Backup";

namespace {

constexpr uint8_t kFormatVersion = 1;
constexpr size_t kSaltLen = 16;
constexpr size_t kPkLen = 32;

uint32_t nowSeconds() {
    const time_t t = time(nullptr);
    if (t > 1000000000) {
        return static_cast<uint32_t>(t);
    }
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

void emitBackupEvent(uint8_t id, bool success, const std::string &message) {
    EventBackupStatus ev{};
    ev.event = id;
    ev.success = success;
    ev.message = message;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(BACKUP_EVENT, id, buf.data(), buf.size());
}

std::string hexEncode(const std::vector<uint8_t> &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

std::string jsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/// BLAKE2b key derivation; must match HouseholdManager::deriveBackupKey().
std::vector<uint8_t> deriveKey(const std::vector<uint8_t> &secret,
                               const std::vector<uint8_t> &salt) {
    std::vector<uint8_t> material;
    material.insert(material.end(), secret.begin(), secret.end());
    material.insert(material.end(), salt.begin(), salt.end());
    std::vector<uint8_t> key(32);
    crypto_generichash(key.data(), key.size(), material.data(), material.size(),
                       reinterpret_cast<const unsigned char *>(household::kBackupKeyLabel),
                       sizeof(household::kBackupKeyLabel) - 1);
    return key;
}

void appendByte(std::vector<uint8_t> &buf, uint8_t v) { buf.push_back(v); }
void appendU32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}
void appendBytes(std::vector<uint8_t> &buf, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    buf.insert(buf.end(), p, p + len);
}

/// Serialize the plaintext header prefix (everything the signature + AEAD bind to).
std::vector<uint8_t> buildHeaderPrefix(const BackupManager::BackupMeta &meta,
                                       const std::vector<uint8_t> &salt,
                                       const std::vector<uint8_t> &nonce) {
    std::vector<uint8_t> buf;
    appendByte(buf, meta.format_version);
    appendByte(buf, static_cast<uint8_t>(std::min(meta.household_id.size(), size_t(24))));
    appendBytes(buf, meta.household_id.data(), std::min(meta.household_id.size(), size_t(24)));
    appendByte(buf, static_cast<uint8_t>(std::min(meta.node_id.size(), size_t(24))));
    appendBytes(buf, meta.node_id.data(), std::min(meta.node_id.size(), size_t(24)));
    appendByte(buf, meta.node_role);
    appendByte(buf, meta.generation);
    appendU32(buf, meta.timestamp);
    appendByte(buf, static_cast<uint8_t>(std::min(meta.firmware_version.size(), size_t(32))));
    appendBytes(buf, meta.firmware_version.data(), std::min(meta.firmware_version.size(), size_t(32)));
    appendBytes(buf, meta.node_public_key.data(), kPkLen);
    appendBytes(buf, salt.data(), kSaltLen);
    appendBytes(buf, nonce.data(), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    return buf;
}

struct Cursor {
    const uint8_t *p;
    size_t n;
    bool take(void *dst, size_t len) {
        if (n < len) return false;
        if (dst) std::memcpy(dst, p, len);
        p += len;
        n -= len;
        return true;
    }
};

bool parseHeaderPrefix(const uint8_t *data, size_t size, BackupManager::BackupMeta &meta,
                       std::vector<uint8_t> &salt, std::vector<uint8_t> &nonce,
                       size_t &headerLen) {
    Cursor c{data, size};
    uint8_t idLen = 0, nodeIdLen = 0, fwLen = 0;
    if (!c.take(&meta.format_version, 1)) return false;
    if (!c.take(&idLen, 1)) return false;
    std::vector<uint8_t> id(idLen);
    if (!c.take(id.data(), idLen)) return false;
    meta.household_id.assign(id.begin(), id.end());
    if (!c.take(&nodeIdLen, 1)) return false;
    std::vector<uint8_t> nodeId(nodeIdLen);
    if (!c.take(nodeId.data(), nodeIdLen)) return false;
    meta.node_id.assign(nodeId.begin(), nodeId.end());
    if (!c.take(&meta.node_role, 1)) return false;
    if (!c.take(&meta.generation, 1)) return false;
    uint8_t ts[4]{};
    if (!c.take(ts, 4)) return false;
    meta.timestamp = static_cast<uint32_t>(ts[0]) | (static_cast<uint32_t>(ts[1]) << 8) |
                     (static_cast<uint32_t>(ts[2]) << 16) | (static_cast<uint32_t>(ts[3]) << 24);
    if (!c.take(&fwLen, 1)) return false;
    std::vector<uint8_t> fw(fwLen);
    if (!c.take(fw.data(), fwLen)) return false;
    meta.firmware_version.assign(fw.begin(), fw.end());
    meta.node_public_key.resize(kPkLen);
    if (!c.take(meta.node_public_key.data(), kPkLen)) return false;
    salt.resize(kSaltLen);
    if (!c.take(salt.data(), kSaltLen)) return false;
    nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    if (!c.take(nonce.data(), nonce.size())) return false;
    headerLen = static_cast<size_t>(c.p - data);
    return true;
}

} // namespace

BackupManager::BackupManager(HouseholdManager &household, NodeIdentityManager &node,
                             ConfigManager &config, NvsCredentialStore &readerData,
                             AuditManager &audit)
    : m_household(household), m_node(node), m_config(config), m_readerData(readerData),
      m_audit(audit) {}

BackupManager::~BackupManager() = default;

bool BackupManager::begin() {
    return true;
}

std::vector<uint8_t> BackupManager::createBackup() {
    emitBackupEvent(BACKUP_STARTED, true, "");
    const household::HouseholdInfo &hh = m_household.info();
    const household::NodeInfo &node = m_node.info();
    if (hh.household_id.empty() || node.node_id.empty()) {
        ESP_LOGE(TAG, "Household or node identity missing; cannot back up.");
        emitBackupEvent(BACKUP_FAILED, false, "household or node identity missing");
        return {};
    }

    std::vector<uint8_t> key = m_household.deriveBackupKey();
    std::vector<uint8_t> salt = m_household.recoverySalt();
    if (key.empty() || salt.size() != kSaltLen) {
        ESP_LOGE(TAG, "No household recovery secret; cannot encrypt a backup.");
        emitBackupEvent(BACKUP_FAILED, false, "no recovery secret");
        return {};
    }

    // --- Classified plaintext payload (A + B only) ---
    const auto &misc = m_config.getConfig<espConfig::misc_config_t>();
    const auto &mqtt = m_config.getConfig<espConfig::mqttConfig_t>();

    std::string issuersJson;
    const NvsCredentialStore::Snapshot snap = m_readerData.snapshot();
    for (size_t i = 0; i < snap.issuers.size(); ++i) {
        const auto &issuer = snap.issuers[i];
        // Only public issuer identity is exported. Endpoints (incl. persistent
        // keys) and the reader private key are intentionally excluded.
        issuersJson += fmt::format("{{\"id\":\"{}\",\"public_key\":\"{}\"}}{}",
                                   hexEncode(issuer.id), hexEncode(issuer.public_key),
                                   i + 1 < snap.issuers.size() ? "," : "");
    }

    std::string payload = fmt::format(
        "{{\"household\":{{\"household_id\":\"{}\",\"household_name\":\"{}\","
        "\"trust_key\":\"{}\",\"recovery_metadata\":\"{}\",\"config_version\":{}}},"
        "\"node_config\":{{\"device_name\":\"{}\",\"mqtt_broker\":\"{}\",\"mqtt_port\":{},"
        "\"mqtt_client_id\":\"{}\",\"mqtt_username\":\"{}\",\"mqtt_password\":\"{}\","
        "\"mqtt_use_ssl\":{},\"mqtt_allow_insecure\":{},\"mqtt_hass_discovery\":{},"
        "\"web_auth_enabled\":{},\"web_username\":\"{}\",\"web_password\":\"{}\","
        "\"access_point_password\":\"{}\"}},"
        "\"issuers\":[{}]}}",
        jsonEscape(hh.household_id), jsonEscape(hh.household_name),
        hexEncode(hh.trust_public_key), hexEncode(hh.recovery_metadata), hh.config_version,
        jsonEscape(misc.deviceName), jsonEscape(mqtt.mqttBroker), mqtt.mqttPort,
        jsonEscape(mqtt.mqttClientId), jsonEscape(mqtt.mqttUsername), jsonEscape(mqtt.mqttPassword),
        mqtt.useSSL ? "true" : "false", mqtt.allowInsecure ? "true" : "false",
        mqtt.hassMqttDiscoveryEnabled ? "true" : "false",
        misc.webAuthEnabled ? "true" : "false", jsonEscape(misc.webUsername),
        jsonEscape(misc.webPassword), jsonEscape(misc.accessPointPassword), issuersJson);

    // --- Header ---
    BackupMeta meta;
    meta.format_version = kFormatVersion;
    meta.household_id = hh.household_id;
    meta.node_id = node.node_id;
    meta.node_role = static_cast<uint8_t>(node.node_role);
    meta.generation = node.generation;
    meta.timestamp = nowSeconds();
    meta.firmware_version = esp_app_get_description()->version;
    meta.node_public_key = node.public_key;

    std::vector<uint8_t> nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(nonce.data(), nonce.size());

    const std::vector<uint8_t> header = buildHeaderPrefix(meta, salt, nonce);

    // --- Encrypt (AEAD) ---
    std::vector<uint8_t> ciphertext(payload.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long ctLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ciphertext.data(), &ctLen,
            reinterpret_cast<const unsigned char *>(payload.data()), payload.size(),
            header.data(), header.size(), nullptr, nonce.data(), key.data()) != 0) {
        ESP_LOGE(TAG, "Backup encryption failed.");
        emitBackupEvent(BACKUP_FAILED, false, "encryption failed");
        return {};
    }
    ciphertext.resize(ctLen);
    // The plaintext (which contains secrets) and the derived key are no longer
    // needed once the AEAD ciphertext exists.
    sodium_memzero(payload.data(), payload.size());
    sodium_memzero(key.data(), key.size());

    // --- Signature over header || ciphertext (provenance + tamper detection) ---
    std::vector<uint8_t> message;
    message.insert(message.end(), header.begin(), header.end());
    message.insert(message.end(), ciphertext.begin(), ciphertext.end());
    const std::vector<uint8_t> sig = m_node.sign(message);
    if (sig.size() != crypto_sign_BYTES) {
        ESP_LOGE(TAG, "Backup signing failed.");
        emitBackupEvent(BACKUP_FAILED, false, "signing failed");
        return {};
    }

    std::vector<uint8_t> blob;
    blob.insert(blob.end(), message.begin(), message.end());
    blob.insert(blob.end(), sig.begin(), sig.end());

    m_lastBackupTime = meta.timestamp;
    m_lastBackupHash.resize(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(m_lastBackupHash.data(), blob.data(), blob.size());
    m_audit.record(AuditManager::BACKUP_CREATED, AuditManager::SOURCE_LOCAL,
                   AuditManager::RESULT_SUCCESS, node.node_id, "");
    emitBackupEvent(BACKUP_COMPLETED, true, "");
    ESP_LOGI(TAG, "Backup created (%u bytes) for node %s.", static_cast<unsigned>(blob.size()),
             node.node_id.c_str());
    return blob;
}

bool BackupManager::decrypt(const std::vector<uint8_t> &blob,
                            const std::vector<uint8_t> &recoverySecret,
                            std::string &payloadOut, BackupMeta &metaOut) {
    BackupMeta meta;
    std::vector<uint8_t> salt, nonce;
    size_t headerLen = 0;
    if (blob.size() < 100 || !parseHeaderPrefix(blob.data(), blob.size(), meta, salt, nonce, headerLen)) {
        return false;
    }
    if (meta.format_version != kFormatVersion) {
        return false; // unknown or future backup version — fail closed
    }

    const size_t sigLen = crypto_sign_BYTES;
    if (blob.size() < headerLen + crypto_aead_xchacha20poly1305_ietf_ABYTES + sigLen) {
        return false;
    }
    const size_t ctLen = blob.size() - headerLen - sigLen;
    const std::vector<uint8_t> ciphertext(blob.begin() + headerLen, blob.begin() + headerLen + ctLen);
    const std::vector<uint8_t> signature(blob.end() - sigLen, blob.end());

    // Signature verification (fails closed on tampering or wrong sender).
    std::vector<uint8_t> signedPart(blob.begin(), blob.begin() + headerLen + ctLen);
    if (crypto_sign_verify_detached(signature.data(), signedPart.data(), signedPart.size(),
                                    meta.node_public_key.data()) != 0) {
        return false;
    }

    const std::vector<uint8_t> key = deriveKey(recoverySecret, salt);
    std::vector<uint8_t> plaintext(ctLen);
    unsigned long long ptLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &ptLen, nullptr, ciphertext.data(), ciphertext.size(),
            blob.data(), headerLen, nonce.data(), key.data()) != 0) {
        return false; // integrity failure or wrong key
    }
    plaintext.resize(ptLen);
    payloadOut.assign(plaintext.begin(), plaintext.end());
    sodium_memzero(plaintext.data(), plaintext.size());
    meta.recovery_salt = salt;
    metaOut = meta;
    return true;
}
