#pragma once
#include "household_types.hpp"
#include <nvs.h>
#include <string>
#include <vector>

/**
 * Owns the device-specific node identity: node_id, Ed25519 keypair and a
 * certificate fingerprint placeholder. The private key never leaves the device.
 *
 * Invariant enforced here: an identity is generated exactly once, and a
 * replacement/restore operation always mints a *new* identity (never a clone).
 */
class NodeIdentityManager {
public:
    NodeIdentityManager();
    ~NodeIdentityManager();

    bool begin();
    bool hasIdentity() const;
    const household::NodeInfo &info() const { return m_info; }

    /// Mint a brand-new identity for a role/name. Fails if one already exists.
    bool generateIdentity(household::NodeRole role, const std::string &name);

    /// Replacement path: same role/name, but a NEW node_id and keypair. The old
    /// generation is taken from the backup so the new id is "GATE-002", never
    /// "GATE-001" again.
    bool regenerateIdentity(uint8_t oldGeneration);

    /// Restore/replacement entry point: mint a fresh identity for a replacement
    /// device. Refuses to run if a different identity already exists (would be a
    /// clone). Generation becomes oldGeneration + 1.
    bool createReplacementIdentity(household::NodeRole role, const std::string &name,
                                   const std::string &householdId, uint8_t oldGeneration);

    /// Each of these writes NVS immediately and reports whether that write landed, so
    /// a caller doing several in a row cannot report success for a node whose identity
    /// never reached storage.
    bool setHousehold(const std::string &householdId);
    bool setState(household::NodeState state);
    bool setRole(household::NodeRole role);

    /// Ed25519-detached signature over `message`, using the device private key.
    std::vector<uint8_t> sign(const std::vector<uint8_t> &message) const;

private:
    bool load();
    bool save();
    bool generateKeypair(std::vector<uint8_t> &pk, std::vector<uint8_t> &sk);
    void recomputeFingerprint();

    nvs_handle_t m_handle = 0;
    bool m_initialized = false;
    household::NodeInfo m_info;
    std::vector<uint8_t> m_privateKey;  ///< never serialized into status/JSON

    static const char *TAG;
};
