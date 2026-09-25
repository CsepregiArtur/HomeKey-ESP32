#pragma once
#include <cstdint>
#include <nvs.h>
#include <string>
#include <vector>

class HouseholdManager;
class NodeIdentityManager;
class ConfigManager;
class NvsCredentialStore;
class AuditManager;

/**
 * Builds and verifies versioned, authenticated-encrypted household backups.
 *
 * Format: header (plaintext) || XChaCha20-Poly1305(nonce, ciphertext, tag)
 *         || Ed25519 signature over (header || ciphertext).
 * The encryption key is derived from the household recovery secret + a salt
 * (BLAKE2b). Confidentiality, integrity, versioning and tamper detection are all
 * provided by the AEAD + signature; no custom crypto is invented.
 *
 * Classification (see ARCHITECTURE_REPORT.md):
 *   A  household data            — included
 *   B  node configuration        — included (inside the encrypted payload)
 *   C  regenerated node identity — excluded (private key, reader identity)
 *   D  sensitive crypto material — excluded (reader private key, endpoint keys)
 */
class BackupManager {
public:
    struct BackupMeta {
        uint8_t format_version = 0;
        std::string household_id;
        std::string node_id;
        uint8_t node_role = 0;
        uint8_t generation = 0;
        uint32_t timestamp = 0;
        std::string firmware_version;
        std::vector<uint8_t> node_public_key;
        std::vector<uint8_t> recovery_salt;  ///< filled by decrypt()
    };

    BackupManager(HouseholdManager &household, NodeIdentityManager &node,
                  ConfigManager &config, NvsCredentialStore &readerData,
                  AuditManager &audit);
    ~BackupManager();

    bool begin();

    /// Build an encrypted, signed backup blob. Empty on failure.
    ///
    /// With @p includeCredentials the payload also carries the HomeKey credential store and
    /// the HomeKit pairing state: the reader's private key, the enrolled issuers' endpoint
    /// keys, and the accessory identity Apple Home recognises together with the controllers
    /// paired to it. That is what lets a replacement node come back as the same device - no
    /// tags re-enrolled, nothing re-paired - and it is exactly why such a file has to be
    /// kept like a key rather than like a document. Off unless asked for.
    std::vector<uint8_t> createBackup(bool includeCredentials = false);

    /// Decrypt + verify a backup blob with a caller-supplied recovery secret.
    /// Fills `payloadOut` (JSON) and `metaOut`. Fails closed on any error.
    static bool decrypt(const std::vector<uint8_t> &blob,
                        const std::vector<uint8_t> &recoverySecret,
                        std::string &payloadOut, BackupMeta &metaOut);

    uint64_t lastBackupTime() const { return m_lastBackupTime; }
    const std::vector<uint8_t> &lastBackupHash() const { return m_lastBackupHash; }

private:
    HouseholdManager &m_household;
    NodeIdentityManager &m_node;
    ConfigManager &m_config;
    NvsCredentialStore &m_readerData;
    AuditManager &m_audit;

    uint64_t m_lastBackupTime = 0;
    std::vector<uint8_t> m_lastBackupHash;

    static const char *TAG;
};
