#include "HealthManager.hpp"
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
    return fmt::format(
        "{{\"network\":\"{}\",\"mqtt\":\"{}\",\"mqtt_error\":{},\"nfc\":\"{}\","
        "\"lock_current\":{},\"lock_target\":{},\"backup\":\"{}\",\"certificate\":\"{}\","
        "\"firmware_version\":\"{}\",\"uptime\":{},\"free_heap\":{},\"reset_reason\":\"{}\","
        "\"security\":{{\"all_ok\":{},\"warnings\":\"{}\"}}}}",
        snap.network_ok ? "OK" : "UNKNOWN", snap.mqtt_ok ? "OK" : "ERROR", snap.mqtt_error,
        snap.nfc_ok ? "OK" : "ERROR", snap.lock_current, snap.lock_target, snap.backup_status,
        snap.certificate_status, snap.firmware_version, snap.uptime_s, snap.free_heap,
        snap.reset_reason, snap.security_all_ok ? "true" : "false", snap.security_warnings);
}
