#include "ProvisioningManager.hpp"
#include "app_event_loop.hpp"
#include "eventStructs.hpp"
#include <array>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <sodium.h>

const char *ProvisioningManager::TAG = "Provisioning";

static const char *KEY_PROV_HASH = "PROV_HASH";
static const char *KEY_PROV_EXPIRY = "PROV_EXPIRY";
static const char *KEY_PROV_USED = "PROV_USED";

namespace {
// Ambiguous glyphs removed: codes are read by hand.
constexpr char kCodeAlphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr size_t kAlphabetSize = sizeof(kCodeAlphabet) - 1;
constexpr size_t kCodeLength = 8;
} // namespace

ProvisioningManager::ProvisioningManager() = default;

ProvisioningManager::~ProvisioningManager() {
    if (!m_codeHash.empty()) {
        sodium_memzero(m_codeHash.data(), m_codeHash.size());
    }
    if (m_initialized && m_handle) {
        nvs_close(m_handle);
        m_handle = 0;
    }
}

bool ProvisioningManager::begin() {
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

std::string ProvisioningManager::issueCode(uint32_t ttlSeconds) {
    std::string code(kCodeLength, '\0');
    std::array<uint8_t, 32> buffer{};
    size_t written = 0;
    while (written < kCodeLength) {
        randombytes_buf(buffer.data(), buffer.size());
        for (uint8_t byte : buffer) {
            if (byte >= 256 - (256 % kAlphabetSize)) {
                continue;
            }
            code[written++] = kCodeAlphabet[byte % kAlphabetSize];
            if (written == kCodeLength) {
                break;
            }
        }
    }

    m_codeHash.resize(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(m_codeHash.data(),
                       reinterpret_cast<const unsigned char *>(code.data()), code.size());
    const uint64_t nowSeconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    m_expirySeconds = nowSeconds + ttlSeconds;
    m_used = false;

    if (!persist()) {
        return {};
    }

    EventProvisionState ev{};
    ev.state = 0;
    ev.success = true;
    ev.message = "";
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(PROVISION_EVENT, PROVISION_CODE_ISSUED, buf.data(), buf.size());

    // Deliberately not logged: a provisioning token is a secret.
    return code;
}

bool ProvisioningManager::validateAndConsume(const std::string &code) {
    if (m_used || m_codeHash.empty()) {
        return false;
    }
    const uint64_t nowSeconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    if (nowSeconds > m_expirySeconds) {
        ESP_LOGW(TAG, "Provisioning code expired.");
        m_used = true;
        persist();
        return false;
    }

    uint8_t hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, reinterpret_cast<const unsigned char *>(code.data()), code.size());
    if (sodium_memcmp(hash, m_codeHash.data(), crypto_hash_sha256_BYTES) != 0) {
        return false;
    }

    // Single use: consume before any further processing (replay protection).
    m_used = true;
    persist();
    return true;
}

bool ProvisioningManager::hasOutstandingCode() const {
    const uint64_t nowSeconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    return !m_used && !m_codeHash.empty() && nowSeconds <= m_expirySeconds;
}

bool ProvisioningManager::persist() {
    if (!m_initialized) {
        return false;
    }
    esp_err_t err = ESP_OK;
    if (!m_codeHash.empty()) {
        err = nvs_set_blob(m_handle, KEY_PROV_HASH, m_codeHash.data(), m_codeHash.size());
    } else {
        err = nvs_erase_key(m_handle, KEY_PROV_HASH);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_set_blob(PROV_HASH) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u64(m_handle, KEY_PROV_EXPIRY, m_expirySeconds);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_u8(m_handle, KEY_PROV_USED, m_used ? 1 : 0);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_commit(m_handle);
    return err == ESP_OK;
}

bool ProvisioningManager::load() {
    uint8_t used = 1;
    if (nvs_get_u8(m_handle, KEY_PROV_USED, &used) != ESP_OK) {
        m_used = true;
        return false;
    }
    m_used = used != 0;

    if (nvs_get_u64(m_handle, KEY_PROV_EXPIRY, &m_expirySeconds) != ESP_OK) {
        m_expirySeconds = 0;
    }

    size_t size = 0;
    esp_err_t err = nvs_get_blob(m_handle, KEY_PROV_HASH, nullptr, &size);
    if (err == ESP_OK && size > 0) {
        m_codeHash.resize(size);
        nvs_get_blob(m_handle, KEY_PROV_HASH, m_codeHash.data(), &size);
    }
    return true;
}
