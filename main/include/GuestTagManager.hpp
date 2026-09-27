#pragma once

#include "guest_types.hpp"

#include <nvs.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Owns the guest-tag credential table: mint, teach, revoke, expire and verify.
 *
 * The table is one bounded NVS blob in the shared `SAVED_DATA` namespace
 * (`GUEST_TAGS`), matching how the other managers persist. Nothing here is
 * device-specific crypto identity, so a guest table can safely be re-created on a
 * replacement node.
 *
 * Distribution: a tag taught on one node is published to the household MQTT
 * namespace so the other nodes can import it. Verification itself is entirely
 * local -- a guest tap must unlock with no broker, no HA and no internet, exactly
 * like HomeKey does.
 */
class GuestTagManager {
public:
    GuestTagManager();
    ~GuestTagManager();

    bool begin();

    // --- Global settings (the "guest access" switch driven from HA) ---

    bool globalEnabled() const { return m_globalEnabled; }
    bool setGlobalEnabled(bool enabled);

    /// Default validity applied when a caller does not supply an explicit window.
    /// 0 means "no expiry".
    uint32_t defaultValiditySeconds() const { return m_defaultValiditySec; }
    bool setDefaultValiditySeconds(uint32_t seconds);

    // --- Table access ---

    size_t capacity() const { return guest::kMaxTags; }
    size_t count() const;
    std::vector<guest::GuestTagRecord> list() const;

    /// Case-insensitive lookup by 8-character hex tag id. Nullptr when absent.
    const guest::GuestTagRecord *findByTagId(const std::string &tagId) const;

    /// Lookup by card UID. Nullptr when the card was never taught here.
    const guest::GuestTagRecord *findByUid(const std::vector<uint8_t> &uid) const;

    // --- Lifecycle ---

    /**
     * Mint a new record that is NOT yet committed to NVS.
     *
     * Committing before the card write succeeded would leave an orphan credential
     * for a card that does not exist, so the mint/commit split is deliberate: mint,
     * write the card, then commit only if the write verified.
     *
     * @param label      Guest name (truncated to guest::kLabelMaxLen - 1).
     * @param validFrom  Unix seconds, 0 = immediately valid.
     * @param validUntil Unix seconds, 0 = no expiry. If both are 0 and a default
     *                   validity is configured, the default window is applied.
     */
    bool mintTag(const std::string &label, uint32_t validFrom, uint32_t validUntil,
                 guest::GuestTagRecord &out) const;

    /// Commit a minted record. Re-teaching the same card replaces its record
    /// (matched by UID) instead of consuming a second slot.
    bool commitTag(const guest::GuestTagRecord &rec);

    /// Import a record received from another household node. Idempotent by tag id.
    bool importTag(const guest::GuestTagRecord &rec);

    bool revokeTag(const std::string &tagId);
    bool revokeByUid(const std::vector<uint8_t> &uid);
    bool setTagEnabled(const std::string &tagId, bool enabled);

    /**
     * Evaluate a tapped card.
     *
     * Performs the UID lookup, validates the payload read from the card against the
     * stored token, then checks the enabled flags and the validity window.
     *
     * @param uid     Card UID as read by the reader.
     * @param payload Raw payload read from the card's first user page.
     * @param now     Current wall-clock seconds (see wallClockNow()).
     * @param record  Optional out-parameter filled when a record was found, even if
     *                the tap was ultimately rejected (so the caller can audit why).
     */
    guest::VerifyResult verify(const std::vector<uint8_t> &uid,
                               const std::array<uint8_t, guest::kCardPayloadLen> &payload,
                               uint32_t now,
                               guest::GuestTagRecord *record = nullptr) const;

    /// Record a successful use (counter + timestamp). Best-effort persist.
    void markUsed(const std::string &tagId, uint32_t now);

    // --- Card payload codec (used by NtagCardIo via NfcManager) ---

    /// Build the 80-byte payload to write at the card's first user page.
    static bool buildCardPayload(const guest::GuestTagRecord &rec,
                                 const std::vector<uint8_t> &uid,
                                 std::array<uint8_t, guest::kCardPayloadLen> &out,
                                 std::string &err);

    /// Decrypt and validate a payload read back from the card.
    static bool parseCardPayload(const std::array<uint8_t, guest::kCardPayloadLen> &in,
                                 const std::vector<uint8_t> &uid,
                                 const guest::GuestTagRecord &rec,
                                 guest::CardPlaintext &out);

    // --- JSON (HTTP API + MQTT status/sync) ---

    /// One record as a JSON object. Includes the token, which is why this is only
    /// ever sent over TLS (HTTP API) or the household MQTT namespace.
    static std::string recordToJson(const guest::GuestTagRecord &rec);

    /// Parse a record from the sync payload. Rejects malformed input.
    static bool recordFromJson(const std::string &json, guest::GuestTagRecord &out,
                               std::string &err);

    /// Full status document: enabled, default validity, capacity, tags[].
    ///
    /// @param includeTokens When true each tag also carries its per-tag secret.
    ///        Only the household sync topic may ask for that; the status topic Home
    ///        Assistant reads must not, or anyone able to read the broker could
    ///        clone a guest card.
    std::string statusJson(bool includeTokens = false) const;

    // --- Time ---

    /// Wall-clock seconds, or 0 when the device has no trustworthy time yet.
    /// Unlike AuditManager's fallback this does NOT substitute uptime: a validity
    /// window compared against uptime would silently accept expired tags.
    static uint32_t wallClockNow();

private:
    bool load();
    bool save();

    nvs_handle_t m_handle = 0;
    bool m_initialized = false;

    bool m_globalEnabled = false;
    uint32_t m_defaultValiditySec = 0;

    /// Fixed-size table, count in m_count. Spare slots are zeroed.
    std::array<guest::GuestTagRecord, guest::kMaxTags> m_tags{};
    uint8_t m_count = 0;

    static const char *TAG;
};
