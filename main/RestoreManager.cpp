#include "RestoreManager.hpp"
#include "AuditManager.hpp"
#include "BackupManager.hpp"
#include "ConfigManager.hpp"
#include "HouseholdManager.hpp"
#include "NodeIdentityManager.hpp"
#include "ReaderDataManager.hpp"
#include "app_event_loop.hpp"
#include "config.hpp"
#include "eventStructs.hpp"
#include "household_types.hpp"
#include <cJSON.h>
#include <esp_log.h>
#include <fmt/format.h>
#include <nvs.h>

const char *RestoreManager::TAG = "Restore";

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hexDecode(const std::string &in) {
    std::vector<uint8_t> out;
    if (in.size() % 2 != 0) return out;
    for (size_t i = 0; i < in.size(); i += 2) {
        const int hi = hexDigit(in[i]);
        const int lo = hexDigit(in[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string getString(const cJSON *obj, const char *key, const std::string &fallback = "") {
    if (!obj) return fallback;
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(node) && node->valuestring) return std::string(node->valuestring);
    return fallback;
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

} // namespace

RestoreManager::RestoreManager(HouseholdManager &household, NodeIdentityManager &node,
                               ConfigManager &config, NvsCredentialStore &readerData,
                               AuditManager &audit)
    : m_household(household), m_node(node), m_config(config), m_readerData(readerData),
      m_audit(audit) {}

RestoreManager::~RestoreManager() = default;

bool RestoreManager::begin() {
    return true;
}

bool RestoreManager::restore(const std::vector<uint8_t> &encryptedBlob,
                             const std::vector<uint8_t> &recoverySecret,
                             std::string &errorOut) {
    m_state = State::IN_PROGRESS;
    emitBackupEvent(RESTORE_STARTED, true, "");

    BackupManager::BackupMeta meta;
    std::string payload;
    if (!BackupManager::decrypt(encryptedBlob, recoverySecret, payload, meta)) {
        m_state = State::FAILED;
        errorOut = "Backup decryption, signature or integrity check failed.";
        emitBackupEvent(RESTORE_FAILED, false, errorOut);
        m_audit.record(AuditManager::BACKUP_RESTORED, AuditManager::SOURCE_LOCAL,
                       AuditManager::RESULT_FAILURE, "", "integrity");
        return false;
    }

    cJSON *root = cJSON_Parse(payload.c_str());
    if (!root) {
        m_state = State::FAILED;
        errorOut = "Backup payload is not valid JSON.";
        emitBackupEvent(RESTORE_FAILED, false, errorOut);
        return false;
    }
    const cJSON *householdNode = cJSON_GetObjectItemCaseSensitive(root, "household");
    const cJSON *configNode = cJSON_GetObjectItemCaseSensitive(root, "node_config");
    const cJSON *issuersNode = cJSON_GetObjectItemCaseSensitive(root, "issuers");

    const std::string householdName = getString(householdNode, "household_name", "Household");
    const std::vector<uint8_t> trustKey = hexDecode(getString(householdNode, "trust_key"));

    // 1. Household membership + the user-supplied recovery secret (from offline storage).
    if (!m_household.restoreHousehold(meta.household_id, householdName, trustKey,
                                      recoverySecret, meta.recovery_salt)) {
        cJSON_Delete(root);
        m_state = State::FAILED;
        errorOut = "Could not restore household membership.";
        emitBackupEvent(RESTORE_FAILED, false, errorOut);
        return false;
    }

    // 2. NEW node identity (never a clone of the old node).
    const household::NodeRole role = static_cast<household::NodeRole>(meta.node_role);
    const std::string nodeName = getString(configNode, "device_name", "Node");
    if (!m_node.createReplacementIdentity(role, nodeName, meta.household_id, meta.generation)) {
        cJSON_Delete(root);
        m_state = State::FAILED;
        errorOut = "Could not create a replacement node identity (a device identity already exists?).";
        emitBackupEvent(RESTORE_FAILED, false, errorOut);
        return false;
    }

    // 3. Restore safe node configuration (secrets are inside the AEAD payload).
    if (configNode) {
        m_config.updateFromJson<espConfig::mqttConfig_t>(
            fmt::format("{{\"mqttBroker\":\"{}\",\"mqttPort\":{},\"mqttClientId\":\"{}\","
                        "\"mqttUsername\":\"{}\",\"mqttPassword\":\"{}\",\"useSSL\":{},\"allowInsecure\":{},"
                        "\"hassMqttDiscoveryEnabled\":{}}}",
                        getString(configNode, "mqtt_broker"),
                        cJSON_GetObjectItemCaseSensitive(configNode, "mqtt_port") ? cJSON_GetObjectItemCaseSensitive(configNode, "mqtt_port")->valueint : 1883,
                        getString(configNode, "mqtt_client_id"),
                        getString(configNode, "mqtt_username"),
                        getString(configNode, "mqtt_password"),
                        getString(configNode, "mqtt_use_ssl") == "true" ? "true" : "false",
                        getString(configNode, "mqtt_allow_insecure") == "true" ? "true" : "false",
                        getString(configNode, "mqtt_hass_discovery") == "true" ? "true" : "false"));
        m_config.saveConfig<espConfig::mqttConfig_t>();

        m_config.updateFromJson<espConfig::misc_config_t>(fmt::format(
            "{{\"deviceName\":\"{}\",\"webAuthEnabled\":{},\"webUsername\":\"{}\","
            "\"webPassword\":\"{}\",\"accessPointPassword\":\"{}\"}}",
            getString(configNode, "device_name"),
            getString(configNode, "web_auth_enabled") == "true" ? "true" : "false",
            getString(configNode, "web_username"),
            getString(configNode, "web_password"),
            getString(configNode, "access_point_password")));
        m_config.saveConfig<espConfig::misc_config_t>();
    }

    // 4. Restore public HomeKey issuer keys. The reader private key and endpoint
    //    persistent keys are intentionally absent: a replacement device re-issues
    //    its reader identity, and enrolled devices must be re-provisioned.
    if (cJSON_IsArray(issuersNode)) {
        const cJSON *issuer = nullptr;
        cJSON_ArrayForEach(issuer, issuersNode) {
            const std::vector<uint8_t> id = hexDecode(getString(issuer, "id"));
            const std::vector<uint8_t> pub = hexDecode(getString(issuer, "public_key"));
            if (id.size() > 0 && pub.size() >= 32) {
                m_readerData.addIssuerIfNotExists(id, pub.data());
            }
        }
        m_readerData.save();
    }

    // 5. Device identity and credentials, when the backup carries them.
    //
    // The credential store is the reader's private key and the enrolled issuers' endpoint
    // keys; the HAP namespace is the accessory identity Apple Home recognises together with
    // the controllers paired to it. Writing them back is what makes this a replacement of
    // the *device* rather than of its membership - which is also why such a backup deserves
    // to be kept like a key. A backup without them restores exactly as it did before.
    bool restoredCredentials = false;
    const cJSON *credentialsNode = cJSON_GetObjectItemCaseSensitive(root, "credentials");
    if (cJSON_IsObject(credentialsNode)) {
        const std::vector<uint8_t> readerStore =
            hexDecode(getString(credentialsNode, "reader_store"));
        if (!readerStore.empty() && m_readerData.importRaw(readerStore)) {
            restoredCredentials = true;
        } else if (!readerStore.empty()) {
            ESP_LOGW(TAG, "Backup carries a credential store that could not be applied.");
        }

        const cJSON *hapNode = cJSON_GetObjectItemCaseSensitive(credentialsNode, "hap");
        if (cJSON_IsObject(hapNode)) {
            nvs_handle_t hap{};
            if (nvs_open("HAP", NVS_READWRITE, &hap) == ESP_OK) {
                const std::vector<uint8_t> accessory = hexDecode(getString(hapNode, "accessory"));
                const std::vector<uint8_t> hapHash = hexDecode(getString(hapNode, "hap_hash"));
                const std::string setupId = getString(hapNode, "setup_id");
                bool wrote = false;
                if (!accessory.empty() &&
                    nvs_set_blob(hap, "ACCESSORY", accessory.data(), accessory.size()) == ESP_OK) {
                    wrote = true;
                }
                if (!hapHash.empty() &&
                    nvs_set_blob(hap, "HAPHASH", hapHash.data(), hapHash.size()) == ESP_OK) {
                    wrote = true;
                }
                if (!setupId.empty() && nvs_set_str(hap, "SETUPID", setupId.c_str()) == ESP_OK) {
                    wrote = true;
                }
                if (wrote) {
                    nvs_commit(hap);
                    restoredCredentials = true;
                } else {
                    ESP_LOGW(TAG, "Backup carries pairing state, but nothing could be written.");
                }
                nvs_close(hap);
            } else {
                ESP_LOGW(TAG, "Could not open the HAP namespace to restore pairing state.");
            }
        }
    }

    cJSON_Delete(root);

    m_state = State::COMPLETED;
    m_rebootRequired = restoredCredentials;
    emitBackupEvent(RESTORE_COMPLETED, true, m_node.info().node_id);
    m_audit.record(AuditManager::BACKUP_RESTORED, AuditManager::SOURCE_LOCAL,
                   AuditManager::RESULT_SUCCESS, m_node.info().node_id, "");
    ESP_LOGI(TAG, "Restore complete. Node: %s%s", m_node.info().node_id.c_str(),
             restoredCredentials ? " (device identity and credentials restored)" : "");
    return true;
}
