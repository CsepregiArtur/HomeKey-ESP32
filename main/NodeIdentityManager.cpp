#include "NodeIdentityManager.hpp"
#include "app_event_loop.hpp"
#include "eventStructs.hpp"
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <fmt/format.h>
#include <nvs_flash.h>
#include <sodium.h>

const char *NodeIdentityManager::TAG = "NodeIdentity";

// NVS keys (namespace "SAVED_DATA", shared with the rest of the app).
static const char *KEY_NODE_META = "NODE_META";
static const char *KEY_NODE_PRIVKEY = "NODE_PRIVKEY";

namespace {

// Fixed binary layout for NodeInfo. Bounded so it fits a single NVS blob.
struct MetaBlob {
    uint8_t nodeIdLen;
    char nodeId[household::kNodeIdMaxLen];
    uint8_t nameLen;
    char name[household::kNameMaxLen];
    uint8_t role;
    uint8_t state;
    uint8_t generation;
    uint8_t householdLen;
    char household[household::kNodeIdMaxLen];
    uint8_t publicKey[32];
    uint8_t fingerprint[32];
};

void truncateCopy(char *dst, const std::string &src, size_t max) {
    const size_t n = std::min(src.size(), max);
    std::memcpy(dst, src.data(), n);
    if (n < max) {
        dst[n] = '\0';
    }
}

} // namespace

NodeIdentityManager::NodeIdentityManager() = default;

NodeIdentityManager::~NodeIdentityManager() {
    if (!m_privateKey.empty()) {
        sodium_memzero(m_privateKey.data(), m_privateKey.size());
    }
    if (m_initialized && m_handle) {
        nvs_close(m_handle);
        m_handle = 0;
    }
}

