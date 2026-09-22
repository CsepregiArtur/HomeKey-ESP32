#include "AuditManager.hpp"
#include "app_event_loop.hpp"
#include "eventStructs.hpp"
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <fmt/format.h>
#include <nvs_flash.h>
#include <time.h>

const char *AuditManager::TAG = "Audit";

namespace {

static_assert(sizeof(AuditManager::RawRecord) == 59, "RawRecord layout changed");

uint32_t wallClockSeconds() {
    const time_t now = time(nullptr);
    if (now > 1000000000) {
        return static_cast<uint32_t>(now);
    }
    // No NTP/time source yet: fall back to monotonic uptime seconds.
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

} // namespace

AuditManager::AuditManager() = default;

AuditManager::~AuditManager() {
    if (m_initialized && m_handle) {
        nvs_close(m_handle);
        m_handle = 0;
    }
}

bool AuditManager::begin() {
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

bool AuditManager::load() {
    std::vector<RawRecord> all;
    for (size_t slot = 0; slot < kSlots; ++slot) {
        const std::string key = fmt::format("AUDIT_{:02d}", slot);
        size_t size = 0;
        if (nvs_get_blob(m_handle, key.c_str(), nullptr, &size) != ESP_OK || size == 0) {
            continue;
        }
        std::vector<uint8_t> buf(size);
        if (nvs_get_blob(m_handle, key.c_str(), buf.data(), &size) != ESP_OK) {
            continue;
        }
        const size_t count = size / sizeof(RawRecord);
        for (size_t i = 0; i < count; ++i) {
            RawRecord rec{};
            std::memcpy(&rec, buf.data() + i * sizeof(RawRecord), sizeof(RawRecord));
            if (rec.seq != 0) {
                all.push_back(rec);
            }
        }
    }
    std::sort(all.begin(), all.end(),
              [](const RawRecord &a, const RawRecord &b) { return a.seq < b.seq; });

    m_ring.clear();
    for (const auto &rec : all) {
        m_ring.push_back(rec);
        if (m_ring.size() > kMaxRecords) {
            m_ring.pop_front();
        }
    }
    m_nextSeq = m_ring.empty() ? 0 : m_ring.back().seq;
    m_currentSlot = static_cast<uint8_t>((m_ring.size() / kRecordsPerSlot) % kSlots);
    m_recordsInSlot = static_cast<uint8_t>(m_ring.size() % kRecordsPerSlot);
    ESP_LOGI(TAG, "Loaded %u audit records.", static_cast<unsigned>(m_ring.size()));
    return true;
}

void AuditManager::record(EventType type, Source source, Result result,
                          const std::string &nodeId, const std::string &metadata) {
    if (!m_initialized) {
        return;
    }
    RawRecord rec{};
    rec.seq = ++m_nextSeq;
    rec.timestamp = wallClockSeconds();
    rec.event_type = static_cast<uint8_t>(type);
    rec.source = static_cast<uint8_t>(source);
    rec.result = static_cast<uint8_t>(result);
    const size_t idLen = std::min(nodeId.size(), sizeof(rec.node_id));
    std::memcpy(rec.node_id, nodeId.data(), idLen);
    const size_t metaLen = std::min(metadata.size(), sizeof(rec.metadata));
    std::memcpy(rec.metadata, metadata.data(), metaLen);

    if (m_ring.size() >= kMaxRecords) {
        m_ring.pop_front();
    }
    m_ring.push_back(rec);

    if (m_recordsInSlot >= kRecordsPerSlot) {
        advanceSlot();
    }
    persistCurrentSlot();

    EventAuditRecord ev{};
    ev.timestamp = rec.timestamp;
    ev.event_type = rec.event_type;
    ev.source = rec.source;
    ev.result = rec.result;
    ev.node_id = nodeId;
    ev.metadata = metadata;
    std::vector<uint8_t> buf;
    alpaca::serialize(ev, buf);
    AppEventLoop::publish(AUDIT_EVENT, AUDIT_RECORDED, buf.data(), buf.size());
}

void AuditManager::advanceSlot() {
    m_currentSlot = static_cast<uint8_t>((m_currentSlot + 1) % kSlots);
    m_recordsInSlot = 0;
}

bool AuditManager::persistCurrentSlot() {
    const std::string key = fmt::format("AUDIT_{:02d}", m_currentSlot);
    // Rebuild the current slot from the tail of the ring.
    std::vector<uint8_t> buf(kRecordsPerSlot * sizeof(RawRecord), 0);
    const size_t start = m_ring.size() > m_recordsInSlot ? m_ring.size() - m_recordsInSlot : 0;
    size_t idx = 0;
    for (size_t i = start; i < m_ring.size() && idx < kRecordsPerSlot; ++i, ++idx) {
        std::memcpy(buf.data() + idx * sizeof(RawRecord), &m_ring[i], sizeof(RawRecord));
    }
    const esp_err_t err = nvs_set_blob(m_handle, key.c_str(), buf.data(), buf.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", key.c_str(), esp_err_to_name(err));
        return false;
    }
    return nvs_commit(m_handle) == ESP_OK;
}

std::vector<AuditManager::View> AuditManager::records() const {
    std::vector<View> out;
    out.reserve(m_ring.size());
    for (const auto &rec : m_ring) {
        View v{};
        v.seq = rec.seq;
        v.timestamp = rec.timestamp;
        v.event_type = rec.event_type;
        v.source = rec.source;
        v.result = rec.result;
        v.node_id = std::string(rec.node_id, strnlen(rec.node_id, sizeof(rec.node_id)));
        v.metadata = std::string(rec.metadata, strnlen(rec.metadata, sizeof(rec.metadata)));
        out.push_back(std::move(v));
    }
    return out;
}

std::string AuditManager::toJson(size_t maxEntries) const {
    std::vector<View> recs = records();
    std::string body;
    size_t count = std::min(maxEntries, recs.size());
    for (size_t i = 0; i < count; ++i) {
        const View &v = recs[i];
        body += fmt::format(
            "{{\"seq\":{},\"timestamp\":{},\"event\":\"{}\",\"source\":\"{}\",\"result\":\"{}\","
            "\"node_id\":\"{}\",\"metadata\":\"{}\"}}{}",
            v.seq, v.timestamp, eventTypeName(v.event_type), sourceName(v.source),
            v.result == RESULT_SUCCESS ? "success" : "failure", v.node_id, v.metadata,
            i + 1 < count ? "," : "");
    }
    return fmt::format("{{\"total\":{},\"records\":[{}]}}", recs.size(), body);
}

const char *AuditManager::eventTypeName(uint8_t type) {
    switch (type) {
        case HOMEKEY_AUTH_SUCCESS: return "homekey_auth_success";
        case HOMEKEY_AUTH_FAILURE: return "homekey_auth_failure";
        case LOCK: return "lock";
        case UNLOCK: return "unlock";
        case MQTT_UNLOCK_REQUEST: return "mqtt_unlock_request";
        case WEB_LOGIN_SUCCESS: return "web_login_success";
        case WEB_LOGIN_FAILURE: return "web_login_failure";
        case CONFIG_CHANGE: return "config_change";
        case PROVISIONING: return "provisioning";
        case NODE_ENROLLMENT: return "node_enrollment";
        case NODE_REVOCATION: return "node_revocation";
        case BACKUP_CREATED: return "backup_created";
        case BACKUP_RESTORED: return "backup_restored";
        case OTA_UPDATE: return "ota_update";
        case SECURITY_CONFIG_CHANGE: return "security_config_change";
        default: return "unknown";
    }
}

const char *AuditManager::sourceName(uint8_t source) {
    switch (source) {
        case SOURCE_LOCAL: return "local";
        case SOURCE_HOMEKIT: return "homekit";
        case SOURCE_MQTT: return "mqtt";
        case SOURCE_WEB: return "web";
        case SOURCE_NFC: return "nfc";
        case SOURCE_SYSTEM: return "system";
        default: return "unknown";
    }
}
