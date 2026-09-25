#include "HealthManager.hpp"
#include "JsonGuard.hpp"
#include "LockManager.hpp"
#include "NfcManager.hpp"
#include "MqttManager.hpp"
#include "SecurityManager.hpp"
#include "eventStructs.hpp"
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <fmt/format.h>
#include <span>
#include <system_error>

const char *HealthManager::TAG = "Health";

HealthManager::HealthManager() = default;
HealthManager::~HealthManager() = default;

bool HealthManager::begin() {
    // MQTT status is read live from MqttManager at snapshot time (the status event
    // carries an enum that alpaca does not round-trip reliably).
    m_nfcSub = AppEventLoop::subscribe(NFC_EVENT, NFC_STATUS_CHANGED,
        [this](const uint8_t *data, size_t size) {
            if (!data || size == 0) return;
            std::span<const uint8_t> payload(data, size);
            std::error_code ec;
            const EventNfcStatus ev = alpaca::deserialize<EventNfcStatus>(payload, ec);
            if (ec) return;
            m_nfcOk = ev.connected;
        });
    m_lockSub = AppEventLoop::subscribe(LOCK_EVENT, LOCK_STATE_CHANGED,
        [this](const uint8_t *data, size_t size) {
            if (!data || size == 0) return;
            std::span<const uint8_t> payload(data, size);
            std::error_code ec;
            const EventLockState ev = alpaca::deserialize<EventLockState>(payload, ec);
            if (ec) return;
            m_lockCurrent = ev.currentState;
            m_lockTarget = ev.targetState;
        });
    m_backupSub = AppEventLoop::subscribe(BACKUP_EVENT, BACKUP_COMPLETED,
        [this](const uint8_t *data, size_t size) {
            if (!data || size == 0) return;
            std::span<const uint8_t> payload(data, size);
            std::error_code ec;
            const EventBackupStatus ev = alpaca::deserialize<EventBackupStatus>(payload, ec);
            if (ec) return;
            m_backupStatus = ev.success ? "ok" : "failed";
        });
    return true;
}

HealthManager::Snapshot HealthManager::snapshot() const {
    Snapshot snap;
    snap.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    snap.free_heap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    snap.firmware_version = esp_app_get_description()->version;
    snap.reset_reason = fmt::format("{}", static_cast<int>(esp_reset_reason()));

    snap.mqtt_ok = m_mqtt ? m_mqtt->isConnected() : m_mqttOk;
    snap.mqtt_error = m_mqtt ? static_cast<uint8_t>(m_mqtt->getLastErrorCode()) : m_mqttError;
    snap.mqtt_error_message = m_mqtt ? m_mqtt->getLastErrorMessage() : m_mqttErrorMsg;
    snap.nfc_ok = m_nfc ? m_nfc->isConnected() : false;
    if (m_lock) {
        snap.lock_current = m_lock->getCurrentState();
        snap.lock_target = m_lock->getTargetState();
    }
    snap.backup_status = m_backupStatus;
    if (m_security) {
        const SecurityManager::Posture posture = m_security->compute();
        snap.security_all_ok = posture.all_ok;
        for (const auto &f : posture.findings) {
            if (f.status != "OK") {
                if (!snap.security_warnings.empty()) {
                    snap.security_warnings += "\n";
                }
                snap.security_warnings += f.component + ": " + f.detail;
            }
        }
    }
    return snap;
}

std::string HealthManager::toJson(const Snapshot &snap) const {
    // Built with cJSON rather than fmt::format, because this document interpolates free text
    // into a JSON string and hand-formatting cannot escape it. security_warnings is the case
    // that bit: findings are joined with real newlines, a raw newline is not legal inside a
    // JSON string, and so the published document was invalid JSON on any device with more
    // than one thing to warn about. This is the only place the lock's state is reported, so
    // a strict reader - the Home Assistant integration - discarded every payload and the
    // lock entity never received a state from it, while everything else about the node
    // looked healthy. cJSON escapes what needs escaping, so the next free-text field added
    // here is safe by construction.
    JsonGuard root(cJSON_CreateObject());
    if (!root.get()) {
        return "{}";
    }

    cJSON_AddStringToObject(root.get(), "network", snap.network_ok ? "OK" : "UNKNOWN");
    cJSON_AddStringToObject(root.get(), "mqtt", snap.mqtt_ok ? "OK" : "ERROR");
    cJSON_AddNumberToObject(root.get(), "mqtt_error", snap.mqtt_error);
    cJSON_AddStringToObject(root.get(), "nfc", snap.nfc_ok ? "OK" : "ERROR");
    cJSON_AddNumberToObject(root.get(), "lock_current", snap.lock_current);
    cJSON_AddNumberToObject(root.get(), "lock_target", snap.lock_target);
    cJSON_AddStringToObject(root.get(), "backup", snap.backup_status.c_str());
    cJSON_AddStringToObject(root.get(), "certificate", snap.certificate_status.c_str());
    cJSON_AddStringToObject(root.get(), "firmware_version", snap.firmware_version.c_str());
    cJSON_AddNumberToObject(root.get(), "uptime", snap.uptime_s);
    cJSON_AddNumberToObject(root.get(), "free_heap", snap.free_heap);
    cJSON_AddStringToObject(root.get(), "reset_reason", snap.reset_reason.c_str());

    JsonGuard security(cJSON_CreateObject());
    if (!security.get()) {
        return "{}";
    }
    cJSON_AddBoolToObject(security.get(), "all_ok", snap.security_all_ok);
    cJSON_AddStringToObject(security.get(), "warnings", snap.security_warnings.c_str());
    cJSON_AddItemToObject(root.get(), "security", security.release());

    char *printed = cJSON_PrintUnformatted(root.get());
    if (!printed) {
        return "{}";
    }
    const std::string json(printed);
    cJSON_free(printed);
    return json;
}
