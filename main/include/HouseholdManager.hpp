#pragma once
#include "household_types.hpp"
#include <nvs.h>
#include <string>
#include <vector>

/**
 * Owns household membership: id, name, provisioning state, trust/recovery
 * metadata and the household recovery secret (created once, exported once,
 * never shown afterwards). Backups are encrypted with a key derived from the
 * recovery secret, so membership can be restored on a replacement device.
 */
class HouseholdManager {
public:
    HouseholdManager();
    ~HouseholdManager();

    bool begin();

    /// Idempotent migration: on first run creates an UNCONFIGURED household and
    /// stamps the config version. Never erases NVS or touches HomeKey data.
    void migrate();

    const household::HouseholdInfo &info() const { return m_info; }
    household::HouseholdState state() const { return m_info.state; }

    /// Join a household and write the record before returning.
    ///
    /// @return true when the membership reached NVS. A caller that ignores this can
    ///         report success for a device that will look unconfigured after a reboot.
    bool joinHousehold(const std::string &id, const std::string &name,
                       const std::vector<uint8_t> &trustKey);

    /// Restore path: adopt household membership and the caller-supplied recovery
    /// secret (from offline storage) and go ACTIVE. Does NOT mint a fresh secret.
    bool restoreHousehold(const std::string &id, const std::string &name,
                          const std::vector<uint8_t> &trustKey,
                          const std::vector<uint8_t> &recoverySecret,
                          const std::vector<uint8_t> &recoverySalt);

    /// Flip PROVISIONING to ACTIVE once the node side has been written too.
    /// @return true when the new state reached NVS.
    bool completeProvisioning();
    void markRecoveryRequired();
    void markRevoked();
    void markActive();

    /// Ensure a recovery secret + salt exist (generated once).
    bool ensureRecoverySecret();
    const std::vector<uint8_t> &recoverySalt() const { return m_salt; }

    /// One-time export of the raw recovery secret (offline storage). Returns
    /// false and leaves `out` empty if already exported or none exists.
    bool exportRecoverySecretOnce(std::vector<uint8_t> &out);

    /// 32-byte backup encryption key: BLAKE2b("HK-HOUSEHOLD-BACKUP-v1", secret||salt).
    std::vector<uint8_t> deriveBackupKey() const;

    /// 32-byte HMAC key for authenticated MQTT commands (BLAKE2b, separate label).
    std::vector<uint8_t> deriveCommandKey() const;

private:
    bool load();
    bool save();
    void recomputeRecoveryMetadata();

    nvs_handle_t m_handle = 0;
    bool m_initialized = false;
    household::HouseholdInfo m_info;
    std::vector<uint8_t> m_recoverySecret;  ///< never serialized into status/JSON
    std::vector<uint8_t> m_salt;

    static const char *TAG;
};
