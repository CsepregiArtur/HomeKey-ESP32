#include "GuestTagManager.hpp"

#include "app_event_loop.hpp"
#include "app_events.hpp"

#include <cJSON.h>
#include <JsonGuard.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <fmt/format.h>
#include <sodium.h>
#include <time.h>

const char *GuestTagManager::TAG = "GuestTags";

namespace {

/// NVS key. Budgeted: NVS_KEY_NAME_MAX_SIZE is 16 including the NUL, so 15 chars max.
constexpr const char *kGuestBlobKey = "GUEST_TAGS";
static_assert(sizeof("GUEST_TAGS") - 1 < 16, "NVS key too long");

/// Bumped when the on-disk blob layout changes. A blob from a future version is
/// ignored rather than misread (fail closed), matching HouseholdManager.
constexpr uint8_t kBlobVersion = 1;

/// Domain-separation label for the per-tag card key.
constexpr char kCardKeyLabel[] = "HK-GUEST-TAG-v1";

constexpr size_t kHeaderLen = 16;

#pragma pack(push, 1)
struct BlobHeader {
    uint8_t version;
    uint8_t count;
    uint8_t global_enabled;
    uint8_t reserved0;
    uint32_t default_validity_sec;
    uint8_t reserved1[8];
};
#pragma pack(pop)
static_assert(sizeof(BlobHeader) == kHeaderLen, "BlobHeader layout changed");

/// Card key: BLAKE2b-256 keyed with the per-tag token. Only a node that holds the
/// token can derive it, which is what makes a foreign payload unreadable/unforgable.
void deriveCardKey(const uint8_t token[guest::kTokenLen], uint8_t out[32]) {
    crypto_generichash(out, 32, reinterpret_cast<const unsigned char *>(kCardKeyLabel),
                       sizeof(kCardKeyLabel) - 1, token, guest::kTokenLen);
}

uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    return 0xFF;
}

/**
 * Announce a *local* change to the guest table.
 *
 * importTag() deliberately does not call this: importing already is the reaction to
 * another node's announcement, and re-announcing would bounce the table between
 * nodes forever.
 */
void publishGuestStateChanged() {
    AppEventLoop::publish(GUEST_EVENT, GUEST_STATE_CHANGED, nullptr, 0);
}

} // namespace

namespace guest {

std::string tagIdHex(const GuestTagRecord &rec) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", rec.tag_raw[0], rec.tag_raw[1],
                  rec.tag_raw[2], rec.tag_raw[3]);
    return std::string(buf);
}

