#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Shared types for the guest NFC-tag feature.
 *
 * A "guest tag" is an ordinary ISO14443A card (NTAG213/215/216 and friends) on
 * which this firmware writes a small, household-keyed payload. The firmware
 * verifies that payload locally and then unlocks exactly like a HomeKey tap.
 *
 * What this deliberately is NOT: a HomeKey credential. HomeKey is Apple's
 * protocol, backed by Apple-issued issuer keys and a secure element. No
 * third-party tag can produce an Apple-compatible authentication. A guest tag is
 * therefore a *separate, locally verified* credential with a different (weaker)
 * threat model. See docs/content/guest-tags.md.
 *
 * Security model (chosen explicitly, see that document):
 *  - The card carries one random 16-byte `token` per tag, never a shared secret.
 *  - The card payload is XChaCha20-Poly1305 encrypted under a key derived from
 *    that token, with the card UID as associated data. That binds a payload to
 *    the physical card, so copying the bytes onto another card fails.
 *  - Because NTAG memory is *static* (no on-card crypto), anyone who reads the
 *    card with another reader can clone it. This is acceptable for guest access
 *    and is stated plainly, not hidden.
 */
namespace guest {

/// Hard cap on stored guest tags. NVS is the tightest resource on this device
/// (~24 KiB shared with the audit ring and the HomeKey credential store), so the
/// table is a fixed-size array serialized as a single blob -- never a growing list.
inline constexpr size_t kMaxTags = 16;

/// Guest name, NUL-padded. Bounded so a record has a fixed size on disk.
inline constexpr size_t kLabelMaxLen = 16;

/// Card UIDs are 4, 7 or 10 bytes for the ISO14443A parts this firmware polls.
inline constexpr size_t kUidMaxLen = 10;

/// Per-tag random secret. This is the credential: whoever holds it can derive
/// the card key.
inline constexpr size_t kTokenLen = 16;

/// XChaCha20-Poly1305 IETF nonce and authentication tag lengths.
inline constexpr size_t kNonceLen = 24;
inline constexpr size_t kAeadTagLen = 16;

/// Card payload envelope on the tag: magic || version || reserved || nonce || ciphertext
inline constexpr uint8_t kCardMagic0 = 'H';
inline constexpr uint8_t kCardMagic1 = 'K';
inline constexpr uint8_t kCardMagic2 = 'G';
inline constexpr uint8_t kCardMagic3 = '1';
inline constexpr uint8_t kCardFormatVersion = 1;

/// Encrypted plaintext carried on the card (exactly 32 bytes so the payload size
/// is fixed and the card layout never shifts between firmware versions).
#pragma pack(push, 1)
struct CardPlaintext {
    uint8_t tag_raw[4];       ///< Random tag id; matches GuestTagRecord::tag_raw
    uint8_t flags;            ///< bit0 = guest tag; other bits reserved (0)
    uint8_t reserved0[3];
    uint32_t valid_from;      ///< Unix seconds, 0 = no lower bound
    uint32_t valid_until;     ///< Unix seconds, 0 = no upper bound
    char label[kLabelMaxLen]; ///< NUL-padded guest name
};
#pragma pack(pop)
static_assert(sizeof(CardPlaintext) == 32, "CardPlaintext layout changed");

/// Bytes written to the card: 4+1+1+24+(32+16) = 78, padded to a 4-byte page multiple.
inline constexpr size_t kCardPayloadLen = 80;

/// Stored guest tag. Fixed size (72 bytes) so the NVS blob stays bounded.
#pragma pack(push, 1)
struct GuestTagRecord {
    uint8_t tag_raw[4];       ///< Random tag id (also embedded in the card payload)
    uint8_t uid_len;          ///< 0 = record not bound to a specific UID (unused today)
    uint8_t enabled;          ///< 0/1
    uint8_t reserved0[2];
    uint8_t token[kTokenLen]; ///< Per-tag secret; never leaves the device except
                              ///< in the household sync payload (MQTT, TLS).
    uint8_t uid[kUidMaxLen];
    uint8_t reserved1[2];
    uint32_t valid_from;      ///< Unix seconds, 0 = no lower bound
    uint32_t valid_until;     ///< Unix seconds, 0 = no upper bound
    uint32_t created_at;
    uint32_t last_used_at;
    uint32_t use_count;
    char label[kLabelMaxLen];
};
#pragma pack(pop)
static_assert(sizeof(GuestTagRecord) == 72, "GuestTagRecord layout changed");

/// Why a tap was accepted or rejected. Reported to MQTT/HA and the audit log.
enum class VerifyResult : uint8_t {
    Accepted = 0,
    NoRecord,     ///< UID was never taught to this node
    Disabled,     ///< Guest access globally off, or this tag disabled
    NotYetValid,  ///< valid_from is in the future
    Expired,      ///< valid_until has passed
    BadPayload,   ///< No/foreign/corrupt guest payload on the card
    NoClock,      ///< Time-bounded tag but the device has no wall clock yet
    Error,        ///< Reader/crypto failure
};

/// Stable lowercase strings for MQTT/JSON and the audit log.
inline const char *verifyResultToString(VerifyResult r) {
    switch (r) {
        case VerifyResult::Accepted: return "ACCEPTED";
        case VerifyResult::NoRecord: return "NO_RECORD";
        case VerifyResult::Disabled: return "DISABLED";
        case VerifyResult::NotYetValid: return "NOT_YET_VALID";
        case VerifyResult::Expired: return "EXPIRED";
        case VerifyResult::BadPayload: return "BAD_PAYLOAD";
        case VerifyResult::NoClock: return "NO_CLOCK";
        case VerifyResult::Error: return "ERROR";
    }
    return "UNKNOWN";
}

/// 8-character uppercase hex of a record's tag id (used in JSON and sync topics).
std::string tagIdHex(const GuestTagRecord &rec);

/// Uppercase hex of a UID, matching the existing MQTT `uid` field format.
std::string uidHex(const std::vector<uint8_t> &uid);

/// Parse an 8-character hex tag id. Returns false on bad input.
bool tagIdFromHex(const std::string &hex, uint8_t out[4]);

/// Parse hex UID bytes. Returns false on malformed input or odd length.
bool uidFromHex(const std::string &hex, std::vector<uint8_t> &out);

} // namespace guest
