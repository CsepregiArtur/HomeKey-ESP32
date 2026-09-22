#pragma once
#include <nvs.h>
#include <string>
#include <vector>

/**
 * Issues single-use, expiring provisioning tokens for secure node enrollment.
 * The token itself is never stored (only its SHA-256), never logged, and can be
 * consumed exactly once (replay protection). Expiry is wall-clock based via
 * esp_timer / system time where available, falling back to monotonic uptime.
 */
class ProvisioningManager {
public:
    ProvisioningManager();
    ~ProvisioningManager();

    bool begin();

    /// Issue a new one-time code valid for `ttlSeconds`. Returns empty on failure.
    std::string issueCode(uint32_t ttlSeconds);

    /// Consume a code if valid, unexpired and unused. Single-use.
    bool validateAndConsume(const std::string &code);

    bool hasOutstandingCode() const;

private:
    bool load();
    bool persist();

    nvs_handle_t m_handle = 0;
    bool m_initialized = false;
    std::vector<uint8_t> m_codeHash;  ///< SHA-256 of the active code (never the code)
    uint64_t m_expirySeconds = 0;     ///< absolute expiry (seconds)
    bool m_used = true;               ///< true when no outstanding code

    static const char *TAG;
};
