// Spec: Process model, Installer.
// config.json: steamPath (string), player (bool), skin (bool),
// playerColor ("steam" or "red"), optional streamTunesRelay, and optional updates.
// Unknown keys and malformed values fail
// the load. The file is capped at 64 KiB so a bad write cannot pin memory.

#include "config.h"

#include "anchors.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace doomnet {
namespace {

constexpr DWORD kMaxConfigBytes = 64 * 1024;

struct Parser {
    const wchar_t* cur = nullptr;
    const wchar_t* end = nullptr;
    std::wstring error;
    bool failed = false;
};

void fail(Parser& parser, const std::wstring& message) {
    if (!parser.failed) {
        parser.failed = true;
        parser.error = message;
    }
}

void skip_ws(Parser& parser) {
    while (parser.cur < parser.end) {
        const wchar_t ch = *parser.cur;
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') {
            break;
        }
        ++parser.cur;
    }
}

bool consume(Parser& parser, wchar_t expected) {
    skip_ws(parser);
    if (parser.cur >= parser.end || *parser.cur != expected) {
        const wchar_t found = parser.cur < parser.end ? *parser.cur : L'?';
        const wchar_t* where = parser.cur < parser.end ? L"" : L" (the file ended)";
        wchar_t message[160] = {};
        swprintf_s(message, L"config.json expected '%c' and found '%c'%s. The file was not applied.", expected, found, where);
        fail(parser, message);
        return false;
    }
    ++parser.cur;
    return true;
}

int hex_value(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f') {
        return ch - L'a' + 10;
    }
    if (ch >= L'A' && ch <= L'F') {
        return ch - L'A' + 10;
    }
    return -1;
}

bool parse_string(Parser& parser, std::wstring& out) {
    out.clear();
    if (!consume(parser, L'"')) {
        return false;
    }
    while (parser.cur < parser.end) {
        const wchar_t ch = *parser.cur++;
        if (ch == L'"') {
            return true;
        }
        if (ch == L'\\') {
            if (parser.cur >= parser.end) {
                fail(parser, L"config.json has a '\\' at the end of a string. The file was not applied.");
                return false;
            }
            const wchar_t esc = *parser.cur++;
            switch (esc) {
            case L'"':
            case L'\\':
            case L'/':
                out.push_back(esc);
                break;
            case L'n':
                out.push_back(L'\n');
                break;
            case L'r':
                out.push_back(L'\r');
                break;
            case L't':
                out.push_back(L'\t');
                break;
            case L'u': {
                int code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (parser.cur >= parser.end) {
                        fail(parser, L"config.json has a \\u escape that is shorter than four hex digits. The file was not applied.");
                        return false;
                    }
                    const int digit = hex_value(*parser.cur++);
                    if (digit < 0) {
                        fail(parser, L"config.json has a \\u escape that is not four hex digits. The file was not applied.");
                        return false;
                    }
                    code = (code << 4) | digit;
                }
                if (code == 0) {
                    fail(parser, L"config.json contains a NUL character inside a string. The file was not applied.");
                    return false;
                }
                out.push_back(static_cast<wchar_t>(code));
                break;
            }
            default:
                wchar_t message[120] = {};
                swprintf_s(message, L"config.json has an unknown escape '\\%c' inside a string. The file was not applied.", esc);
                fail(parser, message);
                return false;
            }
            continue;
        }
        if (ch < 0x20) {
            fail(parser, L"config.json has a raw control character inside a string. Use a \\u escape. The file was not applied.");
            return false;
        }
        out.push_back(ch);
    }
    fail(parser, L"config.json has a string that never closes. The file was not applied.");
    return false;
}

bool parse_bool(Parser& parser, bool& out) {
    skip_ws(parser);
    if (parser.end - parser.cur >= 4 && wcsncmp(parser.cur, L"true", 4) == 0) {
        parser.cur += 4;
        out = true;
        return true;
    }
    if (parser.end - parser.cur >= 5 && wcsncmp(parser.cur, L"false", 5) == 0) {
        parser.cur += 5;
        out = false;
        return true;
    }
    fail(parser, L"config.json expected true or false and found something else. The file was not applied.");
    return false;
}