bool NodeIdentityManager::begin() {
    if (m_initialized) {
        return true;
    }
    esp_err_t err = nvs_open("SAVED_DATA", NVS_READWRITE, &m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    m_initialized = true;
    if (!load()) {
        ESP_LOGI(TAG, "No node identity present yet.");
    }
    return true;
}

bool NodeIdentityManager::hasIdentity() const {
    return !m_info.node_id.empty() && !m_privateKey.empty();
}

bool NodeIdentityManager::generateKeypair(std::vector<uint8_t> &pk, std::vector<uint8_t> &sk) {
    pk.resize(crypto_sign_PUBLICKEYBYTES);
    sk.resize(crypto_sign_SECRETKEYBYTES);
    return crypto_sign_keypair(pk.data(), sk.data()) == 0;
}

void NodeIdentityManager::recomputeFingerprint() {
    std::vector<uint8_t> material;
    material.insert(material.end(), m_info.node_id.begin(), m_info.node_id.end());
    material.insert(material.end(), m_info.public_key.begin(), m_info.public_key.end());
    uint8_t hash[32];
    crypto_hash_sha256(hash, material.data(), material.size());
    m_info.cert_fingerprint.assign(hash, hash + sizeof(hash));
}

bool NodeIdentityManager::generateIdentity(household::NodeRole role, const std::string &name) {
    if (hasIdentity()) {
        ESP_LOGE(TAG, "Refusing to overwrite an existing node identity.");
        return false;
    }
    std::vector<uint8_t> pk, sk;
    if (!generateKeypair(pk, sk)) {
        ESP_LOGE(TAG, "Ed25519 keypair generation failed.");
        return false;
    }
    m_info.node_role = role;
    m_info.node_name = name;
    m_info.generation = 1;
    m_info.node_id = fmt::format("{}-{:03d}", household::nodeRolePrefix(role), m_info.generation);
    m_info.public_key = std::move(pk);
    m_privateKey = std::move(sk);
    recomputeFingerprint();

    if (!save()) {
        return false;
    }

    EventNodeState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.node_id = m_info.node_id;
    ev.node_name = m_info.node_name;
    ev.household_id = m_info.household_id;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(NODE_EVENT, NODE_IDENTITY_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Generated node identity: %s", m_info.node_id.c_str());
    return true;
}

bool NodeIdentityManager::regenerateIdentity(uint8_t oldGeneration) {
    if (!hasIdentity()) {
        ESP_LOGE(TAG, "No existing identity to replace.");
        return false;
    }
    const household::NodeRole role = m_info.node_role;
    const std::string name = m_info.node_name;
    const std::string householdId = m_info.household_id;
    const household::NodeState state = m_info.state;

    std::vector<uint8_t> pk, sk;
    if (!generateKeypair(pk, sk)) {
        ESP_LOGE(TAG, "Ed25519 keypair generation failed.");
        return false;
    }
    // Never reuse the old id: generation is strictly larger than any seen before.
    m_info.generation = static_cast<uint8_t>(std::max<uint16_t>(oldGeneration, m_info.generation) + 1);
    m_info.node_role = role;
    m_info.node_name = name;
    m_info.household_id = householdId;
    m_info.state = state;
    m_info.node_id = fmt::format("{}-{:03d}", household::nodeRolePrefix(role), m_info.generation);
    m_info.public_key = std::move(pk);
    m_privateKey = std::move(sk);
    recomputeFingerprint();

    if (!save()) {
        return false;
    }
    EventNodeState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.node_id = m_info.node_id;
    ev.node_name = m_info.node_name;
    ev.household_id = m_info.household_id;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(NODE_EVENT, NODE_IDENTITY_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Regenerated node identity: %s (replacement of generation %u)",
             m_info.node_id.c_str(), oldGeneration);
    return true;
}

bool NodeIdentityManager::createReplacementIdentity(household::NodeRole role,
                                                     const std::string &name,
                                                     const std::string &householdId,
                                                     uint8_t oldGeneration) {
    if (hasIdentity()) {
        ESP_LOGE(TAG, "A node identity already exists on this device; refusing to replace "
                       "(a replacement must not clone an identity).");
        return false;
    }
    std::vector<uint8_t> pk, sk;
    if (!generateKeypair(pk, sk)) {
        return false;
    }
    m_info.node_role = role;
    m_info.node_name = name;
    m_info.household_id = householdId;
    m_info.state = household::NodeState::ACTIVE;
    m_info.generation = static_cast<uint8_t>(oldGeneration + 1);
    m_info.node_id = fmt::format("{}-{:03d}", household::nodeRolePrefix(role), m_info.generation);
    m_info.public_key = std::move(pk);
    m_privateKey = std::move(sk);
    recomputeFingerprint();
    if (!save()) {
        return false;
    }
    EventNodeState ev{};
    ev.state = static_cast<uint8_t>(m_info.state);
    ev.node_id = m_info.node_id;
    ev.node_name = m_info.node_name;
    ev.household_id = m_info.household_id;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(NODE_EVENT, NODE_IDENTITY_CHANGED, buf.data(), buf.size());
    ESP_LOGI(TAG, "Created replacement node identity: %s", m_info.node_id.c_str());
    return true;
}

void NodeIdentityManager::setHousehold(const std::string &householdId) {
    m_info.household_id = householdId;
    save();
}

void NodeIdentityManager::setState(household::NodeState state) {
    if (m_info.state == state) {
        return;
    }
    m_info.state = state;
    save();
    EventNodeState ev{};
    ev.state = static_cast<uint8_t>(state);
    ev.node_id = m_info.node_id;
    ev.node_name = m_info.node_name;
    ev.household_id = m_info.household_id;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(NODE_EVENT, NODE_STATE_CHANGED, buf.data(), buf.size());
}

void NodeIdentityManager::setRole(household::NodeRole role) {
    m_info.node_role = role;
    save();
}

std::vector<uint8_t> NodeIdentityManager::sign(const std::vector<uint8_t> &message) const {
    if (m_privateKey.empty()) {
        return {};
    }
    std::vector<uint8_t> sig(crypto_sign_BYTES);
    unsigned long long sigLen = 0;
    if (crypto_sign_detached(sig.data(), &sigLen, message.data(), message.size(),
                             m_privateKey.data()) != 0) {
        return {};
    }
    sig.resize(sigLen);
    return sig;
}

bool NodeIdentityManager::save() {
    if (!m_initialized) {
        return false;
    }
    MetaBlob blob{};
    blob.nodeIdLen = static_cast<uint8_t>(std::min(m_info.node_id.size(), household::kNodeIdMaxLen));
    truncateCopy(blob.nodeId, m_info.node_id, household::kNodeIdMaxLen);
    blob.nameLen = static_cast<uint8_t>(std::min(m_info.node_name.size(), household::kNameMaxLen));
    truncateCopy(blob.name, m_info.node_name, household::kNameMaxLen);
    blob.role = static_cast<uint8_t>(m_info.node_role);
    blob.state = static_cast<uint8_t>(m_info.state);
    blob.generation = m_info.generation;
    blob.householdLen = static_cast<uint8_t>(std::min(m_info.household_id.size(), household::kNodeIdMaxLen));
    truncateCopy(blob.household, m_info.household_id, household::kNodeIdMaxLen);
    if (m_info.public_key.size() >= 32) {
        std::memcpy(blob.publicKey, m_info.public_key.data(), 32);
    }
    if (m_info.cert_fingerprint.size() >= 32) {
        std::memcpy(blob.fingerprint, m_info.cert_fingerprint.data(), 32);
    }

    esp_err_t err = nvs_set_blob(m_handle, KEY_NODE_META, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(NODE_META) failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!m_privateKey.empty()) {
        err = nvs_set_blob(m_handle, KEY_NODE_PRIVKEY, m_privateKey.data(), m_privateKey.size());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_blob(NODE_PRIVKEY) failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    err = nvs_commit(m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NodeIdentityManager::load() {
    size_t size = sizeof(MetaBlob);
    MetaBlob blob{};
    esp_err_t err = nvs_get_blob(m_handle, KEY_NODE_META, &blob, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK || size < sizeof(uint8_t) * 6) {
        ESP_LOGW(TAG, "Could not read node meta: %s", esp_err_to_name(err));
        return false;
    }

    m_info.node_id.assign(blob.nodeId, blob.nodeId + std::min<size_t>(blob.nodeIdLen, household::kNodeIdMaxLen));
    m_info.node_name.assign(blob.name, blob.name + std::min<size_t>(blob.nameLen, household::kNameMaxLen));
    m_info.node_role = static_cast<household::NodeRole>(blob.role);
    m_info.state = static_cast<household::NodeState>(blob.state);
    m_info.generation = blob.generation;
    m_info.household_id.assign(blob.household, blob.household + std::min<size_t>(blob.householdLen, household::kNodeIdMaxLen));
    m_info.public_key.assign(blob.publicKey, blob.publicKey + 32);
    m_info.cert_fingerprint.assign(blob.fingerprint, blob.fingerprint + 32);

    size_t keySize = 0;
    err = nvs_get_blob(m_handle, KEY_NODE_PRIVKEY, nullptr, &keySize);
    if (err == ESP_OK && keySize > 0) {
        m_privateKey.resize(keySize);
        err = nvs_get_blob(m_handle, KEY_NODE_PRIVKEY, m_privateKey.data(), &keySize);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read node private key: %s", esp_err_to_name(err));
            m_privateKey.clear();
        }
    }
    return true;
}
