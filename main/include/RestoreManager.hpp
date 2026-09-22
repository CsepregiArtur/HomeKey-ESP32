#pragma once
#include <cstdint>
#include <string>
#include <vector>

class HouseholdManager;
class NodeIdentityManager;
class ConfigManager;
class NvsCredentialStore;
class AuditManager;

/**
 * Drives the replacement-node restore workflow. It decrypts/verifies a backup
 * (via BackupManager), restores household membership + safe node configuration,
 * mints a NEW node identity (never a clone) and explicitly leaves the HomeKey
 * reader identity + endpoint keys un-restored (those are re-provisioned).
 */
class RestoreManager {
public:
    enum class State : uint8_t { IDLE = 0, IN_PROGRESS = 1, COMPLETED = 2, FAILED = 3 };

    RestoreManager(HouseholdManager &household, NodeIdentityManager &node,
                   ConfigManager &config, NvsCredentialStore &readerData,
                   AuditManager &audit);
    ~RestoreManager();

    bool begin();
    State state() const { return m_state; }

    /// Decrypt, verify and apply a backup. Fails closed; fills `errorOut` on failure.
    bool restore(const std::vector<uint8_t> &encryptedBlob,
                 const std::vector<uint8_t> &recoverySecret, std::string &errorOut);

private:
    HouseholdManager &m_household;
    NodeIdentityManager &m_node;
    ConfigManager &m_config;
    NvsCredentialStore &m_readerData;
    AuditManager &m_audit;

    State m_state = State::IDLE;
    static const char *TAG;
};
