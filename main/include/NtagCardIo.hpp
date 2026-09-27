#pragma once

#include "NfcReader.hpp"

#include <cstddef>
#include <cstdint>

/**
 * Minimal ISO14443A Type 2 (NTAG / MIFARE Ultralight) page I/O.
 *
 * Type 2 tags are not ISO-DEP, so their READ/WRITE commands cannot travel as
 * APDUs. They go through INfcReader::transceiveRaw(), which only the PN532
 * backend implements today; every other reader reports supportsCardWrite() false
 * and the guest-teaching path answers "unsupported" instead of failing obscurely.
 *
 * Only two commands are needed:
 *   0x30 READ  <page>            -> 16 bytes (four pages)
 *   0xA2 WRITE <page> <4 bytes>  -> no payload
 * The PN532 handles CRC and the Type 2 short-frame ACK inside InDataExchange.
 */
namespace ntag {

/// NTAG/Ultralight user memory starts here (pages 0-2 are UID/lock/CC).
inline constexpr uint16_t kUserFirstPage = 4;
/// Capability Container lives at page 3.
inline constexpr uint16_t kCcPage = 3;
/// Every write must land on a 4-byte page boundary.
inline constexpr size_t kPageSize = 4;

enum class Family : uint8_t {
    Unknown = 0,
    Ntag213,
    Ntag215,
    Ntag216,
    Ultralight,
    Other,
};

struct Info {
    Family family = Family::Unknown;
    uint16_t userBytes = 0;   ///< Usable bytes from kUserFirstPage (from the CC)
    uint16_t userPages = 0;   ///< userBytes / 4
    uint8_t cc[4] = {0, 0, 0, 0};
    bool identified = false;
};

const char *familyToString(Family f);

/**
 * Read the capability container at page 3 and derive the usable user memory.
 *
 * @note The reader must already have the tag selected (see INfcReader::pollForTag).
 */
bool identify(INfcReader &reader, Info &out, uint32_t timeoutMs = 200);

/// Read `len` bytes from `page`. `len` must be a non-zero multiple of 4 and the
/// range must fit in the tag. Pages are read four at a time.
bool readPages(INfcReader &reader, uint16_t page, uint8_t *out, size_t len,
               uint32_t timeoutMs = 200);

/**
 * Write `len` bytes starting at `page` (multiple of 4), then read every page back.
 *
 * The read-back is not optional: the PN532 reports success for a WRITE frame it
 * merely managed to transmit, so a write is only believed once the same bytes come
 * back off the tag. A failed verification returns false and leaves the card in an
 * indeterminate state, which is why callers must not commit a credential until
 * this returns true.
 */
bool writePages(INfcReader &reader, uint16_t page, const uint8_t *data, size_t len,
                uint32_t timeoutMs = 200);

} // namespace ntag
