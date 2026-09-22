#pragma once
#include <string>
#include <vector>

class ConfigManager;

/**
 * Read-only security posture. Reports named findings (never a numeric "score"):
 * secure boot, flash encryption, OTA signature verification, MQTT TLS, HTTPS,
 * Web UI auth and the HomeSpan OTA password. All findings are derived from
 * compile-time configuration and runtime settings.
 */
class SecurityManager {
public:
    struct Finding {
        std::string component;
        std::string status;  ///< "OK" | "WARNING" | "DISABLED"
        std::string detail;
    };

    struct Posture {
        bool all_ok = true;
        std::vector<Finding> findings;
    };

    explicit SecurityManager(const ConfigManager &config);

    Posture compute() const;
    std::string toJson(const Posture &posture) const;

private:
    const ConfigManager &m_config;
};
