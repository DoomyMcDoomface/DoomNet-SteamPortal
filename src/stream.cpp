// Spec: Playback path, Contracts to match.
// URL the page fetches, built by stage_track_for_playback as an opaque
// string: https://steamloopback.host/steam-music-player/stream/<token>/<id>
// <id> is tostring(track.id), not staged_name_for.
// Token is the hex in player data media_token (at least 16 hex digits).
// Lookup id in library_index.json (id, path, size). library_snapshot.json
// has no filesystem path.
// Missing id, "..", a drive letter, or a path segment is 404.
// Never join the URL onto a directory.
// No Range header: 200, entire file, Content-Length equal to size.
// release_staged_track on this path deletes nothing and returns ok true.
// get_host_escape_probe's "%2e%2e" URL is 404.
// Wide CreateFileW for the open. Do not copy the file into steamui when
// this handler is up.

#include "anchors.h"
#include "json_wire.h"
#include "log.h"
#include "paths.h"
#include "stream.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace doomnet {

void stream_anchor() {}

namespace {

std::atomic<bool> g_stream_hook{false};

constexpr uint64_t kMaxTrackBytes = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMaxIndexBytes = 64ull * 1024ull * 1024ull;

StreamReply refused(const std::wstring& reason) {
    StreamReply reply;
    reply.status = 404;
    reply.content_type = "text/plain";
    reply.body = "not found";
    reply.detail = reason;
    log_event(L"warn", L"stream", reason.c_str());
    return reply;
}

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool percent_decode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) {
            return false;
        }
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

bool segment_is_forbidden(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == "..") {
        return true;
    }
    if (segment.find('\\') != std::string::npos || segment.find('/') != std::string::npos || segment.find(':') != std::string::npos || segment.find("..") != std::string::npos) {
        return true;
    }
    return segment.size() == 2 && ((segment[0] >= 'A' && segment[0] <= 'Z') || (segment[0] >= 'a' && segment[0] <= 'z')) && segment[1] == ':';
}

bool read_bounded(const std::wstring& path, uint64_t limit, std::string& out, std::wstring& error) {
    out.clear();
    error.clear();
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        error = windows_error_text(GetLastError());
        CloseHandle(handle);
        return false;
    }
    if (static_cast<uint64_t>(size.QuadPart) > limit) {
        error = L"the file is larger than DoomNet will read for one play";
        CloseHandle(handle);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t filled = 0;
    while (filled < out.size()) {
        const size_t remaining = out.size() - filled;
        const DWORD chunk = remaining > 1024u * 1024u ? 1024u * 1024u : static_cast<DWORD>(remaining);
        DWORD read = 0;
        if (!ReadFile(handle, out.data() + filled, chunk, &read, nullptr)) {
            error = windows_error_text(GetLastError());
            CloseHandle(handle);
            out.clear();
            return false;
        }
        if (read == 0) {
            break;
        }
        filled += read;
    }
    CloseHandle(handle);
    out.resize(filled);
    return true;
}

std::string token_hex(const std::string& raw) {
    std::string hex;
    hex.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (ch >= '0' && ch <= '9') {
            hex.push_back(static_cast<char>(ch));
        } else if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
            hex.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
        }
    }
    return hex;
}

std::string track_id_text(const Json& track) {
    const Json* id = track.find("id");
    if (id == nullptr) {
        return {};
    }
    if (id->type == Json::Type::String) {
        return id->s;
    }
    if (id->type == Json::Type::Number && std::isfinite(id->n)) {
        char printed[64] = {};
        std::snprintf(printed, sizeof(printed), "%.14g", id->n);
        return printed;
    }
    return {};
}

std::wstring find_track_path(const std::string& index, const std::string& id, std::wstring& error) {
    Json root;
    std::string parse_error;
    if (!json_parse(index, root, parse_error) || root.type != Json::Type::Array) {
        error = L"library_index.json is not a list of tracks";
        return {};
    }
    for (const Json& track : root.a) {
        if (track_id_text(track) != id) {
            continue;
        }
        const std::string path = track.key_string("path");
        std::wstring wide;
        if (!utf8_to_wide_strict(path.data(), path.size(), wide) && !lua_path_to_wide(path.data(), path.size(), wide)) {
            error = L"the track path in library_index.json is not a usable path";
            return {};
        }
        return wide;
    }
    error = L"the track id is not in library_index.json";
    return {};
}

std::wstring lower_copy_wide(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return out;
}

std::string content_type_for(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : lower_copy_wide(path.substr(dot));
    if (ext == L".mp3") {
        return "audio/mpeg";
    }
    if (ext == L".wav") {
        return "audio/wav";
    }
    if (ext == L".flac") {
        return "audio/flac";
    }
    if (ext == L".ogg" || ext == L".oga") {
        return "audio/ogg";
    }
    if (ext == L".m4a") {
        return "audio/mp4";
    }
    if (ext == L".aac") {
        return "audio/aac";
    }
    return "application/octet-stream";
}

