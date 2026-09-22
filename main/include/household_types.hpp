#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * Shared types for the "HomeKey Household" multi-node model.
 *
 * A household is a group of nodes (gate, main house, garage, …) that share trust
 * and recovery material. A node keeps its own device-specific identity, which is
 * never cloned onto another physical device (see the architecture report).
 */
namespace household {

/// Bumped whenever the persisted household/node schema changes; drives migration.
inline constexpr uint8_t CONFIG_VERSION_CURRENT = 1;

/// Domain-separation label for the backup key derivation (BLAKE2b keyed hash).
inline constexpr char kBackupKeyLabel[] = "HK-HOUSEHOLD-BACKUP-v1";

/// Domain-separation label for the authenticated MQTT command key.
inline constexpr char kCommandKeyLabel[] = "HK-HOUSEHOLD-CMD-v1";

/// Bounded lengths used for anything persisted in the small NVS partition.
inline constexpr size_t kNodeIdMaxLen = 24;
inline constexpr size_t kNameMaxLen = 32;

enum class HouseholdState : uint8_t {
  UNCONFIGURED = 0,
  PROVISIONING = 1,
  ACTIVE = 2,
  RECOVERY_REQUIRED = 3,
  REVOKED = 4,
};

enum class NodeState : uint8_t {
  UNCONFIGURED = 0,
  PROVISIONING = 1,
  ACTIVE = 2,
  REVOKED = 3,
  RECOVERY_REQUIRED = 4,
};

enum class NodeRole : uint8_t {
  GATE = 0,
  MAIN_HOUSE = 1,
  SMALL_HOUSE = 2,
  GARAGE = 3,
  WORKSHOP = 4,
  OTHER = 5,
};

const char *householdStateToString(HouseholdState s);
const char *nodeStateToString(NodeState s);
const char *nodeRoleToString(NodeRole r);
NodeRole nodeRoleFromString(const std::string &s);

/// 3-4 char uppercase prefix used to build human-readable node ids ("GATE-001").
const char *nodeRolePrefix(NodeRole r);

// --- Inline definitions (single translation unit) ---

inline const char *householdStateToString(HouseholdState s) {
    switch (s) {
        case HouseholdState::UNCONFIGURED: return "UNCONFIGURED";
        case HouseholdState::PROVISIONING: return "PROVISIONING";
        case HouseholdState::ACTIVE: return "ACTIVE";
        case HouseholdState::RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
        case HouseholdState::REVOKED: return "REVOKED";
    }
    return "UNKNOWN";
}

inline const char *nodeStateToString(NodeState s) {
    switch (s) {
        case NodeState::UNCONFIGURED: return "UNCONFIGURED";
        case NodeState::PROVISIONING: return "PROVISIONING";
        case NodeState::ACTIVE: return "ACTIVE";
        case NodeState::REVOKED: return "REVOKED";
        case NodeState::RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    }
    return "UNKNOWN";
}

inline const char *nodeRoleToString(NodeRole r) {
    switch (r) {
        case NodeRole::GATE: return "gate";
        case NodeRole::MAIN_HOUSE: return "main_house";
        case NodeRole::SMALL_HOUSE: return "small_house";
        case NodeRole::GARAGE: return "garage";
        case NodeRole::WORKSHOP: return "workshop";
        case NodeRole::OTHER: return "other";
    }
    return "other";
}

inline NodeRole nodeRoleFromString(const std::string &s) {
    if (s == "gate") return NodeRole::GATE;
    if (s == "main_house") return NodeRole::MAIN_HOUSE;
    if (s == "small_house") return NodeRole::SMALL_HOUSE;
    if (s == "garage") return NodeRole::GARAGE;
    if (s == "workshop") return NodeRole::WORKSHOP;
    return NodeRole::OTHER;
}

inline const char *nodeRolePrefix(NodeRole r) {
    switch (r) {
        case NodeRole::GATE: return "GATE";
        case NodeRole::MAIN_HOUSE: return "HOUSE";
        case NodeRole::SMALL_HOUSE: return "SMALLHOUSE";
        case NodeRole::GARAGE: return "GARAGE";
        case NodeRole::WORKSHOP: return "WORKSHOP";
        case NodeRole::OTHER: return "NODE";
    }
    return "NODE";
}

/// Public, serializable household metadata. Never contains private keys.
struct HouseholdInfo {
  std::string household_id;
  std::string household_name;
  HouseholdState state = HouseholdState::UNCONFIGURED;
  uint8_t config_version = CONFIG_VERSION_CURRENT;
  std::vector<uint8_t> trust_public_key;   ///< household trust anchor (public)
  std::vector<uint8_t> recovery_metadata;  ///< salt + KDF/fingerprint info (public)
  bool has_recovery_secret = false;        ///< a recovery secret exists on this device
  bool recovery_exported = false;          ///< the one-time export has happened
};

/// Public, serializable node metadata. The private key is intentionally absent.
struct NodeInfo {
  std::string node_id;                     ///< e.g. "GATE-001"
  std::string node_name;                   ///< e.g. "Gate"
  NodeRole node_role = NodeRole::OTHER;
  NodeState state = NodeState::UNCONFIGURED;
  std::string household_id;                ///< empty when not a household member
  uint8_t generation = 0;                  ///< bumped on replacement (never reused)
  std::vector<uint8_t> public_key;         ///< Ed25519 public key (32 bytes)
  std::vector<uint8_t> cert_fingerprint;   ///< SHA-256(node_id || public_key)
};

} // namespace household