bool has_dotdot(const std::wstring& path) {
    return path.find(L"..") != std::wstring::npos;
}

bool valid_streamtunes_relay(const std::wstring& value) {
    if (value.empty()) {
        return true;
    }
    if (value.size() > 512 || value.find_first_of(L"\r\n\t") != std::wstring::npos) {
        return false;
    }
    return value.rfind(L"wss://", 0) == 0 ||
           value.rfind(L"ws://127.0.0.1", 0) == 0 ||
           value.rfind(L"ws://localhost", 0) == 0;
}

bool parse_color(const std::wstring& text, PlayerColor& color) {
    if (text == L"steam") {
        color = PlayerColor::Steam;
        return true;
    }
    if (text == L"red") {
        color = PlayerColor::Red;
        return true;
    }
    return false;
}

bool wide_from_utf8(const char* data, size_t length, std::wstring& out) {
    if (length == 0) {
        out.clear();
        return true;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0);
    if (chars <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(chars), L'\0');
    const int wrote = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), out.data(), chars);
    return wrote == chars;
}

bool utf8_from_wide(const std::wstring& in, std::string& out) {
    if (in.empty()) {
        out.clear();
        return true;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return false;
    }
    out.assign(static_cast<size_t>(bytes), '\0');
    const int wrote = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, in.c_str(), static_cast<int>(in.size()), out.data(), bytes, nullptr, nullptr);
    return wrote == bytes;
}

