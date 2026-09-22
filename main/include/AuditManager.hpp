#pragma once
#include <cstdint>
#include <deque>
#include <nvs.h>
#include <string>
#include <vector>

/**
 * Bounded, NVS-backed security audit log. Records are fixed-size and stored in
 * a small number of NVS slots (ring), so flash usage is strictly bounded.
 * Secrets must never be passed as metadata.
 */
class AuditManager {
public:
    enum EventType : uint8_t {
        HOMEKEY_AUTH_SUCCESS = 0,
        HOMEKEY_AUTH_FAILURE = 1,
        LOCK = 2,
        UNLOCK = 3,
        MQTT_UNLOCK_REQUEST = 4,
        WEB_LOGIN_SUCCESS = 5,
        WEB_LOGIN_FAILURE = 6,
        CONFIG_CHANGE = 7,
        PROVISIONING = 8,
        NODE_ENROLLMENT = 9,
        NODE_REVOCATION = 10,
        BACKUP_CREATED = 11,
        BACKUP_RESTORED = 12,
        OTA_UPDATE = 13,
        SECURITY_CONFIG_CHANGE = 14,
        MAX_EVENT
    };

    enum Source : uint8_t {
        SOURCE_LOCAL = 0,
        SOURCE_HOMEKIT = 1,
        SOURCE_MQTT = 2,
        SOURCE_WEB = 3,
        SOURCE_NFC = 4,
        SOURCE_SYSTEM = 5,
    };

    enum Result : uint8_t {
        RESULT_SUCCESS = 0,
        RESULT_FAILURE = 1,
    };

    struct View {
        uint32_t seq;
        uint32_t timestamp;
        uint8_t event_type;
        uint8_t source;
        uint8_t result;
        std::string node_id;
        std::string metadata;
    };

    AuditManager();
    ~AuditManager();

    bool begin();

    /// Append a record. `metadata` is bounded and must not contain secrets.
    void record(EventType type, Source source, Result result,
                const std::string &nodeId, const std::string &metadata = "");

    std::vector<View> records() const;
    std::string toJson(size_t maxEntries) const;

    static const char *eventTypeName(uint8_t type);
    static const char *sourceName(uint8_t source);

    /// Packed, fixed-size record so NVS slots stay a predictable size.
    struct __attribute__((packed)) RawRecord {
        uint32_t seq;
        uint32_t timestamp;
        uint8_t event_type;
        uint8_t source;
        uint8_t result;
        char node_id[16];
        char metadata[32];
    };

    bool load();
    bool persistCurrentSlot();
    void advanceSlot();

    nvs_handle_t m_handle = 0;
    bool m_initialized = false;
    uint32_t m_nextSeq = 0;
    uint8_t m_currentSlot = 0;
    uint8_t m_recordsInSlot = 0;
    std::deque<RawRecord> m_ring;

    static constexpr size_t kMaxRecords = 256;
    static constexpr size_t kRecordsPerSlot = 32;
    static constexpr size_t kSlots = kMaxRecords / kRecordsPerSlot;

    static const char *TAG;
};
