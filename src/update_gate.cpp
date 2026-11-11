// Spec: production updates stay off for source trees and when config says off.

#include "update_gate.h"

#include "text.h"
#include "wide_io.h"

#include <algorithm>
#include <cctype>
#include <cwctype>

namespace doomnet {
namespace {

std::wstring lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

bool starts_with_ci(const std::wstring& value, const std::wstring& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return lower_copy(value.substr(0, prefix.size())) == lower_copy(prefix);
}

bool parse_version_string(const std::string& json, const char* key, std::wstring& out, std::wstring& error) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_at = json.find(needle);
    if (key_at == std::string::npos) {
        out.clear();
        return true;
    }
    size_t colon = json.find(':', key_at + needle.size());
    if (colon == std::string::npos) {
        error = L"versions.json key '" + utf8_to_wide_lossy(std::string(key)) + L"' is missing a value.";
        return false;
    }
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
        ++i;
    }
    if (i >= json.size() || json[i] != '"') {
        error = L"versions.json key '" + utf8_to_wide_lossy(std::string(key)) + L"' must be a string.";
        return false;
    }
    ++i;
    std::string raw;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\') {
            ++i;
            if (i >= json.size()) {
                error = L"versions.json has a truncated escape in '" + utf8_to_wide_lossy(std::string(key)) + L"'.";
                return false;
            }
        }
        raw.push_back(json[i++]);
    }
    if (i >= json.size()) {
        error = L"versions.json key '" + utf8_to_wide_lossy(std::string(key)) + L"' is missing its closing quote.";
        return false;
    }
    if (raw.size() > 64) {
        error = L"versions.json key '" + utf8_to_wide_lossy(std::string(key)) + L"' is longer than 64 characters.";
        return false;
    }
    for (unsigned char ch : raw) {
        if (ch < 0x20 || ch == '"' || ch == '\\') {
            error = L"versions.json key '" + utf8_to_wide_lossy(std::string(key)) + L"' has a character the installer refuses.";
            return false;
        }
    }
    out = utf8_to_wide_lossy(raw);
    return true;
}

}  // namespace

bool path_is_dev_source_tree(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const std::wstring value = lower_copy(path);
    static const wchar_t* roots[] = {
        L"d:\\documents\\repos\\doomnet-steamportal",
        L"d:\\documents\\repos\\steamtunes",
        L"d:\\documents\\repos\\steamskin",
    };
    for (const wchar_t* root : roots) {
        if (starts_with_ci(value, root)) {
            return true;
        }
    }
    return false;
}

bool updates_should_run(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled) {
    if (!config_updates_enabled) {
        return false;
    }
    if (path_is_dev_source_tree(exe_path) || path_is_dev_source_tree(data_root)) {
        return false;
    }
    return true;
}

bool installed_host_leaves_when_steam_exits(const std::wstring& exe_path) {
    return !exe_path.empty() && !path_is_dev_source_tree(exe_path);
}

bool load_installed_versions(const std::wstring& path, InstalledVersions& out, std::wstring& error) {
    error.clear();
    std::string text;
    if (!read_entire_file(path, text, error)) {
        error = L"Could not read installed versions from '" + path + L"'. " + error;
        return false;
    }
    InstalledVersions parsed;
    if (!parse_version_string(text, "doomnet", parsed.doomnet, error) ||
        !parse_version_string(text, "steamtunes", parsed.steamtunes, error) ||
        !parse_version_string(text, "steamskin", parsed.steamskin, error)) {
        return false;
    }
    out = parsed;
    return true;
}

bool save_installed_versions(const std::wstring& path, const InstalledVersions& versions, std::wstring& error) {
    error.clear();
    auto quote = [](const std::wstring& value) {
        std::string out = "\"";
        const std::string utf8 = wide_to_utf8(value);
        for (unsigned char ch : utf8) {
            if (ch == '\\' || ch == '"') out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        }
        out.push_back('"');
        return out;
    };
    std::string json = "{\n  \"doomnet\": ";
    json += quote(versions.doomnet);
    json += ",\n  \"steamtunes\": ";
    json += quote(versions.steamtunes);
    json += ",\n  \"steamskin\": ";
    json += quote(versions.steamskin);
    json += "\n}\n";
    if (!write_new_file(path, json.data(), json.size(), error)) {
        error = L"Could not write installed versions to '" + path + L"'. " + error;
        return false;
    }
    return true;
}

}  // namespace doomnet
