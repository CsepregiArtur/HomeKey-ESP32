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
 * (via BackupManager) and restores household membership + safe node configuration.
 *
 * It also restores the HomeKey credential store and the HomeKit pairing state, but **only
 * from a backup that carries them** - which it does when it was taken with that asked for,
 * because those are the reader's private key and the accessory identity Apple Home trusts.
 * With them, the device comes back as the same device: no tags re-enrolled, nothing
 * re-paired. Without them the older behaviour stands: a new node identity is minted (never
 * a clone) and enrolled devices have to be re-provisioned.
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

    /// True when the last restore wrote device identity or credentials, which only takes
    /// effect after a restart: the reader holds its key material in RAM, and HomeSpan reads
    /// the pairing state at boot.
    bool rebootRequired() const { return m_rebootRequired; }

private:
    HouseholdManager &m_household;
    NodeIdentityManager &m_node;
    ConfigManager &m_config;
    NvsCredentialStore &m_readerData;
    AuditManager &m_audit;

    State m_state = State::IDLE;
    bool m_rebootRequired = false;
    static const char *TAG;
};
