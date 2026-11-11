#include "catalog.h"

#include "config.h"
#include "log.h"
#include "paths.h"
#include "text.h"
#include "wide_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace doomnet {
namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool extract_string(const std::string& json, const char* key, std::string& out, std::wstring& error) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) {
        error = L"The production manifest is missing '" + std::wstring(key, key + strlen(key)) + L"'.";
        return false;
    }
    const size_t colon = json.find(':', at + needle.size());
    if (colon == std::string::npos) {
        error = L"The production manifest key '" + std::wstring(key, key + strlen(key)) + L"' has no value.";
        return false;
    }
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
        ++i;
    }
    if (i >= json.size() || json[i] != '"') {
        error = L"The production manifest key '" + std::wstring(key, key + strlen(key)) + L"' must be a string.";
        return false;
    }
    ++i;
    std::string value;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\') {
            ++i;
            if (i >= json.size()) {
                error = L"The production manifest has a truncated escape.";
                return false;
            }
        }
        value.push_back(json[i++]);
    }
    if (i >= json.size() || value.size() > 2048) {
        error = L"The production manifest key '" + std::wstring(key, key + strlen(key)) + L"' is missing its closing quote or is too long.";
        return false;
    }
    out = value;
    return true;
}

bool piece_from(const std::string& block, ManifestPiece& piece, std::wstring& error) {
    std::string version;
    std::string url;
    std::string sha;
    if (!extract_string(block, "version", version, error) || !extract_string(block, "url", url, error) || !extract_string(block, "sha256", sha, error)) {
        return false;
    }
    if (version.empty() || version.size() > 64 || url.rfind("https://", 0) != 0 || sha.size() != 64) {
        error = L"A production manifest piece needs a version, an https URL, and a 64 character sha256.";
        return false;
    }
    for (char ch : sha) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            error = L"A production manifest sha256 must be hexadecimal.";
            return false;
        }
    }
    piece.version.assign(version.begin(), version.end());
    piece.url.assign(url.begin(), url.end());
    piece.sha256 = lower_ascii(sha);
    return true;
}

bool object_for(const std::string& json, const char* name, std::string& block, std::wstring& error) {
    const std::string needle = std::string("\"") + name + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) {
        error = L"The production manifest is missing the '" + std::wstring(name, name + strlen(name)) + L"' piece.";
        return false;
    }
    const size_t open = json.find('{', at + needle.size());
    const size_t close = open == std::string::npos ? std::string::npos : json.find('}', open);
    if (open == std::string::npos || close == std::string::npos) {
        error = L"The production manifest piece '" + std::wstring(name, name + strlen(name)) + L"' is not an object.";
        return false;
    }
    block = json.substr(open, close - open + 1);
    return true;
}

std::string hex_of(const unsigned char* bytes, size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.resize(count * 2);
    for (size_t i = 0; i < count; ++i) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return text;
}

bool versions_differ(const std::wstring& installed, const std::wstring& published) {
    return !installed.empty() && !published.empty() && installed != published;
}

}  // namespace

bool parse_release_manifest(const std::string& json, ReleaseManifest& out, std::wstring& error) {
    error.clear();
    ReleaseManifest parsed;
    std::string block;
    if (!object_for(json, "doomnet", block, error) || !piece_from(block, parsed.doomnet, error) ||
        !object_for(json, "steamtunes", block, error) || !piece_from(block, parsed.steamtunes, error) ||
        !object_for(json, "steamskin", block, error) || !piece_from(block, parsed.steamskin, error)) {
        return false;
    }
    out = parsed;
    return true;
}

UpdatePlan updates_needed(const ReleaseManifest& manifest, const InstalledVersions& installed) {
    UpdatePlan plan;
    plan.doomnet = versions_differ(installed.doomnet, manifest.doomnet.version);
    plan.steamtunes = versions_differ(installed.steamtunes, manifest.steamtunes.version);
    plan.steamskin = versions_differ(installed.steamskin, manifest.steamskin.version);
    return plan;
}

std::string sha256_hex(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD ignored = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &ignored, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::string object(object_size, '\0');
    if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_size, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    const NTSTATUS hashed = BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
    unsigned char digest[32] = {};
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, digest, sizeof(digest), 0) : hashed;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finished != 0) {
        return {};
    }
    return hex_of(digest, sizeof(digest));
}

bool sha256_file(const std::wstring& path, std::string& hex, std::wstring& error) {
    hex.clear();
    error.clear();
    if (path.empty() || path.find(L"..") != std::wstring::npos) {
        error = L"Refusing to hash '" + path + L"' because the path is empty or contains '..'.";
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not hash '" + path + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD ignored = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &ignored, 0) != 0) {
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        CloseHandle(file);
        error = L"Could not hash '" + path + L"' because SHA-256 did not open.";
        return false;
    }
    std::string object(object_size, '\0');
    if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_size, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        error = L"Could not hash '" + path + L"' because SHA-256 did not start.";
        return false;
    }
    unsigned char buffer[64 * 1024];
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) {
            error = L"Could not hash '" + path + L"'. " + windows_error_text(GetLastError());
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (BCryptHashData(hash, buffer, got, 0) != 0) {
            error = L"Could not hash '" + path + L"' because SHA-256 stopped.";
            ok = false;
            break;
        }
    }
    unsigned char digest[32] = {};
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        error = L"Could not hash '" + path + L"' because SHA-256 did not finish.";
        ok = false;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    hex = hex_of(digest, sizeof(digest));
    return hex.size() == 64;
}