bool parse_range(const std::string& header, uint64_t total, uint64_t& start, uint64_t& end, std::wstring& error) {
    const std::string text = lower_copy(header);
    if (text.rfind("bytes=", 0) != 0) {
        error = L"the Range header is not a byte range";
        return false;
    }
    const std::string spec = text.substr(6);
    const size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos) {
        error = L"the Range header could not be read";
        return false;
    }
    try {
        start = dash == 0 ? 0 : std::stoull(spec.substr(0, dash));
        if (dash + 1 >= spec.size()) {
            end = total == 0 ? 0 : total - 1;
        } else {
            end = std::stoull(spec.substr(dash + 1));
        }
    } catch (...) {
        error = L"the Range header could not be read";
        return false;
    }
    if (dash == 0) {
        if (total == 0 || end == 0 || end > total) {
            error = L"the Range header is past the end of the file";
            return false;
        }
        start = total - end;
        end = total - 1;
    }
    if (total == 0 || start > end || start >= total) {
        error = L"the Range header is past the end of the file";
        return false;
    }
    if (end >= total) {
        end = total - 1;
    }
    return true;
}

}  // namespace

bool stream_hook_up() {
    return g_stream_hook.load();
}

void set_stream_hook(bool up) {
    g_stream_hook.store(up);
}

StreamReply serve_stream_url(const std::string& url, const std::string& range_header) {
    const std::string lowered = lower_copy(url);
    if (lowered.find("%2e%2e") != std::string::npos) {
        return refused(L"Refused a stream request because the address contains '..'.");
    }
    const size_t cut = url.find_first_of("?#");
    const std::string clean = cut == std::string::npos ? url : url.substr(0, cut);
    const std::string clean_lower = lower_copy(clean);
    const std::string marker = "/steam-music-player/stream/";
    const size_t at = clean_lower.find(marker);
    if (at == std::string::npos) {
        return refused(L"Refused a stream request because it is not a track address.");
    }
    const std::string rest = clean.substr(at + marker.size());
    std::vector<std::string> parts;
    std::string current;
    for (char ch : rest) {
        if (ch == '/') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() || rest.empty() || rest.back() == '/') {
        parts.push_back(current);
    }
    if (parts.size() != 2) {
        return refused(L"Refused a stream request because the address is not a token and a track id.");
    }
    std::string token_part;
    std::string id_part;
    if (!percent_decode(parts[0], token_part) || !percent_decode(parts[1], id_part) || segment_is_forbidden(token_part) || segment_is_forbidden(id_part)) {
        return refused(L"Refused a stream request because a piece of the address is a path.");
    }
    std::wstring root;
    std::wstring root_error;
    if (!data_root(root, root_error)) {
        return refused(root_error.empty() ? L"Refused a stream request because the DoomNet data folder is not set." : root_error);
    }
    std::string token_file;
    std::wstring token_error;
    if (!read_bounded(root + L"\\player\\backend\\data\\media_token", 4096, token_file, token_error)) {
        return refused(L"Refused a stream request because the playback token could not be read. " + token_error);
    }
    const std::string expected = token_hex(token_file);
    const std::string presented = token_hex(token_part);
    if (expected.size() < 16 || presented != expected) {
        return refused(L"Refused a stream request because the playback token does not match.");
    }
    std::string index;
    std::wstring index_error;
    if (!read_bounded(root + L"\\player\\backend\\data\\library_index.json", kMaxIndexBytes, index, index_error)) {
        return refused(L"Refused a stream request because library_index.json could not be read. " + index_error);
    }
    std::wstring path_error;
    const std::wstring path = find_track_path(index, id_part, path_error);
    if (path.empty()) {
        return refused(L"Refused track '" + utf8_to_wide_lossy(id_part) + L"' because " + path_error + L".");
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return refused(L"Refused track '" + utf8_to_wide_lossy(id_part) + L"' because '" + path + L"' is not on disk. " + windows_error_text(GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return refused(L"Refused track '" + utf8_to_wide_lossy(id_part) + L"' because '" + path + L"' is a folder.");
    }
    std::string file;
    std::wstring read_error;
    if (!read_bounded(path, kMaxTrackBytes, file, read_error)) {
        return refused(L"Refused track '" + utf8_to_wide_lossy(id_part) + L"' because '" + path + L"' could not be read. " + read_error);
    }
    StreamReply reply;
    reply.content_type = content_type_for(path);
    reply.detail = path;
    if (!range_header.empty()) {
        uint64_t start = 0;
        uint64_t end = 0;
        std::wstring range_error;
        if (!parse_range(range_header, file.size(), start, end, range_error)) {
            return refused(L"Refused track '" + utf8_to_wide_lossy(id_part) + L"' because " + range_error + L".");
        }
        const uint64_t count = end - start + 1;
        reply.status = 206;
        reply.body.assign(file.data() + static_cast<size_t>(start), static_cast<size_t>(count));
        reply.content_range = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file.size());
    } else {
        reply.status = 200;
        reply.body = std::move(file);
    }
    log_event(L"info", L"stream", (L"Played '" + path + L"' for track '" + utf8_to_wide_lossy(id_part) + L"' without copying it into steamui.").c_str());
    return reply;
}

}  // namespace doomnet
