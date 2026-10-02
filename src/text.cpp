#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace doomnet {

bool utf8_to_wide_strict(const char* bytes, size_t length, std::wstring& out) {
    out.clear();
    if (length == 0) {
        return true;
    }
    if (length > 0x7fffffffu) {
        return false;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(length), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(length), out.data(), needed);
    if (written != needed) {
        out.clear();
        return false;
    }
    return true;
}

bool lua_path_to_wide(const char* bytes, size_t length, std::wstring& out) {
    out.clear();
    if (bytes == nullptr || length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == '\0') {
            return false;
        }
    }
    if (utf8_to_wide_strict(bytes, length, out)) {
        return true;
    }
    if (length > 0x7fffffffu) {
        return false;
    }
    const int needed = MultiByteToWideChar(CP_ACP, 0, bytes, static_cast<int>(length), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, bytes, static_cast<int>(length), out.data(), needed) != needed) {
        out.clear();
        return false;
    }
    return true;
}

std::wstring utf8_to_wide_lossy(const std::string& text) {
    if (text.empty() || text.size() > 0x7fffffffu) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty() || text.size() > 0x7fffffffu) {
        return std::string();
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string sanitize_utf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    auto cont = [&](size_t at) { return at < n && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80; };
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const bool overlong = c == 0xE0 && c1 < 0xA0;
            const bool surrogate = c == 0xED && c1 >= 0xA0;
            len = (overlong || surrogate) ? 0 : 3;
        } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const bool overlong = c == 0xF0 && c1 < 0x90;
            const bool too_big = c == 0xF4 && c1 >= 0x90;
            len = (overlong || too_big) ? 0 : 4;
        }
        if (len == 0) {
            out.append("\xEF\xBF\xBD");
            ++i;
        } else {
            out.append(text, i, len);
            i += len;
        }
    }
    return out;
}

}  // namespace doomnet
