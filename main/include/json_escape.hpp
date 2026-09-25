#pragma once
#include <string>
#include <string_view>

/**
 * @brief Escape a string for use inside a JSON string literal.
 *
 * These documents are assembled with ``fmt::format``, which writes each value between quotes
 * verbatim. Anything a user can type - a device name, a household id, an issuer label - can
 * therefore break the document, and a broken document is never a partial failure: a strict
 * reader discards the whole payload, so one character silently removes every field in it.
 *
 * This codebase already paid for that lesson. The household health document interpolated
 * security warnings that were joined with real newlines, and a newline is not legal inside a
 * JSON string, so the document carrying the lock's state was unparseable on every device that
 * had anything to warn about - the lock simply never updated anywhere and nothing logged a
 * reason.
 *
 * Prefer building a document with cJSON or JsonBuilder where that is practical: they escape
 * by construction and cannot be forgotten for a field. This helper is for the ``fmt::format``
 * sites that remain, and every string interpolated there that did not come from a fixed set
 * of literals should go through it.
 */
inline std::string jsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char *digits = "0123456789abcdef";
                    out += "\\u00";
                    out += digits[(static_cast<unsigned char>(c) >> 4) & 0x0F];
                    out += digits[static_cast<unsigned char>(c) & 0x0F];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}