std::string uidHex(const std::vector<uint8_t> &uid) {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(uid.size() * 2);
    for (uint8_t b : uid) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

bool tagIdFromHex(const std::string &hex, uint8_t out[4]) {
    if (hex.size() != 8) return false;
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t hi = hexNibble(hex[i * 2]);
        const uint8_t lo = hexNibble(hex[i * 2 + 1]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool uidFromHex(const std::string &hex, std::vector<uint8_t> &out) {
    if (hex.empty() || (hex.size() % 2) != 0 || hex.size() > guest::kUidMaxLen * 2) {
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const uint8_t hi = hexNibble(hex[i]);
        const uint8_t lo = hexNibble(hex[i + 1]);
        if (hi == 0xFF || lo == 0xFF) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace guest

GuestTagManager::GuestTagManager() = default;

GuestTagManager::~GuestTagManager() {
    if (m_initialized && m_handle) {
        nvs_close(m_handle);
        m_handle = 0;
    }
}

uint32_t GuestTagManager::wallClockNow() {
    const time_t now = time(nullptr);
    // Same sanity floor the rest of the firmware uses; anything below means NTP has
    // not landed yet. We deliberately do NOT fall back to uptime here: comparing a
    // calendar validity window against uptime would accept expired tags.
    if (now > 1000000000) {
        return static_cast<uint32_t>(now);
    }
    return 0;
}

bool GuestTagManager::begin() {
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

bool GuestTagManager::load() {
    size_t size = 0;
    esp_err_t err = nvs_get_blob(m_handle, kGuestBlobKey, nullptr, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No guest table stored yet.");
        return true;
    }
    if (err != ESP_OK || size < kHeaderLen) {
        ESP_LOGW(TAG, "Guest table missing or too small (%s); starting empty.",
                 esp_err_to_name(err));
        return true;
    }

    std::vector<uint8_t> buf(size);
    if (nvs_get_blob(m_handle, kGuestBlobKey, buf.data(), &size) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read guest table; starting empty.");
        return true;
    }

    BlobHeader hdr{};
    std::memcpy(&hdr, buf.data(), kHeaderLen);
    if (hdr.version != kBlobVersion) {
        // Fail closed: an unknown layout is not guessed at.
        ESP_LOGW(TAG, "Guest table version %u is not supported (expected %u); ignoring it.",
                 static_cast<unsigned>(hdr.version), static_cast<unsigned>(kBlobVersion));
        return true;
    }

    m_globalEnabled = hdr.global_enabled != 0;
    m_defaultValiditySec = hdr.default_validity_sec;

    const size_t maxBySize = (size - kHeaderLen) / sizeof(guest::GuestTagRecord);
    const size_t n = std::min<size_t>({hdr.count, guest::kMaxTags, maxBySize});
    m_count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(&m_tags[i], buf.data() + kHeaderLen + i * sizeof(guest::GuestTagRecord),
                    sizeof(guest::GuestTagRecord));
    }
    for (size_t i = n; i < guest::kMaxTags; ++i) {
        std::memset(&m_tags[i], 0, sizeof(guest::GuestTagRecord));
    }
    ESP_LOGI(TAG, "Loaded %u guest tag(s); guest access %s.", static_cast<unsigned>(m_count),
             m_globalEnabled ? "ENABLED" : "disabled");
    return true;
}

bool GuestTagManager::save() {
    if (!m_initialized) {
        return false;
    }
    BlobHeader hdr{};
    hdr.version = kBlobVersion;
    hdr.count = m_count;
    hdr.global_enabled = m_globalEnabled ? 1 : 0;
    hdr.default_validity_sec = m_defaultValiditySec;

    std::vector<uint8_t> buf(kHeaderLen + guest::kMaxTags * sizeof(guest::GuestTagRecord), 0);
    std::memcpy(buf.data(), &hdr, kHeaderLen);
    for (size_t i = 0; i < m_count; ++i) {
        std::memcpy(buf.data() + kHeaderLen + i * sizeof(guest::GuestTagRecord), &m_tags[i],
                    sizeof(guest::GuestTagRecord));
    }

    esp_err_t err = nvs_set_blob(m_handle, kGuestBlobKey, buf.data(), buf.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", kGuestBlobKey, esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(m_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool GuestTagManager::setGlobalEnabled(bool enabled) {
    m_globalEnabled = enabled;
    if (!save()) {
        // Report the truth rather than claiming a state that will not survive a reboot.
        return false;
    }
    ESP_LOGI(TAG, "Guest access %s.", enabled ? "ENABLED" : "disabled");
    publishGuestStateChanged();
    return true;
}

bool GuestTagManager::setDefaultValiditySeconds(uint32_t seconds) {
    // Bound it to a sane ceiling (10 years) so a bad unit cannot overflow the window.
    m_defaultValiditySec = std::min<uint32_t>(seconds, 10u * 365u * 24u * 3600u);
    if (!save()) {
        return false;
    }
    ESP_LOGI(TAG, "Default guest validity: %u s.", static_cast<unsigned>(m_defaultValiditySec));
    publishGuestStateChanged();
    return true;
}

size_t GuestTagManager::count() const { return m_count; }

std::vector<guest::GuestTagRecord> GuestTagManager::list() const {
    std::vector<guest::GuestTagRecord> out;
    out.reserve(m_count);
    for (size_t i = 0; i < m_count; ++i) {
        out.push_back(m_tags[i]);
    }
    return out;
}

const guest::GuestTagRecord *GuestTagManager::findByTagId(const std::string &tagId) const {
    uint8_t raw[4];
    if (!guest::tagIdFromHex(tagId, raw)) {
        return nullptr;
    }
    for (size_t i = 0; i < m_count; ++i) {
        if (std::memcmp(m_tags[i].tag_raw, raw, 4) == 0) {
            return &m_tags[i];
        }
    }
    return nullptr;
}

const guest::GuestTagRecord *GuestTagManager::findByUid(const std::vector<uint8_t> &uid) const {
    if (uid.empty() || uid.size() > guest::kUidMaxLen) {
        return nullptr;
    }
    for (size_t i = 0; i < m_count; ++i) {
        const auto &rec = m_tags[i];
        if (rec.uid_len == uid.size() && std::memcmp(rec.uid, uid.data(), uid.size()) == 0) {
            return &rec;
        }
    }
    return nullptr;
}

bool GuestTagManager::mintTag(const std::string &label, uint32_t validFrom,
                              uint32_t validUntil, guest::GuestTagRecord &out) const {
    if (m_count >= guest::kMaxTags) {
        ESP_LOGW(TAG, "Guest table full (%u); revoke a tag first.",
                 static_cast<unsigned>(guest::kMaxTags));
        return false;
    }

    std::memset(&out, 0, sizeof(out));
    randombytes_buf(out.tag_raw, sizeof(out.tag_raw));
    randombytes_buf(out.token, sizeof(out.token));
    out.enabled = 1;
    out.created_at = wallClockNow();

    std::snprintf(out.label, sizeof(out.label), "%s", label.c_str());

    if (validFrom == 0 && validUntil == 0 && m_defaultValiditySec > 0) {
        const uint32_t now = wallClockNow();
        if (now > 0) {
            validFrom = now;
            validUntil = now + m_defaultValiditySec;
        } else {
            ESP_LOGW(TAG, "No wall clock yet; teaching an unbounded guest tag instead of "
                          "applying the default validity window.");
        }
    }
    out.valid_from = validFrom;
    out.valid_until = validUntil;
    return true;
}

bool GuestTagManager::commitTag(const guest::GuestTagRecord &rec) {
    // Re-teaching the same physical card refreshes its existing slot.
    for (size_t i = 0; i < m_count; ++i) {
        if (m_tags[i].uid_len == rec.uid_len && rec.uid_len > 0 &&
            std::memcmp(m_tags[i].uid, rec.uid, rec.uid_len) == 0) {
            m_tags[i] = rec;
            if (!save()) {
                return false;
            }
            ESP_LOGI(TAG, "Guest tag %s re-taught (slot %zu).",
                     guest::tagIdHex(rec).c_str(), i);
            return true;
        }
    }
    if (m_count >= guest::kMaxTags) {
        return false;
    }
    m_tags[m_count++] = rec;
    if (!save()) {
        // Roll back so RAM and NVS agree; the caller must not believe it is stored.
        --m_count;
        std::memset(&m_tags[m_count], 0, sizeof(guest::GuestTagRecord));
        ESP_LOGE(TAG, "Could not store guest tag %s.", guest::tagIdHex(rec).c_str());
        return false;
    }
    ESP_LOGI(TAG, "Guest tag %s committed for UID %s.", guest::tagIdHex(rec).c_str(),
             guest::uidHex(std::vector<uint8_t>(rec.uid, rec.uid + rec.uid_len)).c_str());
    publishGuestStateChanged();
    return true;
}

bool GuestTagManager::importTag(const guest::GuestTagRecord &rec) {
    // Idempotent: a sync replay of an existing tag updates it in place.
    for (size_t i = 0; i < m_count; ++i) {
        if (std::memcmp(m_tags[i].tag_raw, rec.tag_raw, 4) == 0) {
            m_tags[i] = rec;
            return save();
        }
    }
    if (m_count >= guest::kMaxTags) {
        ESP_LOGW(TAG, "Cannot import guest tag %s: table full.", guest::tagIdHex(rec).c_str());
        return false;
    }
    m_tags[m_count++] = rec;
    if (!save()) {
        --m_count;
        std::memset(&m_tags[m_count], 0, sizeof(guest::GuestTagRecord));
        return false;
    }
    ESP_LOGI(TAG, "Imported guest tag %s from the household.",
             guest::tagIdHex(rec).c_str());
    return true;
}

bool GuestTagManager::revokeTag(const std::string &tagId) {
    uint8_t raw[4];
    if (!guest::tagIdFromHex(tagId, raw)) {
        return false;
    }
    for (size_t i = 0; i < m_count; ++i) {
        if (std::memcmp(m_tags[i].tag_raw, raw, 4) == 0) {
            // Compact the table; slot order is not meaningful.
            for (size_t j = i + 1; j < m_count; ++j) {
                m_tags[j - 1] = m_tags[j];
            }
            --m_count;
            std::memset(&m_tags[m_count], 0, sizeof(guest::GuestTagRecord));
            if (!save()) {
                return false;
            }
            ESP_LOGI(TAG, "Revoked guest tag %s.", tagId.c_str());
            publishGuestStateChanged();
            return true;
        }
    }
    return false;
}

bool GuestTagManager::revokeByUid(const std::vector<uint8_t> &uid) {
    const auto *rec = findByUid(uid);
    if (rec == nullptr) {
        return false;
    }
    return revokeTag(guest::tagIdHex(*rec));
}

bool GuestTagManager::setTagEnabled(const std::string &tagId, bool enabled) {
    uint8_t raw[4];
    if (!guest::tagIdFromHex(tagId, raw)) {
        return false;
    }
    for (size_t i = 0; i < m_count; ++i) {
        if (std::memcmp(m_tags[i].tag_raw, raw, 4) == 0) {
            m_tags[i].enabled = enabled ? 1 : 0;
            return save();
        }
    }
    return false;
}

bool GuestTagManager::buildCardPayload(const guest::GuestTagRecord &rec,
                                       const std::vector<uint8_t> &uid,
                                       std::array<uint8_t, guest::kCardPayloadLen> &out,
                                       std::string &err) {
    if (uid.empty() || uid.size() > guest::kUidMaxLen) {
        err = "invalid uid length";
        return false;
    }

    guest::CardPlaintext pt{};
    std::memcpy(pt.tag_raw, rec.tag_raw, sizeof(pt.tag_raw));
    pt.flags = 0x01;
    pt.valid_from = rec.valid_from;
    pt.valid_until = rec.valid_until;
    std::snprintf(pt.label, sizeof(pt.label), "%s", rec.label);

    uint8_t key[32];
    deriveCardKey(rec.token, key);

    out.fill(0);
    out[0] = guest::kCardMagic0;
    out[1] = guest::kCardMagic1;
    out[2] = guest::kCardMagic2;
    out[3] = guest::kCardMagic3;
    out[4] = guest::kCardFormatVersion;
    out[5] = 0; // reserved

    uint8_t *nonce = out.data() + 6;
    randombytes_buf(nonce, guest::kNonceLen);

    unsigned long long clen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        out.data() + 6 + guest::kNonceLen, &clen,
        reinterpret_cast<const unsigned char *>(&pt), sizeof(pt), uid.data(), uid.size(),
        nullptr, nonce, key);
    sodium_memzero(key, sizeof(key));
    if (rc != 0 || clen != sizeof(pt) + guest::kAeadTagLen) {
        err = "encrypt failed";
        return false;
    }
    return true;
}

bool GuestTagManager::parseCardPayload(const std::array<uint8_t, guest::kCardPayloadLen> &in,
                                       const std::vector<uint8_t> &uid,
                                       const guest::GuestTagRecord &rec,
                                       guest::CardPlaintext &out) {
    if (in[0] != guest::kCardMagic0 || in[1] != guest::kCardMagic1 ||
        in[2] != guest::kCardMagic2 || in[3] != guest::kCardMagic3) {
        return false;
    }
    if (in[4] != guest::kCardFormatVersion) {
        return false;
    }

    const uint8_t *nonce = in.data() + 6;
    const uint8_t *ct = in.data() + 6 + guest::kNonceLen;
    const size_t ctLen = sizeof(guest::CardPlaintext) + guest::kAeadTagLen;
    if (6 + guest::kNonceLen + ctLen > in.size()) {
        return false;
    }

    uint8_t key[32];
    deriveCardKey(rec.token, key);

    unsigned long long mlen = 0;
    // The UID is authenticated but NOT encrypted: a payload lifted onto a different
    // card fails here, which is the point of binding it.
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char *>(&out), &mlen, nullptr, ct, ctLen, uid.data(),
        uid.size(), nonce, key);
    sodium_memzero(key, sizeof(key));
    if (rc != 0 || mlen != sizeof(guest::CardPlaintext)) {
        return false;
    }
    return std::memcmp(out.tag_raw, rec.tag_raw, sizeof(rec.tag_raw)) == 0;
}

guest::VerifyResult GuestTagManager::verify(
    const std::vector<uint8_t> &uid,
    const std::array<uint8_t, guest::kCardPayloadLen> &payload, uint32_t now,
    guest::GuestTagRecord *record) const {
    const auto *rec = findByUid(uid);
    if (rec == nullptr) {
        return guest::VerifyResult::NoRecord;
    }
    if (record != nullptr) {
        *record = *rec;
    }
    if (!m_globalEnabled || rec->enabled == 0) {
        return guest::VerifyResult::Disabled;
    }

    guest::CardPlaintext pt{};
    if (!parseCardPayload(payload, uid, *rec, pt)) {
        return guest::VerifyResult::BadPayload;
    }

    if (rec->valid_from != 0 || rec->valid_until != 0) {
        if (now == 0) {
            // A time-bounded tag cannot be judged without a clock. Fail closed.
            return guest::VerifyResult::NoClock;
        }
        if (rec->valid_from != 0 && now < rec->valid_from) {
            return guest::VerifyResult::NotYetValid;
        }
        if (rec->valid_until != 0 && now > rec->valid_until) {
            return guest::VerifyResult::Expired;
        }
    }
    return guest::VerifyResult::Accepted;
}

void GuestTagManager::markUsed(const std::string &tagId, uint32_t now) {
    uint8_t raw[4];
    if (!guest::tagIdFromHex(tagId, raw)) {
        return;
    }
    for (size_t i = 0; i < m_count; ++i) {
        if (std::memcmp(m_tags[i].tag_raw, raw, 4) == 0) {
            m_tags[i].last_used_at = now;
            m_tags[i].use_count += 1;
            save();
            return;
        }
    }
}

std::string GuestTagManager::recordToJson(const guest::GuestTagRecord &rec) {
    JsonBuilder obj = JsonBuilder::object();
    if (!obj) {
        return "{}";
    }
    obj.addString("tag_id", guest::tagIdHex(rec));
    obj.addString("uid", guest::uidHex(std::vector<uint8_t>(rec.uid, rec.uid + rec.uid_len)));
    obj.addString("label", std::string(rec.label));
    obj.addBool("enabled", rec.enabled != 0);
    obj.addNumber("valid_from", static_cast<double>(rec.valid_from));
    obj.addNumber("valid_until", static_cast<double>(rec.valid_until));
    obj.addNumber("created_at", static_cast<double>(rec.created_at));
    obj.addNumber("last_used_at", static_cast<double>(rec.last_used_at));
    obj.addNumber("use_count", static_cast<double>(rec.use_count));
    // Token is the credential. Only ever emitted over TLS (/api/ha/*) or the
    // household MQTT namespace, so another node can verify the same card offline.
    obj.addString("token", guest::uidHex(std::vector<uint8_t>(rec.token, rec.token + guest::kTokenLen)));
    return obj.toStringUnformatted();
}

bool GuestTagManager::recordFromJson(const std::string &json, guest::GuestTagRecord &out,
                                     std::string &err) {
    auto parsed = parse_json(json);
    if (!parsed) {
        err = "invalid json";
        return false;
    }
    cJSON *root = parsed->get();
    if (!cJSON_IsObject(root)) {
        err = "not an object";
        return false;
    }

    const cJSON *tagId = cJSON_GetObjectItemCaseSensitive(root, "tag_id");
    const cJSON *uidItem = cJSON_GetObjectItemCaseSensitive(root, "uid");
    const cJSON *tokenItem = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(tagId) || !cJSON_IsString(uidItem) || !cJSON_IsString(tokenItem)) {
        err = "missing tag_id/uid/token";
        return false;
    }

    std::memset(&out, 0, sizeof(out));
    if (!guest::tagIdFromHex(tagId->valuestring, out.tag_raw)) {
        err = "bad tag_id";
        return false;
    }

    std::vector<uint8_t> uid;
    if (!guest::uidFromHex(uidItem->valuestring, uid) || uid.empty()) {
        err = "bad uid";
        return false;
    }
    out.uid_len = static_cast<uint8_t>(uid.size());
    std::memcpy(out.uid, uid.data(), uid.size());

    std::vector<uint8_t> token;
    if (!guest::uidFromHex(tokenItem->valuestring, token) ||
        token.size() != guest::kTokenLen) {
        err = "bad token";
        return false;
    }
    std::memcpy(out.token, token.data(), guest::kTokenLen);

    const cJSON *label = cJSON_GetObjectItemCaseSensitive(root, "label");
    if (cJSON_IsString(label)) {
        std::snprintf(out.label, sizeof(out.label), "%s", label->valuestring);
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    out.enabled = cJSON_IsBool(enabled) ? (cJSON_IsTrue(enabled) ? 1 : 0) : 1;

    auto num = [&](const char *key, uint32_t &dst) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
            dst = static_cast<uint32_t>(item->valuedouble);
        }
    };
    num("valid_from", out.valid_from);
    num("valid_until", out.valid_until);
    num("created_at", out.created_at);
    num("last_used_at", out.last_used_at);
    num("use_count", out.use_count);
    return true;
}

std::string GuestTagManager::statusJson(bool includeTokens) const {
    JsonBuilder obj = JsonBuilder::object();
    if (!obj) {
        return "{}";
    }
    obj.addBool("enabled", m_globalEnabled);
    obj.addNumber("default_validity_seconds", static_cast<double>(m_defaultValiditySec));
    obj.addNumber("capacity", static_cast<double>(guest::kMaxTags));
    obj.addNumber("count", static_cast<double>(m_count));
    obj.addNumber("wall_clock", static_cast<double>(wallClockNow()));
    obj.withArray("tags", [&](JsonBuilder &arr) {
        for (size_t i = 0; i < m_count; ++i) {
            if (includeTokens) {
                auto rec = parse_json(recordToJson(m_tags[i]));
                if (rec) {
                    arr.addItemToArray(std::move(*rec));
                }
                continue;
            }
            // Token-free projection for display/HASS discovery.
            const auto &rec = m_tags[i];
            JsonBuilder item = JsonBuilder::object();
            if (!item) {
                continue;
            }
            item.addString("tag_id", guest::tagIdHex(rec));
            item.addString("uid", guest::uidHex(std::vector<uint8_t>(rec.uid, rec.uid + rec.uid_len)));
            item.addString("label", std::string(rec.label));
            item.addBool("enabled", rec.enabled != 0);
            item.addNumber("valid_from", static_cast<double>(rec.valid_from));
            item.addNumber("valid_until", static_cast<double>(rec.valid_until));
            item.addNumber("created_at", static_cast<double>(rec.created_at));
            item.addNumber("last_used_at", static_cast<double>(rec.last_used_at));
            item.addNumber("use_count", static_cast<double>(rec.use_count));
            arr.addItemToArray(item.extractGuard());
        }
    });
    return obj.toStringUnformatted();
}
