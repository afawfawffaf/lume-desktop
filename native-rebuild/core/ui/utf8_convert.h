#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// UTF-8 to UTF-16 for the page protocol.
//
// Bytes are decoded as UTF-8, not widened one byte at a time. A byte-wise
// widening turns every multi-byte sequence into two or three unrelated
// characters, which reads as mojibake once the string reaches WebView. Invalid
// input becomes U+FFFD so one bad byte cannot truncate the message.
namespace lume::ui {

inline std::wstring ToWide(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index++]);
        std::uint32_t codepoint = 0;
        std::size_t continuation = 0;
        if (first <= 0x7F) codepoint = first;
        else if ((first & 0xE0) == 0xC0) { codepoint = first & 0x1F; continuation = 1; }
        else if ((first & 0xF0) == 0xE0) { codepoint = first & 0x0F; continuation = 2; }
        else if ((first & 0xF8) == 0xF0) { codepoint = first & 0x07; continuation = 3; }
        else { result.push_back(L'\uFFFD'); continue; }
        bool valid = index + continuation <= value.size();
        for (std::size_t part = 0; valid && part < continuation; ++part) {
            const auto next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xC0) != 0x80) valid = false;
            else codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (!valid || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            result.push_back(L'\uFFFD');
            continue;
        }
        if (codepoint <= 0xFFFF) {
            result.push_back(static_cast<wchar_t>(codepoint));
        } else {
            codepoint -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
        }
    }
    return result;
}

}  // namespace lume::ui
