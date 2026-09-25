#include "HouseholdManager.hpp"
#include "app_event_loop.hpp"
#include "eventStructs.hpp"
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sodium.h>

const char *HouseholdManager::TAG = "Household";

static const char *KEY_HH_META = "HH_META";

// NVS keys may be at most NVS_KEY_NAME_MAX_SIZE - 1 (15) characters, and the obvious names
// for these two are longer: the secret was "HH_RECOVERY_SECRET", 18 characters, so
// nvs_set_blob rejected every write of it and save() could never succeed. The recovery
// secret is what household backups are encrypted with, so the effect was not a missing
// field but a device claiming a stored secret it did not have, and backups that could
// never have been restored. The assertion below is what stops a rename reintroducing it.
static const char *KEY_HH_RECOVERY_SECRET = "HH_REC_SECRET";
static const char *KEY_HH_RECOVERY_SALT = "HH_REC_SALT";

static_assert(sizeof("HH_META") - 1 < NVS_KEY_NAME_MAX_SIZE &&
                  sizeof("HH_REC_SECRET") - 1 < NVS_KEY_NAME_MAX_SIZE &&
                  sizeof("HH_REC_SALT") - 1 < NVS_KEY_NAME_MAX_SIZE,
              "An NVS key longer than 15 characters is rejected at write time.");

namespace {

constexpr size_t kMaxIdLen = household::kNodeIdMaxLen;
constexpr size_t kMaxNameLen = household::kNameMaxLen;
constexpr size_t kMaxTrustLen = 64;
constexpr size_t kMaxRecoveryMetaLen = 64;

struct MetaBlob {
    uint8_t idLen;
    char id[kMaxIdLen];
    uint8_t nameLen;
    char name[kMaxNameLen];
    uint8_t state;
    uint8_t configVersion;
    uint8_t trustLen;
    uint8_t trustKey[kMaxTrustLen];
    uint8_t recoveryMetaLen;
    uint8_t recoveryMeta[kMaxRecoveryMetaLen];
    uint8_t hasRecoverySecret;
    uint8_t recoveryExported;
};

void truncateCopy(char *dst, const std::string &src, size_t max) {
    const size_t n = std::min(src.size(), max);
    std::memcpy(dst, src.data(), n);
    if (n < max) {
        dst[n] = '\0';
    }
}

} // namespace

HouseholdManager::HouseholdManager() = default;

HouseholdManager::~HouseholdManager() {
    if (!m_recoverySecret.empty()) {
        sodium_memzero(m_recoverySecret.data(), m_recoverySecret.size());
    }
    if (!m_salt.empty()) {
        sodium_memzero(m_salt.data(), m_salt.size());
    }
    if (m_initialized && m_handle) {
        nvs_close(m_handle);
        m_handle = 0;
    }
}

