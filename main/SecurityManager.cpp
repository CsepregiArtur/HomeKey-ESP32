#include "SecurityManager.hpp"
#include "ConfigManager.hpp"
#include "config.hpp"
#include "defaults.h"
#include <fmt/format.h>
#include <sdkconfig.h>

namespace {
void addFinding(SecurityManager::Posture &p, const std::string &component,
                const std::string &status, const std::string &detail) {
    p.findings.push_back({component, status, detail});
    if (status == "WARNING" || status == "DISABLED") {
        p.all_ok = false;
    }
}
} // namespace

SecurityManager::SecurityManager(const ConfigManager &config) : m_config(config) {}

SecurityManager::Posture SecurityManager::compute() const {
    Posture posture;
    posture.all_ok = true;

    // Compile-time, hardware-bound protections.
#ifdef CONFIG_SECURE_BOOT
    // The original ESP32 only supports Secure Boot V1 (ECDSA-P256).
    addFinding(posture, "secure_boot", "OK", "Secure Boot V1 enabled.");
#else
    addFinding(posture, "secure_boot", "WARNING", "Secure boot disabled.");
#endif

#ifdef CONFIG_SECURE_FLASH_ENC_ENABLED
    addFinding(posture, "flash_encryption", "OK", "Flash encryption enabled.");
#else
    addFinding(posture, "flash_encryption", "WARNING", "Flash encryption disabled.");
#endif

#ifdef CONFIG_NVS_ENCRYPTION
    addFinding(posture, "nvs_encryption", "OK", "NVS encryption enabled.");
#else
    addFinding(posture, "nvs_encryption", "WARNING", "NVS encryption disabled.");
#endif

    const auto &misc = m_config.getConfig<espConfig::misc_config_t>();
    const auto &mqtt = m_config.getConfig<espConfig::mqttConfig_t>();

    if (mqtt.useSSL) {
        addFinding(posture, "mqtt_tls", "OK", "MQTT TLS enabled.");
    } else {
        addFinding(posture, "mqtt_tls", "WARNING",
                   "MQTT TLS disabled; MQTT command topics can unlock the door.");
    }

    if (misc.webHttpsEnabled) {
        addFinding(posture, "https", "OK", "HTTPS web interface enabled.");
    } else {
        addFinding(posture, "https", "WARNING", "HTTPS disabled; web traffic is plain HTTP.");
    }

    if (misc.webAuthEnabled) {
        addFinding(posture, "web_auth", "OK", "Web UI authentication enabled.");
    } else {
        addFinding(posture, "web_auth", "WARNING",
                   "Web UI authentication disabled; anyone on the network can reconfigure the device.");
    }

    return posture;
}

std::string SecurityManager::toJson(const Posture &posture) const {
    std::string findings;
    for (size_t i = 0; i < posture.findings.size(); ++i) {
        const auto &f = posture.findings[i];
        findings += fmt::format(
            "{{\"component\":\"{}\",\"status\":\"{}\",\"detail\":\"{}\"}}{}",
            f.component, f.status, f.detail, i + 1 < posture.findings.size() ? "," : "");
    }
    return fmt::format("{{\"all_ok\":{},\"findings\":[{}]}}", posture.all_ok ? "true" : "false",
                       findings);
}