void append_json_string(std::string& out, const std::wstring& text) {
    std::string utf8;
    if (!utf8_from_wide(text, utf8)) {
        utf8.clear();
    }
    out.push_back('"');
    for (unsigned char ch : utf8) {
        switch (ch) {
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
            if (ch < 0x20) {
                char buf[8] = {};
                sprintf_s(buf, "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

}  // namespace

ConfigLoad load_config_file(const std::wstring& path, Config& out, std::wstring& error) {
    error.clear();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            return ConfigLoad::Missing;
        }
        error = L"Could not open '" + path + L"'. " + windows_error_text(GetLastError()) + L" The in-memory defaults were left unchanged.";
        return ConfigLoad::Error;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > kMaxConfigBytes) {
        CloseHandle(handle);
        error = L"Refusing '" + path + L"' because it is " + std::to_wstring(size.QuadPart) + L" bytes. The limit is 65536 bytes so a damaged file cannot use unbounded memory. The in-memory defaults were left unchanged.";
        return ConfigLoad::Error;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = size.QuadPart == 0 || ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(handle);
    if (!ok || read != bytes.size()) {
        error = L"Could not read '" + path + L"' after it opened. " + windows_error_text(GetLastError()) + L" The in-memory defaults were left unchanged.";
        return ConfigLoad::Error;
    }
    std::wstring text;
    if (!wide_from_utf8(bytes.data(), bytes.size(), text)) {
        error = L"'" + path + L"' is not valid UTF-8, so none of its values were applied. Save it as UTF-8 and try again.";
        return ConfigLoad::Error;
    }

    Parser parser{text.data(), text.data() + text.size(), L"", false};
    if (!consume(parser, L'{')) {
        error = parser.error.empty() ? L"config.json must be a JSON object starting with '{'. The file was not applied." : parser.error;
        return ConfigLoad::Error;
    }
    Config parsed;
    bool seen_path = false;
    bool seen_player = false;
    bool seen_skin = false;
    bool seen_color = false;
    bool seen_streamtunes_relay = false;
    bool seen_updates = false;
    skip_ws(parser);
    if (parser.cur < parser.end && *parser.cur == L'}') {
        ++parser.cur;
    } else {
        while (!parser.failed) {
            std::wstring key;
            if (!parse_string(parser, key) || !consume(parser, L':')) {
                break;
            }
            if (key == L"steamPath") {
                if (seen_path || !parse_string(parser, parsed.steam_path)) {
                    fail(parser, L"config.json lists steamPath more than once or the value is not a string. The file was not applied.");
                    break;
                }
                if (has_dotdot(parsed.steam_path)) {
                    fail(parser, L"config.json steamPath '" + parsed.steam_path + L"' contains '..', which could leave the intended folder. The file was not applied.");
                    break;
                }
                seen_path = true;
            } else if (key == L"player") {
                if (seen_player || !parse_bool(parser, parsed.player)) {
                    fail(parser, L"config.json lists player more than once or the value is not true or false. The file was not applied.");
                    break;
                }
                seen_player = true;
            } else if (key == L"skin") {
                if (seen_skin || !parse_bool(parser, parsed.skin)) {
                    fail(parser, L"config.json lists skin more than once or the value is not true or false. The file was not applied.");
                    break;
                }
                seen_skin = true;
            } else if (key == L"playerColor") {
                std::wstring color;
                if (seen_color || !parse_string(parser, color) || !parse_color(color, parsed.player_color)) {
                    const std::wstring shown = color.empty() ? L"(missing)" : color;
                    fail(parser, L"config.json playerColor is '" + shown + L"'. It must be 'steam' (Steam Blue) or 'red' (match Steam-Skin). The file was not applied.");
                    break;
                }
                seen_color = true;
            } else if (key == L"streamTunesRelay") {
                if (seen_streamtunes_relay || !parse_string(parser, parsed.streamtunes_relay) || !valid_streamtunes_relay(parsed.streamtunes_relay)) {
                    fail(parser, L"config.json streamTunesRelay must be empty, an https-secured WebSocket URL starting with 'wss://', or a local test URL. The file was not applied.");
                    break;
                }
                seen_streamtunes_relay = true;
            } else if (key == L"updates") {
                if (seen_updates || !parse_bool(parser, parsed.updates)) {
                    fail(parser, L"config.json lists updates more than once or the value is not true or false. The file was not applied.");
                    break;
                }
                seen_updates = true;
            } else {
                fail(parser, L"config.json key '" + key + L"' is not recognized. The only keys are steamPath, player, skin, playerColor, streamTunesRelay, and updates. The file was not applied.");
                break;
            }
            skip_ws(parser);
            if (parser.cur < parser.end && *parser.cur == L',') {
                ++parser.cur;
                continue;
            }
            if (!consume(parser, L'}')) {
                break;
            }
            break;
        }
    }
    skip_ws(parser);
    if (parser.failed || parser.cur != parser.end) {
        error = parser.error.empty() ? L"config.json has extra text after the closing '}'. The file was not applied." : parser.error;
        return ConfigLoad::Error;
    }
    out = parsed;
    return ConfigLoad::Ok;
}

bool save_config_file(const std::wstring& path, const Config& config, std::wstring& error) {
    error.clear();
    if (has_dotdot(config.steam_path)) {
        error = L"Refusing to save config.json because steamPath '" + config.steam_path + L"' contains '..'.";
        return false;
    }
    std::string json = "{\n  \"steamPath\": ";
    append_json_string(json, config.steam_path);
    json += ",\n  \"player\": ";
    json += config.player ? "true" : "false";
    json += ",\n  \"skin\": ";
    json += config.skin ? "true" : "false";
    json += ",\n  \"playerColor\": ";
    json += config.player_color == PlayerColor::Red ? "\"red\"" : "\"steam\"";
    json += ",\n  \"streamTunesRelay\": ";
    append_json_string(json, config.streamtunes_relay);
    json += ",\n  \"updates\": ";
    json += config.updates ? "true" : "false";
    json += "\n}\n";
    if (json.size() > kMaxConfigBytes) {
        error = L"Refusing to save '" + path + L"' because the JSON would be " + std::to_wstring(json.size()) + L" bytes, over the 65536 byte limit.";
        return false;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"Could not write '" + path + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(handle);
    if (!ok || written != json.size()) {
        error = L"The write to '" + path + L"' stopped after " + std::to_wstring(written) + L" of " + std::to_wstring(json.size()) + L" bytes. The file may be incomplete.";
        return false;
    }
    return true;
}

void config_anchor() {}

}  // namespace doomnet
