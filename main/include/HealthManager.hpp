#pragma once
#include "app_event_loop.hpp"
#include <cstdint>
#include <string>
#include <vector>

class ConfigManager;
class NfcManager;
class MqttManager;
class LockManager;
class SecurityManager;

/**
 * Aggregates a health snapshot: network, MQTT, NFC, lock, backup, certificate,
 * firmware/uptime/heap/reset reason and the security posture. Dynamic values are
 * updated through the event bus; static values are read on demand.
 */
class HealthManager {
public:
    struct Snapshot {
        bool network_ok = false;
        bool mqtt_ok = false;
        uint8_t mqtt_error = 0;
        std::string mqtt_error_message;
        bool nfc_ok = false;
        uint8_t lock_current = 255;
        uint8_t lock_target = 255;
        std::string backup_status = "unknown";
        std::string certificate_status = "unknown";
        std::string firmware_version;
        uint32_t uptime_s = 0;
        uint32_t free_heap = 0;
        std::string reset_reason;
        bool security_all_ok = true;
        std::string security_warnings;
    };

    HealthManager();
    ~HealthManager();

    bool begin();

    void setNfcManager(NfcManager *nfc) { m_nfc = nfc; }
    void setMqttManager(MqttManager *mqtt) { m_mqtt = mqtt; }
    void setLockManager(LockManager *lock) { m_lock = lock; }
    void setSecurityManager(SecurityManager *security) { m_security = security; }

    Snapshot snapshot() const;
    std::string toJson(const Snapshot &snap) const;

private:
    NfcManager *m_nfc = nullptr;
    MqttManager *m_mqtt = nullptr;
    LockManager *m_lock = nullptr;
    SecurityManager *m_security = nullptr;

    bool m_mqttOk = false;
    uint8_t m_mqttError = 0;
    std::string m_mqttErrorMsg;
    bool m_nfcOk = false;
    uint8_t m_lockCurrent = 255;
    uint8_t m_lockTarget = 255;
    std::string m_backupStatus = "unknown";

    AppEventLoop::SubscriptionHandle m_nfcSub;
    AppEventLoop::SubscriptionHandle m_lockSub;
    AppEventLoop::SubscriptionHandle m_backupSub;

    static const char *TAG;
};