bool payload_hash_matches(const std::wstring& path, const std::string& expected_sha256, std::wstring& error) {
    error.clear();
    std::string actual;
    if (!sha256_file(path, actual, error)) {
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    if (lower_ascii(actual) != lower_ascii(expected_sha256)) {
        error = L"The hash of '" + path + L"' did not match the production manifest. The install was not replaced.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    const std::wstring detail = L"The hash of '" + path + L"' matched the production manifest.";
    log_event(L"info", L"installer", detail.c_str());
    return true;
}

bool download_https(const std::wstring& url, const std::wstring& destination, size_t max_bytes, std::wstring& error) {
    error.clear();
    if (url.rfind(L"https://", 0) != 0) {
        error = L"Refusing to download '" + url + L"' because a production payload must use https. Nothing was written.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    if (destination.empty() || destination.find(L"..") != std::wstring::npos) {
        error = L"Refusing to download '" + url + L"' into '" + destination + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"Refusing to download '" + url + L"' because it is not an https URL. Nothing was written.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    HINTERNET session = WinHttpOpen(L"DoomNet", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        error = L"Could not open a download for '" + url + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection == nullptr ? nullptr : WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    const bool sent = request != nullptr && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!sent || !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        error = L"The production download of '" + url + L"' did not return the payload, so nothing was replaced.";
        log_event(L"error", L"installer", error.c_str());
        if (request != nullptr) {
            WinHttpCloseHandle(request);
        }
        if (connection != nullptr) {
            WinHttpCloseHandle(connection);
        }
        WinHttpCloseHandle(session);
        return false;
    }
    HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not create '" + destination + L"' for '" + url + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"installer", error.c_str());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    size_t total = 0;
    bool ok = true;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = L"The production download of '" + url + L"' stopped before the file was complete. " + windows_error_text(GetLastError());
            ok = false;
            break;
        }
        if (available == 0) {
            break;
        }
        if (total + available > max_bytes) {
            error = L"The production download of '" + url + L"' is larger than the installer allows. Nothing was replaced.";
            ok = false;
            break;
        }
        std::string buffer(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &got)) {
            error = L"The production download of '" + url + L"' could not be read. " + windows_error_text(GetLastError());
            ok = false;
            break;
        }
        DWORD written = 0;
        if (got > 0 && (!WriteFile(file, buffer.data(), got, &written, nullptr) || written != got)) {
            error = L"The production download of '" + url + L"' could not be saved to '" + destination + L"'. " + windows_error_text(GetLastError());
            ok = false;
            break;
        }
        total += got;
    }
    CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok) {
        DeleteFileW(destination.c_str());
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    log_event(L"info", L"installer", (L"Downloaded '" + url + L"' to '" + destination + L"'.").c_str());
    return true;
}