bool HouseholdManager::begin() {
    if (m_initialized) {
        return true;
    }
    esp_err_t err = nvs_open("SAVED_DATA", NVS_READWRITE, &m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    m_initialized = true;
    load();
    return true;
}

void HouseholdManager::migrate() {
    size_t size = sizeof(MetaBlob);
    MetaBlob blob{};
    const esp_err_t err = nvs_get_blob(m_handle, KEY_HH_META, &blob, &size);
    if (err == ESP_OK) {
        // Fail closed on a record written by a newer firmware.
        if (blob.configVersion > household::CONFIG_VERSION_CURRENT) {
            ESP_LOGE(TAG, "Unsupported future household config version %u (current %u); "
                          "refusing to migrate.",
                     blob.configVersion, household::CONFIG_VERSION_CURRENT);
            return;
        }
        if (blob.configVersion == 0) {
            // Unstamped/corrupt record: re-initialize the household record without
            // touching any existing config, HomeKey or pairing data.
            ESP_LOGW(TAG, "Household record has no config version; re-initializing.");
            m_info = household::HouseholdInfo{};
            m_info.state = household::HouseholdState::UNCONFIGURED;
            m_info.config_version = household::CONFIG_VERSION_CURRENT;
            save();
            return;
        }
        return; // already migrated / configured
    }
    // First run of this firmware: establish a household record without touching
    // any existing config, HomeKey or pairing data.
    m_info = household::HouseholdInfo{};
    m_info.state = household::HouseholdState::UNCONFIGURED;
    m_info.config_version = household::CONFIG_VERSION_CURRENT;
    save();
    ESP_LOGI(TAG, "Migration: created UNCONFIGURED household record (version %u).",
             household::CONFIG_VERSION_CURRENT);
}

bool HouseholdManager::joinHousehold(const std::string &id, const std::string &name,
                                     const std::vector<uint8_t> &trustKey) {
    m_info.household_id = id;
    m_info.household_name = name;
    if (!trustKey.empty()) {
        m_info.trust_public_key = trustKey;
    }
    m_info.state = household::HouseholdState::PROVISIONING;
    m_info.config_version = household::CONFIG_VERSION_CURRENT;
    ensureRecoverySecret();
    // Reported rather than ignored: this write is what makes the membership survive a
    // reboot, and a household stuck at PROVISIONING is what swallowing the result of it
    // looks like from the outside.
    const bool stored = save();
    if (!stored) {
        ESP_LOGE(TAG, "Household %s was joined in memory but could not be stored; the "
                      "device will not be a member after a reboot.",
                 id.c_str());
    }
    EventHouseholdState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.household_id = m_info.household_id;
    ev.household_name = m_info.household_name;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(HOUSEHOLD_EVENT, HOUSEHOLD_MEMBERSHIP_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Joined household %s (PROVISIONING)", id.c_str());
    return stored;
}

bool HouseholdManager::restoreHousehold(const std::string &id, const std::string &name,
                                        const std::vector<uint8_t> &trustKey,
                                        const std::vector<uint8_t> &recoverySecret,
                                        const std::vector<uint8_t> &recoverySalt) {
    if (recoverySecret.empty() || recoverySalt.empty()) {
        return false;
    }
    m_info.household_id = id;
    m_info.household_name = name;
    if (!trustKey.empty()) {
        m_info.trust_public_key = trustKey;
    }
    m_info.state = household::HouseholdState::ACTIVE;
    m_info.config_version = household::CONFIG_VERSION_CURRENT;
    m_info.has_recovery_secret = true;
    m_info.recovery_exported = true; // the user already has it offline
    m_recoverySecret = recoverySecret;
    m_salt = recoverySalt;
    recomputeRecoveryMetadata();
    save();
    EventHouseholdState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.household_id = m_info.household_id;
    ev.household_name = m_info.household_name;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(HOUSEHOLD_EVENT, HOUSEHOLD_STATE_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Restored household membership %s (ACTIVE).", id.c_str());
    return true;
}

bool HouseholdManager::completeProvisioning() {
    m_info.state = household::HouseholdState::ACTIVE;
    const bool stored = save();
    if (!stored) {
        // The Web UI and /api/ha/state read the in-memory record, so both would keep
        // reporting ACTIVE until the next reboot and PROVISIONING after it. Never
        // announce a state that will not be there then.
        ESP_LOGE(TAG, "Household %s is ACTIVE in memory but could not be stored.",
                 m_info.household_id.c_str());
        return false;
    }
    EventHouseholdState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.household_id = m_info.household_id;
    ev.household_name = m_info.household_name;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(HOUSEHOLD_EVENT, HOUSEHOLD_STATE_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Household %s is ACTIVE.", m_info.household_id.c_str());
    return true;
}

void HouseholdManager::markRecoveryRequired() {
    m_info.state = household::HouseholdState::RECOVERY_REQUIRED;
    save();
}

void HouseholdManager::markRevoked() {
    m_info.state = household::HouseholdState::REVOKED;
    save();
}

void HouseholdManager::markActive() {
    m_info.state = household::HouseholdState::ACTIVE;
    save();
}

bool HouseholdManager::ensureRecoverySecret() {
    if (!m_recoverySecret.empty() && !m_salt.empty()) {
        return true;
    }
    m_recoverySecret.resize(32);
    m_salt.resize(16);
    randombytes_buf(m_recoverySecret.data(), m_recoverySecret.size());
    randombytes_buf(m_salt.data(), m_salt.size());
    m_info.has_recovery_secret = true;
    m_info.recovery_exported = false;
    recomputeRecoveryMetadata();
    // The secret exists only if this write lands. Reporting it as generated without
    // storing it is how a device ends up offering a recovery secret it cannot produce.
    if (!save()) {
        ESP_LOGE(TAG, "Could not store the generated recovery secret; backups made with "
                      "it would not be recoverable.");
        return false;
    }
    ESP_LOGI(TAG, "Generated household recovery secret (export it once, then store offline).");
    return true;
}

bool HouseholdManager::exportRecoverySecretOnce(std::vector<uint8_t> &out) {
    if (m_recoverySecret.empty()) {
        return false;
    }
    if (m_info.recovery_exported) {
        ESP_LOGW(TAG, "Recovery secret was already exported; refusing to re-display it.");
        return false;
    }
    out = m_recoverySecret;
    m_info.recovery_exported = true;
    save();
    ESP_LOGI(TAG, "Recovery secret exported once.");
    return true;
}

std::vector<uint8_t> HouseholdManager::deriveBackupKey() const {
    if (m_recoverySecret.empty() || m_salt.empty()) {
        return {};
    }
    std::vector<uint8_t> material;
    material.insert(material.end(), m_recoverySecret.begin(), m_recoverySecret.end());
    material.insert(material.end(), m_salt.begin(), m_salt.end());

    std::vector<uint8_t> key(32);
    crypto_generichash(key.data(), key.size(), material.data(), material.size(),
                       reinterpret_cast<const unsigned char *>(household::kBackupKeyLabel),
                       sizeof(household::kBackupKeyLabel) - 1);
    return key;
}

std::vector<uint8_t> HouseholdManager::deriveCommandKey() const {
    if (m_recoverySecret.empty() || m_salt.empty()) {
        return {};
    }
    std::vector<uint8_t> material;
    material.insert(material.end(), m_recoverySecret.begin(), m_recoverySecret.end());
    material.insert(material.end(), m_salt.begin(), m_salt.end());

    std::vector<uint8_t> key(32);
    crypto_generichash(key.data(), key.size(), material.data(), material.size(),
                       reinterpret_cast<const unsigned char *>(household::kCommandKeyLabel),
                       sizeof(household::kCommandKeyLabel) - 1);
    return key;
}

void HouseholdManager::recomputeRecoveryMetadata() {
    // Public fingerprint of the backup key: lets a user confirm the correct
    // recovery secret without exposing the key itself.
    const std::vector<uint8_t> key = deriveBackupKey();
    if (key.size() >= 16) {
        m_info.recovery_metadata.assign(key.begin(), key.begin() + 16);
    }
}

bool HouseholdManager::save() {
    if (!m_initialized) {
        return false;
    }
    MetaBlob blob{};
    blob.idLen = static_cast<uint8_t>(std::min(m_info.household_id.size(), kMaxIdLen));
    truncateCopy(blob.id, m_info.household_id, kMaxIdLen);
    blob.nameLen = static_cast<uint8_t>(std::min(m_info.household_name.size(), kMaxNameLen));
    truncateCopy(blob.name, m_info.household_name, kMaxNameLen);
    blob.state = static_cast<uint8_t>(m_info.state);
    blob.configVersion = m_info.config_version;
    blob.trustLen = static_cast<uint8_t>(std::min(m_info.trust_public_key.size(), kMaxTrustLen));
    if (blob.trustLen) {
        std::memcpy(blob.trustKey, m_info.trust_public_key.data(), blob.trustLen);
    }
    blob.recoveryMetaLen = static_cast<uint8_t>(std::min(m_info.recovery_metadata.size(), kMaxRecoveryMetaLen));
    if (blob.recoveryMetaLen) {
        std::memcpy(blob.recoveryMeta, m_info.recovery_metadata.data(), blob.recoveryMetaLen);
    }
    blob.hasRecoverySecret = m_info.has_recovery_secret ? 1 : 0;
    blob.recoveryExported = m_info.recovery_exported ? 1 : 0;

    // The secret and the salt are written before the record that refers to them. The record
    // says whether a recovery secret exists, so writing it first allows a failure to leave
    // behind a device that claims a secret it does not have - which is exactly what the
    // over-long keys above produced once they were rejected.
    esp_err_t err = ESP_OK;
    if (!m_recoverySecret.empty()) {
        err = nvs_set_blob(m_handle, KEY_HH_RECOVERY_SECRET, m_recoverySecret.data(), m_recoverySecret.size());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", KEY_HH_RECOVERY_SECRET, esp_err_to_name(err));
            return false;
        }
    }
    if (!m_salt.empty()) {
        err = nvs_set_blob(m_handle, KEY_HH_RECOVERY_SALT, m_salt.data(), m_salt.size());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", KEY_HH_RECOVERY_SALT, esp_err_to_name(err));
            return false;
        }
    }
    err = nvs_set_blob(m_handle, KEY_HH_META, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(HH_META) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool HouseholdManager::load() {
    size_t size = sizeof(MetaBlob);
    MetaBlob blob{};
    esp_err_t err = nvs_get_blob(m_handle, KEY_HH_META, &blob, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read household meta: %s", esp_err_to_name(err));
        return false;
    }
    m_info.household_id.assign(blob.id, blob.id + std::min<size_t>(blob.idLen, kMaxIdLen));
    m_info.household_name.assign(blob.name, blob.name + std::min<size_t>(blob.nameLen, kMaxNameLen));
    m_info.state = static_cast<household::HouseholdState>(blob.state);
    m_info.config_version = blob.configVersion;
    m_info.trust_public_key.assign(blob.trustKey, blob.trustKey + blob.trustLen);
    m_info.recovery_metadata.assign(blob.recoveryMeta, blob.recoveryMeta + blob.recoveryMetaLen);
    m_info.has_recovery_secret = blob.hasRecoverySecret != 0;
    m_info.recovery_exported = blob.recoveryExported != 0;

    // Fail closed on a record written by a newer firmware whose schema we cannot
    // interpret. The device stays UNCONFIGURED instead of acting on misparsed data.
    if (m_info.config_version > household::CONFIG_VERSION_CURRENT) {
        ESP_LOGE(TAG, "Unsupported future household config version %u (current %u); "
                      "refusing to load household state.",
                 m_info.config_version, household::CONFIG_VERSION_CURRENT);
        m_info = household::HouseholdInfo{};
        return false;
    }

    size_t keySize = 0;
    err = nvs_get_blob(m_handle, KEY_HH_RECOVERY_SECRET, nullptr, &keySize);
    if (err == ESP_OK && keySize > 0) {
        m_recoverySecret.resize(keySize);
        nvs_get_blob(m_handle, KEY_HH_RECOVERY_SECRET, m_recoverySecret.data(), &keySize);
    }
    keySize = 0;
    err = nvs_get_blob(m_handle, KEY_HH_RECOVERY_SALT, nullptr, &keySize);
    if (err == ESP_OK && keySize > 0) {
        m_salt.resize(keySize);
        nvs_get_blob(m_handle, KEY_HH_RECOVERY_SALT, m_salt.data(), &keySize);
    }

    // Reconcile what the record claims with what was actually read back. A device that
    // says it holds a recovery secret it cannot produce is worse than one that admits it
    // holds none, because the user is told their backups are recoverable. Any record left
    // by the over-long key names above is in exactly that state.
    if (m_info.has_recovery_secret && (m_recoverySecret.empty() || m_salt.empty())) {
        ESP_LOGW(TAG, "Household record claims a recovery secret, but none is stored; "
                      "clearing the claim. A new one is made on the next join or export.");
        m_info.has_recovery_secret = false;
        m_info.recovery_exported = false;
        m_info.recovery_metadata.clear();
        save();
    }
    return true;
}
