#include "NtagCardIo.hpp"

#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <esp_rom_sys.h>

namespace ntag {
namespace {

const char *TAG = "NtagCardIo";

constexpr uint8_t kCmdRead = 0x30;
constexpr uint8_t kCmdWrite = 0xA2;
constexpr size_t kReadChunkPages = 4; // READ returns exactly four pages

/// Type 2 EEPROM write time. The datasheet allows up to 5 ms; the read-back below
/// would otherwise race the internal write and report a false failure.
constexpr uint32_t kWriteSettleUs = 6000;

bool readFourPages(INfcReader &reader, uint16_t page, uint8_t out[16], uint32_t timeoutMs) {
    const std::vector<uint8_t> cmd = {kCmdRead, static_cast<uint8_t>(page & 0xFF)};
    std::vector<uint8_t> resp;
    if (!reader.transceiveRaw(cmd, resp, timeoutMs)) {
        ESP_LOGW(TAG, "READ page %u was not answered at all (no response frame).",
                 static_cast<unsigned>(page));
        return false;
    }
    if (resp.empty()) {
        // The frame came back but carried no data: the tag refused the command. For a
        // MIFARE Classic that is expected - its READ only works after sector
        // authentication, which this firmware deliberately does not implement - and it
        // is by far the most common reason a card cannot be taught. Saying so here saves
        // the user from guessing, because the caller's message alone cannot tell this
        // apart from a card that really is not a Type 2 tag.
        ESP_LOGW(TAG,
                 "READ page %u was refused (0 bytes of data). A MIFARE Classic refuses "
                 "the Type 2 READ until its sector is authenticated, so a 4-byte UID "
                 "card is normally a Classic and cannot be used; NTAG213/215/216 have "
                 "7-byte UIDs.",
                 static_cast<unsigned>(page));
        return false;
    }
    if (resp.size() < 16) {
        ESP_LOGW(TAG, "READ page %u returned %u bytes (expected 16).",
                 static_cast<unsigned>(page), static_cast<unsigned>(resp.size()));
        return false;
    }
    std::memcpy(out, resp.data(), 16);
    return true;
}

bool writeOnePage(INfcReader &reader, uint16_t page, const uint8_t data[4],
                  uint32_t timeoutMs) {
    const std::vector<uint8_t> cmd = {kCmdWrite, static_cast<uint8_t>(page & 0xFF), data[0],
                                      data[1], data[2], data[3]};
    std::vector<uint8_t> resp;
    // A successful WRITE has no response payload, so the return value only tells us
    // the frame was sent. writePages() verifies by reading back.
    (void)reader.transceiveRaw(cmd, resp, timeoutMs);
    esp_rom_delay_us(kWriteSettleUs);
    return true;
}

} // namespace

const char *familyToString(Family f) {
    switch (f) {
        case Family::Ntag213: return "NTAG213";
        case Family::Ntag215: return "NTAG215";
        case Family::Ntag216: return "NTAG216";
        case Family::Ultralight: return "MIFARE Ultralight";
        case Family::Other: return "ISO14443A Type 2";
        case Family::Unknown: break;
    }
    return "unknown";
}

bool identify(INfcReader &reader, Info &out, uint32_t timeoutMs) {
    out = Info{};
    uint8_t buf[16];
    if (!readFourPages(reader, kCcPage, buf, timeoutMs)) {
        return false;
    }
    std::memcpy(out.cc, buf, 4);

    // CC0 = 0x12 (E1 = "NFC Forum Type 2" magic byte pair), CC1 = 0xE1.
    if (buf[0] != 0x12 || buf[1] != 0xE1) {
        ESP_LOGD(TAG, "No Type 2 capability container (CC=%02X %02X %02X %02X).", buf[0],
                 buf[1], buf[2], buf[3]);
        return false;
    }

    // CC2 is the user memory size in 8-byte units.
    out.userBytes = static_cast<uint16_t>(buf[2]) * 8;
    out.userPages = static_cast<uint16_t>(out.userBytes / kPageSize);
    out.identified = out.userBytes > 0;

    // Total memory = 16 (UID/CC) + user bytes; used only to pick a friendly name.
    const uint16_t totalPages =
        static_cast<uint16_t>(kUserFirstPage + out.userPages);
    if (out.userBytes == 144) {
        out.family = Family::Ntag213;
    } else if (out.userBytes == 496) {
        out.family = Family::Ntag215;
    } else if (out.userBytes == 872) {
        out.family = Family::Ntag216;
    } else if (out.userBytes == 48) {
        out.family = Family::Ultralight;
    } else {
        out.family = Family::Other;
    }
    ESP_LOGI(TAG, "Tag identified: %s, %u user bytes, %u pages (%u total pages).",
             familyToString(out.family), static_cast<unsigned>(out.userBytes),
             static_cast<unsigned>(out.userPages), static_cast<unsigned>(totalPages));
    return out.identified;
}

bool readPages(INfcReader &reader, uint16_t page, uint8_t *out, size_t len,
               uint32_t timeoutMs) {
    if (out == nullptr || len == 0 || (len % kPageSize) != 0) {
        return false;
    }
    size_t done = 0;
    uint16_t current = page;
    while (done < len) {
        uint8_t buf[16];
        if (!readFourPages(reader, current, buf, timeoutMs)) {
            return false;
        }
        const size_t take = std::min<size_t>(16, len - done);
        std::memcpy(out + done, buf, take);
        done += take;
        current = static_cast<uint16_t>(current + kReadChunkPages);
    }
    return true;
}

bool writePages(INfcReader &reader, uint16_t page, const uint8_t *data, size_t len,
                uint32_t timeoutMs) {
    if (data == nullptr || len == 0 || (len % kPageSize) != 0) {
        return false;
    }
    uint16_t current = page;
    for (size_t done = 0; done < len; done += kPageSize, ++current) {
        if (!writeOnePage(reader, current, data + done, timeoutMs)) {
            return false;
        }
    }

    // Read back the whole range and compare. Without this a wrong key/page or a
    // locked tag would look like a successful teach.
    std::vector<uint8_t> verify(len, 0);
    if (!readPages(reader, page, verify.data(), len, timeoutMs)) {
        ESP_LOGW(TAG, "Write read-back failed.");
        return false;
    }
    if (std::memcmp(verify.data(), data, len) != 0) {
        ESP_LOGE(TAG, "Write verify mismatch at page %u.", static_cast<unsigned>(page));
        return false;
    }
    return true;
}

} // namespace ntag