bool replace_file_if_hash_matches(const std::wstring& source, const std::wstring& destination, const std::string& expected_sha256, std::wstring& error) {
    if (!payload_hash_matches(source, expected_sha256, error)) {
        return false;
    }
    if (destination.empty() || destination.find(L"..") != std::wstring::npos) {
        error = L"Refusing to replace '" + destination + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        error = L"Could not replace '" + destination + L"' with the verified payload '" + source + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    log_event(L"info", L"installer", (L"Replaced '" + destination + L"' with the verified payload '" + source + L"'.").c_str());
    return true;
}

namespace {

enum class ApplyResult { Applied, Retry, Rejected };

std::wstring pending_path(const std::wstring& data_root) {
    return data_root + L"\\updates\\pending.txt";
}

std::wstring payload_path(const std::wstring& data_root, const std::wstring& name) {
    return data_root + L"\\updates\\" + name + L".bin";
}

bool read_u16(const std::string& bytes, size_t at, unsigned& out) {
    if (at + 2 > bytes.size()) {
        return false;
    }
    out = static_cast<unsigned char>(bytes[at]) | (static_cast<unsigned char>(bytes[at + 1]) << 8);
    return true;
}

bool read_u32(const std::string& bytes, size_t at, unsigned& out) {
    if (at + 4 > bytes.size()) {
        return false;
    }
    out = static_cast<unsigned char>(bytes[at]) | (static_cast<unsigned char>(bytes[at + 1]) << 8) | (static_cast<unsigned char>(bytes[at + 2]) << 16) | (static_cast<unsigned char>(bytes[at + 3]) << 24);
    return true;
}

bool read_capped(const std::wstring& path, size_t limit, std::string& out, std::wstring& error) {
    out.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not read '" + path + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit) {
        CloseHandle(file);
        error = L"'" + path + L"' is larger than the updater will open.";
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t filled = 0;
    while (filled < out.size()) {
        DWORD got = 0;
        const size_t remaining = out.size() - filled;
        const DWORD chunk = remaining > 1024u * 1024u ? 1024u * 1024u : static_cast<DWORD>(remaining);
        if (!ReadFile(file, out.data() + filled, chunk, &got, nullptr)) {
            CloseHandle(file);
            out.clear();
            error = L"Could not read '" + path + L"'. " + windows_error_text(GetLastError());
            return false;
        }
        if (got == 0) {
            break;
        }
        filled += got;
    }
    CloseHandle(file);
    out.resize(filled);
    return true;
}

bool zip_name_ok(const std::string& name, std::wstring& wide) {
    wide.clear();
    if (name.empty() || name.size() > 240 || name.back() == '/') {
        return false;
    }
    if (!utf8_to_wide_strict(name.data(), name.size(), wide)) {
        return false;
    }
    for (wchar_t& ch : wide) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    if (wide.size() >= 2 && wide[1] == L':') {
        return false;
    }
    if (wide[0] == L'\\' || wide[0] == L'/') {
        return false;
    }
    size_t start = 0;
    while (start <= wide.size()) {
        const size_t slash = wide.find(L'\\', start);
        const std::wstring part = wide.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
        if (slash == std::wstring::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool remove_tree(const std::wstring& path, int depth, std::wstring& error) {
    if (depth > 32 || path.find(L"..") != std::wstring::npos) {
        error = L"Refusing to remove '" + path + L"'.";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = L"Refusing to remove reparse point '" + path + L"'.";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") {
                    continue;
                }
                if (!remove_tree(path + L"\\" + name, depth + 1, error)) {
                    FindClose(find);
                    return false;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        if (!RemoveDirectoryW(path.c_str())) {
            error = L"Could not remove '" + path + L"'. " + windows_error_text(GetLastError());
            return false;
        }
        return true;
    }
    if (!DeleteFileW(path.c_str())) {
        error = L"Could not remove '" + path + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    return true;
}

bool extract_stored_zip(const std::string& bytes, const std::wstring& dest, std::wstring& error) {
    if (bytes.size() < 22) {
        error = L"The update is not a zip file, so nothing was replaced.";
        return false;
    }
    size_t eocd = std::string::npos;
    const size_t begin = bytes.size() > 66000 ? bytes.size() - 66000 : 0;
    for (size_t i = bytes.size() - 22; ; --i) {
        unsigned sig = 0;
        if (read_u32(bytes, i, sig) && sig == 0x06054b50u) {
            eocd = i;
            break;
        }
        if (i == begin) {
            break;
        }
    }
    unsigned count = 0;
    unsigned cd_off = 0;
    if (eocd == std::string::npos || !read_u16(bytes, eocd + 10, count) || !read_u32(bytes, eocd + 16, cd_off)) {
        error = L"The update zip has no directory, so nothing was replaced.";
        return false;
    }
    if (!remove_tree(dest, 0, error) || !ensure_directory(dest)) {
        return false;
    }
    size_t cursor = cd_off;
    for (unsigned n = 0; n < count; ++n) {
        unsigned sig = 0;
        unsigned method = 0;
        unsigned flags = 0;
        unsigned size = 0;
        unsigned name_len = 0;
        unsigned extra_len = 0;
        unsigned comment_len = 0;
        unsigned local_off = 0;
        if (!read_u32(bytes, cursor, sig) || sig != 0x02014b50u || !read_u16(bytes, cursor + 8, flags) || !read_u16(bytes, cursor + 10, method) || !read_u32(bytes, cursor + 20, size) || !read_u16(bytes, cursor + 28, name_len) || !read_u16(bytes, cursor + 30, extra_len) || !read_u16(bytes, cursor + 32, comment_len) || !read_u32(bytes, cursor + 42, local_off)) {
            error = L"The update zip could not be read, so nothing was replaced.";
            return false;
        }
        if (cursor + 46 + name_len > bytes.size()) {
            error = L"The update zip could not be read, so nothing was replaced.";
            return false;
        }
        const std::string name = bytes.substr(cursor + 46, name_len);
        cursor += 46 + name_len + extra_len + comment_len;
        if (!name.empty() && name.back() == '/') {
            continue;
        }
        std::wstring relative;
        if ((flags & 1u) != 0 || (flags & 8u) != 0 || method != 0 || !zip_name_ok(name, relative)) {
            error = L"The update zip contains '" + utf8_to_wide_lossy(name) + L"', which the updater will not unpack. Nothing was replaced.";
            return false;
        }
        unsigned local_name = 0;
        unsigned local_extra = 0;
        unsigned local_sig = 0;
        if (!read_u32(bytes, local_off, local_sig) || local_sig != 0x04034b50u || !read_u16(bytes, local_off + 26, local_name) || !read_u16(bytes, local_off + 28, local_extra)) {
            error = L"The update zip could not be read, so nothing was replaced.";
            return false;
        }
        const size_t data_at = static_cast<size_t>(local_off) + 30 + local_name + local_extra;
        if (data_at + size > bytes.size()) {
            error = L"The update zip is shorter than it claims, so nothing was replaced.";
            return false;
        }
        const std::wstring target = dest + L"\\" + relative;
        const size_t slash = target.find_last_of(L'\\');
        std::wstring folder = slash == std::wstring::npos ? dest : target.substr(0, slash);
        std::wstring made = dest;
        size_t part = dest.size();
        while (part < folder.size()) {
            const size_t next = folder.find(L'\\', part + 1);
            made = folder.substr(0, next == std::wstring::npos ? std::wstring::npos : next);
            if (!ensure_directory(made)) {
                error = L"Could not create '" + made + L"'. " + windows_error_text(GetLastError());
                return false;
            }
            if (next == std::wstring::npos) {
                break;
            }
            part = next;
        }
        std::wstring write_error;
        if (!write_new_file(target, bytes.data() + data_at, size, write_error)) {
            error = write_error;
            return false;
        }
    }
    return true;
}

bool find_named(const std::wstring& folder, const std::wstring& file_name, std::wstring& found, int depth) {
    if (depth > 8) {
        return false;
    }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = false;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        const std::wstring path = folder + L"\\" + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (find_named(path, file_name, found, depth + 1)) {
                ok = true;
                break;
            }
            continue;
        }
        if (_wcsicmp(name.c_str(), file_name.c_str()) == 0) {
            found = path;
            ok = true;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

bool copy_tree_filtered(const std::wstring& from, const std::wstring& to, bool skip_data, int depth, std::wstring& error) {
    if (depth > 32 || from.find(L"..") != std::wstring::npos || to.find(L"..") != std::wstring::npos) {
        error = L"Refusing to copy '" + from + L"' to '" + to + L"'.";
        return false;
    }
    if (!ensure_directory(to)) {
        error = L"Could not create '" + to + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        error = L"Could not list '" + from + L"'. " + windows_error_text(GetLastError());
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || name == L".git" || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        if (_wcsicmp(name.c_str(), L"settings.json") == 0) {
            continue;
        }
        const std::wstring source = from + L"\\" + name;
        const std::wstring dest = to + L"\\" + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (skip_data && _wcsicmp(name.c_str(), L"data") == 0) {
                continue;
            }
            if (!copy_tree_filtered(source, dest, skip_data, depth + 1, error)) {
                ok = false;
                break;
            }
            continue;
        }
        if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) {
            error = L"Could not copy '" + source + L"' to '" + dest + L"'. " + windows_error_text(GetLastError());
            ok = false;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

bool replace_program(const std::wstring& staged, const std::wstring& dest, std::wstring& error) {
    const std::wstring previous = dest + L".previous";
    DeleteFileW(previous.c_str());
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(dest.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"Could not move '" + dest + L"' aside. " + windows_error_text(GetLastError());
        return false;
    }
    if (!CopyFileW(staged.c_str(), dest.c_str(), FALSE)) {
        error = L"Could not copy '" + staged + L"' to '" + dest + L"'. " + windows_error_text(GetLastError());
        if (GetFileAttributesW(previous.c_str()) != INVALID_FILE_ATTRIBUTES) {
            MoveFileExW(previous.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    }
    return true;
}

bool remember_pending(const std::wstring& data_root, const std::wstring& name, const std::wstring& version, const std::string& sha256, std::wstring& error) {
    std::string text;
    std::wstring ignored;
    read_capped(pending_path(data_root), 64u * 1024u, text, ignored);
    std::string next;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t end = text.find('\n', cursor);
        const std::string line = text.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? text.size() : end + 1;
        if (line.rfind(wide_to_utf8(name) + " ", 0) == 0) {
            continue;
        }
        if (!line.empty()) {
            next += line;
            next.push_back('\n');
        }
    }
    next += wide_to_utf8(name);
    next.push_back(' ');
    next += wide_to_utf8(version);
    next.push_back(' ');
    next += sha256;
    next.push_back('\n');
    if (!ensure_directory(data_root + L"\\updates") || !write_new_file(pending_path(data_root), next.data(), next.size(), error)) {
        return false;
    }
    return true;
}

bool drop_pending(const std::wstring& data_root, const std::wstring& name) {
    std::string text;
    std::wstring ignored;
    if (!read_capped(pending_path(data_root), 64u * 1024u, text, ignored)) {
        return true;
    }
    std::string next;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t end = text.find('\n', cursor);
        std::string line = text.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(wide_to_utf8(name) + " ", 0) == 0 || line.empty()) {
            continue;
        }
        next += line;
        next.push_back('\n');
    }
    std::wstring error;
    if (next.empty()) {
        DeleteFileW(pending_path(data_root).c_str());
        return true;
    }
    return write_new_file(pending_path(data_root), next.data(), next.size(), error);
}

bool save_piece_version(const std::wstring& data_root, const std::wstring& name, const std::wstring& version, std::wstring& error) {
    InstalledVersions versions;
    if (!load_installed_versions(data_root + L"\\versions.json", versions, error)) {
        return false;
    }
    if (name == L"doomnet") {
        versions.doomnet = version;
    } else if (name == L"steamtunes") {
        versions.steamtunes = version;
    } else if (name == L"steamskin") {
        versions.steamskin = version;
    } else {
        error = L"Refusing to record an update named '" + name + L"'.";
        return false;
    }
    return save_installed_versions(data_root + L"\\versions.json", versions, error);
}

ApplyResult apply_piece(const std::wstring& data_root, const std::wstring& steam_folder, const std::wstring& name, const std::wstring& version, const std::wstring& zip_path) {
    std::wstring error;
    const std::wstring unpack = data_root + L"\\updates\\unpack-" + name;
    std::string bytes;
    if (!read_capped(zip_path, 64u * 1024u * 1024u, bytes, error) || !extract_stored_zip(bytes, unpack, error)) {
        log_event(L"error", L"installer", error.c_str());
        return ApplyResult::Rejected;
    }
    bool ok = false;
    if (name == L"doomnet") {
        std::wstring exe;
        std::wstring dll;
        if (!find_named(unpack, L"DoomNet.exe", exe, 0)) {
            log_event(L"error", L"installer", L"The DoomNet update does not contain DoomNet.exe, so the installed program was not replaced.");
            return ApplyResult::Rejected;
        }
        if (!replace_program(exe, data_root + L"\\DoomNet.exe", error)) {
            log_event(L"error", L"installer", error.c_str());
            return ApplyResult::Retry;
        }
        if (find_named(unpack, L"version.dll", dll, 0)) {
            if (!CopyFileW(dll.c_str(), (data_root + L"\\version.dll").c_str(), FALSE)) {
                log_event(L"warn", L"installer", (L"Could not copy version.dll to '" + data_root + L"'. " + windows_error_text(GetLastError())).c_str());
            }
            const std::wstring steam_dll = steam_folder.empty() ? std::wstring() : steam_folder + L"\\version.dll";
            if (!steam_dll.empty() && steam_client_running(steam_folder + L"\\steam.exe")) {
                log_event(L"info", L"installer", (L"Left '" + steam_dll + L"' in place because Steam is running. The updated program is '" + data_root + L"\\DoomNet.exe' and will load the next time Steam starts.").c_str());
            } else if (!steam_dll.empty() && !CopyFileW(dll.c_str(), steam_dll.c_str(), FALSE)) {
                log_event(L"warn", L"installer", (L"Could not replace '" + steam_dll + L"'. " + windows_error_text(GetLastError()) + L" The updated program is still '" + data_root + L"\\DoomNet.exe'.").c_str());
            }
        }
        ok = true;
        log_event(L"info", L"installer", (L"Replaced '" + data_root + L"\\DoomNet.exe' with version " + version + L". It will load the next time Steam starts.").c_str());
    } else if (name == L"steamtunes") {
        std::wstring plugin;
        if (!find_named(unpack, L"plugin.json", plugin, 0)) {
            log_event(L"error", L"installer", L"The SteamTunes update does not contain plugin.json, so the installed player was not replaced.");
            return ApplyResult::Rejected;
        }
        const size_t slash = plugin.find_last_of(L'\\');
        const std::wstring source = slash == std::wstring::npos ? unpack : plugin.substr(0, slash);
        if (GetFileAttributesW((source + L"\\backend\\main.lua").c_str()) == INVALID_FILE_ATTRIBUTES) {
            log_event(L"error", L"installer", L"The SteamTunes update does not contain backend\\main.lua, so the installed player was not replaced.");
            return ApplyResult::Rejected;
        }
        if (!copy_tree_filtered(source, data_root + L"\\player", true, 0, error)) {
            log_event(L"error", L"installer", error.c_str());
            return ApplyResult::Retry;
        }
        ok = true;
        log_event(L"info", L"installer", (L"Replaced the player in '" + data_root + L"\\player' with version " + version + L". Saved settings and the music library were left in place.").c_str());
    } else if (name == L"steamskin") {
        std::wstring skin_json;
        if (!find_named(unpack, L"skin.json", skin_json, 0)) {
            log_event(L"error", L"installer", L"The SteamSkin update does not contain skin.json, so the installed skin was not replaced.");
            return ApplyResult::Rejected;
        }
        const size_t slash = skin_json.find_last_of(L'\\');
        const std::wstring source = slash == std::wstring::npos ? unpack : skin_json.substr(0, slash);
        if (!copy_tree_filtered(source, data_root + L"\\skin", false, 0, error)) {
            log_event(L"error", L"installer", error.c_str());
            return ApplyResult::Retry;
        }
        ok = true;
        log_event(L"info", L"installer", (L"Replaced the skin in '" + data_root + L"\\skin' with version " + version + L".").c_str());
    } else {
        log_event(L"error", L"installer", (L"Refusing to apply an update named '" + name + L"'.").c_str());
        return ApplyResult::Rejected;
    }
    if (!ok || !save_piece_version(data_root, name, version, error)) {
        log_event(L"error", L"installer", error.empty() ? L"The update files were copied, but versions.json was not updated." : error.c_str());
        return ApplyResult::Retry;
    }
    return ApplyResult::Applied;
}

bool stage_piece(const std::wstring& data_root, const std::wstring& steam_folder, const wchar_t* name, const ManifestPiece& piece) {
    const std::wstring folder = data_root + L"\\updates";
    if (!ensure_directory(folder)) {
        const std::wstring detail = L"Could not create '" + folder + L"', so the " + name + L" payload was not downloaded.";
        log_event(L"error", L"installer", detail.c_str());
        return false;
    }
    const std::wstring dest = payload_path(data_root, name);
    std::wstring error;
    if (!download_https(piece.url, dest, 64u * 1024u * 1024u, error)) {
        return false;
    }
    if (!payload_hash_matches(dest, piece.sha256, error)) {
        DeleteFileW(dest.c_str());
        log_event(L"error", L"installer", (L"Deleted '" + dest + L"' because its hash did not match. The installed " + std::wstring(name) + L" files were not replaced.").c_str());
        return false;
    }
    const ApplyResult applied = apply_piece(data_root, steam_folder, name, piece.version, dest);
    if (applied == ApplyResult::Applied) {
        DeleteFileW(dest.c_str());
        drop_pending(data_root, name);
        return true;
    }
    if (applied == ApplyResult::Rejected) {
        DeleteFileW(dest.c_str());
        drop_pending(data_root, name);
        return false;
    }
    if (!remember_pending(data_root, name, piece.version, piece.sha256, error)) {
        log_event(L"error", L"installer", (L"The verified " + std::wstring(name) + L" payload is at '" + dest + L"', but it could not be saved for the next start. " + error).c_str());
        return false;
    }
    log_event(L"info", L"installer", (L"The verified " + std::wstring(name) + L" payload is at '" + dest + L"' and will replace the installed files the next time Steam starts.").c_str());
    return true;
}

}  // namespace

bool apply_pending_updates(const std::wstring& data_root, const std::wstring& steam_folder) {
    std::string text;
    std::wstring error;
    if (!read_capped(pending_path(data_root), 64u * 1024u, text, error)) {
        return true;
    }
    bool ok = true;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t end = text.find('\n', cursor);
        std::string line = text.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const size_t first = line.find(' ');
        const size_t second = first == std::string::npos ? std::string::npos : line.find(' ', first + 1);
        if (first == std::string::npos || second == std::string::npos || second + 1 + 64 != line.size()) {
            log_event(L"error", L"installer", L"An update waiting in pending.txt could not be read, so it was not applied.");
            ok = false;
            continue;
        }
        const std::wstring name = utf8_to_wide_lossy(line.substr(0, first));
        const std::wstring version = utf8_to_wide_lossy(line.substr(first + 1, second - first - 1));
        const std::string sha = line.substr(second + 1);
        const std::wstring bin = payload_path(data_root, name);
        if (!payload_hash_matches(bin, sha, error)) {
            DeleteFileW(bin.c_str());
            drop_pending(data_root, name);
            log_event(L"error", L"installer", (L"Deleted '" + bin + L"' because its hash did not match. The installed files were not replaced.").c_str());
            ok = false;
            continue;
        }
        const ApplyResult applied = apply_piece(data_root, steam_folder, name, version, bin);
        if (applied == ApplyResult::Applied) {
            DeleteFileW(bin.c_str());
            drop_pending(data_root, name);
        } else if (applied == ApplyResult::Rejected) {
            DeleteFileW(bin.c_str());
            drop_pending(data_root, name);
            ok = false;
        } else {
            ok = false;
        }
    }
    return ok;
}

UpdateCheckResult check_production_updates(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled, const std::wstring& steam_folder) {
    UpdateCheckResult result;
    if (!updates_should_run(exe_path, data_root, config_updates_enabled)) {
        log_event(L"info", L"installer", L"Production updates are off because this process is a source tree or config.json has updates set to false. No manifest was read.");
        result.message = L"Updates are off for this copy, so nothing was downloaded.";
        return result;
    }
    const wchar_t* published = L"https://github.com/DoomyMcDoomface/DoomNet-SteamPortal/releases/download/production/production-manifest.json";
    const std::wstring cached = data_root + L"\\updates\\production-manifest.json";
    const std::wstring local = data_root + L"\\production-manifest.json";
    std::string json;
    std::wstring error;
    bool have = false;
    if (ensure_directory(data_root + L"\\updates") && download_https(published, cached, 64u * 1024u, error) && read_entire_file(cached, json, error)) {
        have = true;
    }
    if (!have) {
        error.clear();
        have = read_entire_file(local, json, error);
    }
    if (!have) {
        log_event(L"info", L"installer", (L"Production updates are on, and neither '" + std::wstring(published) + L"' nor '" + local + L"' could be read, so no payload was replaced.").c_str());
        result.message = L"The update list could not be read, so nothing was replaced.";
        return result;
    }
    ReleaseManifest manifest;
    if (!parse_release_manifest(json, manifest, error)) {
        log_event(L"error", L"installer", error.c_str());
        result.message = error.empty() ? L"The update list could not be read, so nothing was replaced." : error;
        return result;
    }
    InstalledVersions installed;
    if (!load_installed_versions(data_root + L"\\versions.json", installed, error)) {
        log_event(L"warn", L"installer", error.c_str());
        result.message = error.empty() ? L"The installed versions could not be read, so nothing was replaced." : error;
        return result;
    }
    const UpdatePlan plan = updates_needed(manifest, installed);
    if (!plan.any()) {
        log_event(L"info", L"installer", L"The production manifest matches the installed versions. No payload was replaced.");
        result.message = L"This copy is already up to date.";
        return result;
    }
    publish_update_notice(L"A new version is available. It's downloading now.");
    std::wstring detail = L"A production update is ready.";
    if (plan.doomnet) {
        detail += L" DoomNet " + installed.doomnet + L" to " + manifest.doomnet.version + L".";
    }
    if (plan.steamtunes) {
        detail += L" SteamTunes " + installed.steamtunes + L" to " + manifest.steamtunes.version + L".";
    }
    if (plan.steamskin) {
        detail += L" SteamSkin " + installed.steamskin + L" to " + manifest.steamskin.version + L".";
    }
    detail += L" The payload is replaced only after its hash matches.";
    log_event(L"info", L"installer", detail.c_str());
    bool replaced = false;
    bool missed = false;
    if (plan.doomnet) {
        if (stage_piece(data_root, steam_folder, L"doomnet", manifest.doomnet)) {
            replaced = true;
        } else {
            missed = true;
        }
    }
    if (!missed && plan.steamtunes) {
        if (stage_piece(data_root, steam_folder, L"steamtunes", manifest.steamtunes)) {
            replaced = true;
        } else {
            missed = true;
        }
    }
    if (!missed && plan.steamskin) {
        if (stage_piece(data_root, steam_folder, L"steamskin", manifest.steamskin)) {
            replaced = true;
        } else {
            missed = true;
        }
    }
    if (!replaced) {
        result.message = L"A new version is available, but it could not be downloaded. Nothing was replaced.";
        publish_update_notice(result.message);
        return result;
    }
    result.restart_steam = true;
    publish_update_notice(missed ? L"Part of the new version was downloaded. Quit Steam and open it again to use it."
                                 : L"The new version was downloaded. Quit Steam and open it again to use it.");
    result.message = missed ? L"Part of the update was downloaded. Steam is restarting so it can load."
                            : L"An update was downloaded. Steam is restarting so it can load.";
    return result;
}

namespace {

std::mutex g_update_notice_mu;
std::wstring g_update_notice;

}  // namespace

void publish_update_notice(const std::wstring& message) {
    {
        std::lock_guard<std::mutex> lock(g_update_notice_mu);
        g_update_notice = message;
    }
    if (!message.empty()) {
        log_event(L"info", L"installer", message.c_str());
    }
}

std::wstring current_update_notice() {
    std::lock_guard<std::mutex> lock(g_update_notice_mu);
    return g_update_notice;
}

void note_production_update_gate(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled, const std::wstring& steam_folder) {
    check_production_updates(exe_path, data_root, config_updates_enabled, steam_folder);
}

namespace {

std::mutex g_update_check_mu;
bool g_update_check_busy = false;
bool g_update_check_done = false;
bool g_update_check_restart = false;
std::wstring g_update_check_message;
std::wstring g_update_check_steam;

std::wstring steam_exe_from(const std::wstring& steam_path) {
    std::wstring steam_exe = steam_path;
    if (steam_exe.size() < 9 || _wcsicmp(steam_exe.c_str() + (steam_exe.size() - 9), L"steam.exe") != 0) {
        if (!steam_exe.empty() && steam_exe.back() != L'\\' && steam_exe.back() != L'/') {
            steam_exe += L'\\';
        }
        steam_exe += L"steam.exe";
    }
    return steam_exe;
}

void restart_steam_after_update(std::wstring steam_path) {
    // The page shows the message first. Quitting immediately would close it before the toast.
    Sleep(2000);
    const std::wstring steam_exe = steam_exe_from(steam_path);
    std::wstring error;
    if (steam_client_running(steam_exe)) {
        if (!ask_steam_to_quit(steam_exe, error)) {
            log_event(L"error", L"installer", error.empty() ? L"Steam was not restarted after the update." : error.c_str());
            return;
        }
        for (int i = 0; i < 40; ++i) {
            Sleep(500);
            if (!steam_client_running(steam_exe)) {
                break;
            }
        }
    }
    if (steam_client_running(steam_exe)) {
        log_event(L"error", L"installer", (L"Steam was still running after the update, so '" + steam_exe + L"' was not started again.").c_str());
        return;
    }
    if (!start_steam_visible(steam_exe, error)) {
        log_event(L"error", L"installer", error.empty() ? (L"'" + steam_exe + L"' was not started after the update.").c_str() : error.c_str());
    }
}

}  // namespace

bool begin_store_update_check(const std::wstring& data_root, const std::wstring& steam_path) {
    {
        std::lock_guard<std::mutex> lock(g_update_check_mu);
        if (g_update_check_busy) {
            return false;
        }
        g_update_check_busy = true;
        g_update_check_done = false;
        g_update_check_restart = false;
        g_update_check_message.clear();
        g_update_check_steam = steam_path;
    }
    std::thread([data_root, steam_path]() {
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        bool updates = false;
        Config config;
        std::wstring error;
        if (load_config_file(data_root + L"\\config.json", config, error) == ConfigLoad::Ok) {
            updates = config.updates;
        }
        const UpdateCheckResult result = check_production_updates(exe_path, data_root, updates, steam_path);
        std::lock_guard<std::mutex> lock(g_update_check_mu);
        g_update_check_message = result.message;
        g_update_check_restart = result.restart_steam && !steam_path.empty();
        if (result.restart_steam && steam_path.empty()) {
            g_update_check_message = L"An update was downloaded. Steam was not restarted because no Steam folder is configured.";
        }
        g_update_check_done = true;
    }).detach();
    return true;
}

std::string take_store_update_check(std::wstring& message) {
    bool restart = false;
    std::wstring steam;
    {
        std::lock_guard<std::mutex> lock(g_update_check_mu);
        if (!g_update_check_done) {
            message.clear();
            return "wait";
        }
        message = g_update_check_message;
        restart = g_update_check_restart;
        steam = g_update_check_steam;
        g_update_check_done = false;
        g_update_check_restart = false;
        g_update_check_busy = false;
    }
    if (restart) {
        publish_update_notice(message.empty() ? L"An update was downloaded. Steam is restarting so it can load." : message);
        std::thread(restart_steam_after_update, steam).detach();
    }
    return "ready";
}

bool steam_client_running(const std::wstring& steam_exe) {
    if (steam_exe.empty()) {
        return false;
    }
    const size_t slash = steam_exe.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? std::wstring() : steam_exe.substr(0, slash);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") != 0 && _wcsicmp(entry.szExeFile, L"steamwebhelper.exe") != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            wchar_t image[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, image, &size)) {
                if (_wcsicmp(image, steam_exe.c_str()) == 0) {
                    running = true;
                } else if (_wcsicmp(entry.szExeFile, L"steamwebhelper.exe") == 0 && !folder.empty()) {
                    const std::wstring image_text = image;
                    if (image_text.size() > folder.size() && _wcsnicmp(image_text.c_str(), folder.c_str(), folder.size()) == 0) {
                        running = true;
                    }
                }
            }
            CloseHandle(process);
        } while (!running && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

bool ask_steam_to_quit(const std::wstring& steam_exe, std::wstring& error) {
    error.clear();
    const size_t slash = steam_exe.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? steam_exe : steam_exe.substr(slash + 1);
    if (_wcsicmp(name.c_str(), L"steam.exe") != 0) {
        error = L"Refusing to quit '" + steam_exe + L"' because the installer only stops a file named steam.exe, and it does not use launch_hidden.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    std::wstring command = L"\"" + steam_exe + L"\" -shutdown";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(steam_exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        error = L"Could not ask '" + steam_exe + L"' to quit before the update. " + windows_error_text(GetLastError());
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    log_event(L"info", L"installer", (L"Asked '" + steam_exe + L"' to quit so the update can restart it. launch_hidden was not used.").c_str());
    return true;
}

bool start_steam_visible(const std::wstring& steam_exe, std::wstring& error) {
    error.clear();
    if (steam_exe.empty() || steam_exe.find(L"..") != std::wstring::npos) {
        error = L"Refusing to start '" + steam_exe + L"' because the path is empty or contains '..'.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    const size_t slash = steam_exe.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? steam_exe : steam_exe.substr(slash + 1);
    if (_wcsicmp(name.c_str(), L"steam.exe") != 0) {
        error = L"Refusing to start '" + steam_exe + L"' because the installer only starts a file named steam.exe, and it does not use launch_hidden.";
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    if (steam_client_running(steam_exe)) {
        error = L"'" + steam_exe + L"' is still running, so Steam was not started again. launch_hidden was not used.";
        log_event(L"info", L"installer", error.c_str());
        return false;
    }
    std::wstring command = L"\"" + steam_exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(steam_exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        error = L"Could not start '" + steam_exe + L"' after the update. " + windows_error_text(GetLastError());
        log_event(L"error", L"installer", error.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    log_event(L"info", L"installer", (L"Started '" + steam_exe + L"' in a visible window after the update. launch_hidden was not used. Steam was restarted.").c_str());
    return true;
}

}  // namespace doomnet
