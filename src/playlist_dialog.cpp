#include "playlist_dialog.h"

#include "log.h"
#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doomnet {
namespace {

enum class PickState { Idle, Running, Path, Cancel, Failed };
enum class DialogKind { Open, Save };

std::mutex g_mu;
PickState g_state = PickState::Idle;
std::wstring g_path;
std::wstring g_error;
std::thread g_thread;

const wchar_t* kOpenFilter =
    L"*.m3u;*.m3u8;*.pls;*.xspf;*.jspf;*.wpl;*.zpl;*.asx;*.wax;*.wvx;*.b4s;*.mpcpl;*.aimppl;*.aimppl4;*.xml;*.plist";

std::wstring com_failure(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return windows_error_text(static_cast<unsigned long>(HRESULT_CODE(hr)));
    }
    wchar_t buffer[96] = {};
    swprintf_s(buffer, L"HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

bool ends_with_ci(const std::wstring& text, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    if (text.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        wchar_t left = text[text.size() - n + i];
        wchar_t right = suffix[i];
        if (left >= L'A' && left <= L'Z') {
            left = static_cast<wchar_t>(left - L'A' + L'a');
        }
        if (right >= L'A' && right <= L'Z') {
            right = static_cast<wchar_t>(right - L'A' + L'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

std::wstring ensure_m3u_extension(std::wstring path) {
    while (!path.empty() && (path.back() == L' ' || path.back() == L'.')) {
        path.pop_back();
    }
    if (!ends_with_ci(path, L".m3u") && !ends_with_ci(path, L".m3u8")) {
        path += L".m3u";
    }
    return path;
}

bool path_has_dotdot(const std::wstring& path) {
    std::wstring part;
    for (size_t i = 0; i <= path.size(); ++i) {
        const bool split = i == path.size() || path[i] == L'\\' || path[i] == L'/';
        if (!split) {
            part.push_back(path[i]);
            continue;
        }
        if (part == L"..") {
            return true;
        }
        part.clear();
    }
    return false;
}

std::string trim_copy(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string file_name_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string dirname_of(std::string path) {
    for (char& ch : path) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    while (!path.empty() && path.back() == '\\') {
        path.pop_back();
    }
    const size_t slash = path.find_last_of('\\');
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash);
}

bool is_absolute_path(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0])) != 0) {
        return true;
    }
    return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
}

std::string collapse_path(const std::string& path) {
    std::string prefix;
    std::string body = path;
    if (body.size() >= 2 && body[1] == ':' && std::isalpha(static_cast<unsigned char>(body[0])) != 0) {
        prefix.assign(body, 0, 2);
        body = body.size() > 2 ? body.substr(2) : std::string();
    } else if (body.size() >= 2 && body[0] == '\\' && body[1] == '\\') {
        prefix = "\\\\";
        body = body.substr(2);
    }
    std::vector<std::string> parts;
    std::string part;
    body.push_back('\\');
    for (const char ch : body) {
        if (ch != '\\') {
            part.push_back(ch);
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        part.clear();
    }
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            joined.push_back('\\');
        }
        joined += parts[i];
    }
    if (!prefix.empty() && prefix.back() != '\\') {
        if (joined.empty()) {
            return prefix + "\\";
        }
        return prefix + "\\" + joined;
    }
    return prefix + joined;
}

void finish(PickState state, const std::wstring& path, const std::wstring& error) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_state = state;
    g_path = path;
    g_error = error;
}

void run_dialog(DialogKind kind, std::wstring suggested) {
    const HRESULT start = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = start == S_OK || start == S_FALSE;
    IFileDialog* dialog = nullptr;
    const CLSID clsid = kind == DialogKind::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr) {
        finish(PickState::Failed, L"", L"The playlist file window could not be opened. " + com_failure(hr));
    } else {
        DWORD options = 0;
        dialog->GetOptions(&options);
        const DWORD extra = kind == DialogKind::Save ? (FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR)
                                                      : (FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
        hr = dialog->SetOptions(options | extra);
        if (FAILED(hr)) {
            finish(PickState::Failed, L"", L"The playlist file window could not be limited to files. " + com_failure(hr));
        } else {
            COMDLG_FILTERSPEC specs[2] = {};
            if (kind == DialogKind::Save) {
                specs[0].pszName = L"Playlist";
                specs[0].pszSpec = L"*.m3u;*.m3u8";
                dialog->SetFileTypes(1, specs);
                dialog->SetFileTypeIndex(1);
                dialog->SetDefaultExtension(L"m3u");
                dialog->SetFileName(suggested.c_str());
                dialog->SetTitle(L"Save the playlist");
            } else {
                specs[0].pszName = L"Playlist files";
                specs[0].pszSpec = playlist_open_filter();
                specs[1].pszName = L"All files";
                specs[1].pszSpec = L"*.*";
                dialog->SetFileTypes(2, specs);
                dialog->SetFileTypeIndex(1);
                dialog->SetTitle(L"Choose a playlist file");
            }
            hr = dialog->Show(GetForegroundWindow());
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                finish(PickState::Cancel, L"", L"");
            } else if (FAILED(hr)) {
                finish(PickState::Failed, L"", L"The playlist file window failed. " + com_failure(hr));
            } else {
                IShellItem* item = nullptr;
                hr = dialog->GetResult(&item);
                PWSTR chosen = nullptr;
                if (FAILED(hr) || item == nullptr) {
                    finish(PickState::Failed, L"", L"The playlist file window did not return a file. " + com_failure(hr));
                } else if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) || chosen == nullptr) {
                    finish(PickState::Failed, L"", L"The playlist file window returned a file with no path. " + com_failure(hr));
                } else {
                    std::wstring path = chosen;
                    CoTaskMemFree(chosen);
                    if (kind == DialogKind::Save) {
                        path = ensure_m3u_extension(path);
                    }
                    finish(PickState::Path, path, L"");
                }
                if (item != nullptr) {
                    item->Release();
                }
            }
        }
        dialog->Release();
    }
    if (uninit) {
        CoUninitialize();
    }
}

bool begin_dialog(DialogKind kind, const std::wstring& suggested) {
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == PickState::Running) {
            log_event(L"warn", L"playlists", L"The playlist file window is already open, so a second one was not started.");
            return false;
        }
        if (g_thread.joinable()) {
            previous.swap(g_thread);
        }
        g_path.clear();
        g_error.clear();
        g_state = PickState::Running;
        g_thread = std::thread(run_dialog, kind, suggested);
    }
    if (previous.joinable()) {
        previous.join();
    }
    log_event(L"info", L"playlists", kind == DialogKind::Save ? L"Opened the playlist save window." : L"Opened the playlist file window.");
    return true;
}

}  // namespace

std::string playlist_m3u(const std::vector<PlaylistSong>& songs) {
    std::string out = "\xEF\xBB\xBF#EXTM3U\r\n";
    for (const PlaylistSong& song : songs) {
        std::string label = song.label;
        for (char& ch : label) {
            if (ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        label = trim_copy(label);
        if (label.empty()) {
            label = file_name_of(song.path);
        }
        if (label.empty()) {
            label = "Track";
        }
        std::string path = song.path;
        for (char& ch : path) {
            if (ch == '/') {
                ch = '\\';
            }
        }
        out += "#EXTINF:-1,";
        out += label;
        out += "\r\n";
        out += path;
        out += "\r\n";
    }
    return out;
}

std::wstring playlist_file_name(const std::wstring& playlist_name) {
    std::wstring name;
    bool spaced = false;
    for (const wchar_t ch : playlist_name) {
        const bool bad = ch < 32 || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|';
        if (bad || ch == L' ' || ch == L'\t') {
            if (!name.empty() && !spaced) {
                name.push_back(L' ');
                spaced = true;
            }
            continue;
        }
        spaced = false;
        name.push_back(ch);
        if (name.size() >= 80) {
            break;
        }
    }
    while (!name.empty() && name.back() == L' ') {
        name.pop_back();
    }
    if (name.empty()) {
        name = L"playlist";
    }
    if (!ends_with_ci(name, L".m3u") && !ends_with_ci(name, L".m3u8")) {
        name += L".m3u";
    }
    return name;
}

std::string resolve_playlist_entry(const std::string& playlist_path, const std::string& entry) {
    std::string trimmed = trim_copy(entry);
    if (trimmed.empty()) {
        return {};
    }
    for (char& ch : trimmed) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    std::string combined;
    if (is_absolute_path(trimmed)) {
        combined = trimmed;
    } else {
        std::string dir = dirname_of(playlist_path);
        if (dir.empty()) {
            combined = trimmed;
        } else {
            if (dir.back() != '\\') {
                dir.push_back('\\');
            }
            combined = dir + trimmed;
        }
    }
    return collapse_path(combined);
}

const wchar_t* playlist_open_filter() {
    return kOpenFilter;
}

bool write_playlist_file(const std::wstring& path, const std::string& body, std::wstring& error) {
    error.clear();
    if (path.empty() || path_has_dotdot(path)) {
        error = L"The playlist file '" + path + L"' was not saved. The path is empty or contains '..'.";
        log_event(L"error", L"playlists", error.c_str());
        return false;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = L"The playlist file '" + path + L"' was not saved. " + windows_error_text(GetLastError());
        log_event(L"error", L"playlists", error.c_str());
        return false;
    }
    size_t done = 0;
    bool ok = true;
    unsigned long write_error = 0;
    while (done < body.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(body.size() - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(handle, body.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            write_error = GetLastError();
            ok = false;
            break;
        }
        done += wrote;
    }
    CloseHandle(handle);
    if (!ok) {
        error = L"The playlist file '" + path + L"' was not saved. " + windows_error_text(write_error);
        log_event(L"error", L"playlists", error.c_str());
        return false;
    }
    log_event(L"info", L"playlists", (L"Saved playlist file '" + path + L"'.").c_str());
    return true;
}

const char* playlist_io_script() {
    return
        "function export_playlist(id)\n"
        "  local playlists = require(\"playlists\")\n"
        "  local library = require(\"library\")\n"
        "  local found = nil\n"
        "  for _, playlist in ipairs(playlists.get_all()) do\n"
        "    if tostring(playlist.id) == tostring(id) then\n"
        "      found = playlist\n"
        "    end\n"
        "  end\n"
        "  if not found then\n"
        "    return \"That playlist is not in the player.\"\n"
        "  end\n"
        "  local songs = {}\n"
        "  for _, trackId in ipairs(found.trackIds or {}) do\n"
        "    local track = library.get_track(trackId)\n"
        "    if track and type(track.path) == \"string\" and track.path ~= \"\" then\n"
        "      local artist = tostring(track.artist or \"\")\n"
        "      local title = tostring(track.title or \"\")\n"
        "      local label = title\n"
        "      if artist ~= \"\" and title ~= \"\" then\n"
        "        label = artist .. \" - \" .. title\n"
        "      elseif artist ~= \"\" then\n"
        "        label = artist\n"
        "      end\n"
        "      songs[#songs + 1] = { label = label, path = track.path }\n"
        "    end\n"
        "  end\n"
        "  if not doomnet_begin_playlist_save(tostring(found.name or \"playlist\")) then\n"
        "    return \"A file window is already open.\"\n"
        "  end\n"
        "  local utils = require(\"utils\")\n"
        "  local status, value\n"
        "  while true do\n"
        "    status, value = doomnet_playlist_dialog_result()\n"
        "    if status ~= \"wait\" then\n"
        "      break\n"
        "    end\n"
        "    utils.sleep(200)\n"
        "  end\n"
        "  if status == \"failed\" then\n"
        "    return value or \"The playlist file could not be saved.\"\n"
        "  end\n"
        "  if status ~= \"path\" or not value or value == \"\" then\n"
        "    return \"\"\n"
        "  end\n"
        "  local body = doomnet_playlist_m3u(songs)\n"
        "  if not doomnet_write_playlist_file(value, body) then\n"
        "    return \"The playlist file could not be saved.\"\n"
        "  end\n"
        "  if #songs == 0 then\n"
        "    return \"Saved the playlist. It has no songs in it yet.\"\n"
        "  end\n"
        "  return \"Saved the playlist.\"\n"
        "end\n"
        "\n"
        "function import_playlist()\n"
        "  if not doomnet_begin_playlist_open() then\n"
        "    return \"A file window is already open.\"\n"
        "  end\n"
        "  local utils = require(\"utils\")\n"
        "  local status, value\n"
        "  while true do\n"
        "    status, value = doomnet_playlist_dialog_result()\n"
        "    if status ~= \"wait\" then\n"
        "      break\n"
        "    end\n"
        "    utils.sleep(200)\n"
        "  end\n"
        "  if status == \"failed\" then\n"
        "    return value or \"That playlist file could not be opened.\"\n"
        "  end\n"
        "  if status ~= \"path\" or not value or value == \"\" then\n"
        "    return \"\"\n"
        "  end\n"
        "  local playlist_file = require(\"playlist_file\")\n"
        "  local library = require(\"library\")\n"
        "  local playlists = require(\"playlists\")\n"
        "  local lists = nil\n"
        "  if playlist_file.read_all then\n"
        "    lists = playlist_file.read_all(value)\n"
        "  elseif playlist_file.read then\n"
        "    local one = playlist_file.read(value)\n"
        "    if one then\n"
        "      lists = { one }\n"
        "    end\n"
        "  end\n"
        "  if not lists or not lists[1] then\n"
        "    return \"That file is not a playlist this player can read.\"\n"
        "  end\n"
        "  local made = 0\n"
        "  local songs = 0\n"
        "  local named = \"\"\n"
        "  for _, parsed in ipairs(lists) do\n"
        "    local ids = {}\n"
        "    local seen = {}\n"
        "    for _, entry in ipairs((parsed and parsed.paths) or {}) do\n"
        "      local resolved = doomnet_resolve_playlist_entry(value, tostring(entry or \"\"))\n"
        "      local id = nil\n"
        "      if resolved and library.id_for_path then\n"
        "        id = library.id_for_path(resolved)\n"
        "      end\n"
        "      if id ~= nil and not seen[tostring(id)] then\n"
        "        seen[tostring(id)] = true\n"
        "        ids[#ids + 1] = id\n"
        "      end\n"
        "    end\n"
        "    if #ids > 0 then\n"
        "      local created = playlists.create(parsed.name or \"Imported playlist\")\n"
        "      if created and created.id ~= nil then\n"
        "        for _, trackId in ipairs(ids) do\n"
        "          playlists.add_track(created.id, trackId)\n"
        "        end\n"
        "        made = made + 1\n"
        "        songs = songs + #ids\n"
        "        named = tostring(created.name or parsed.name or \"Imported playlist\")\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "  if made == 0 then\n"
        "    return \"None of the songs in that file are in the library.\"\n"
        "  end\n"
        "  if made == 1 then\n"
        "    return \"Imported \" .. tostring(songs) .. \" songs into \" .. named .. \".\"\n"
        "  end\n"
        "  return \"Imported \" .. tostring(made) .. \" playlists.\"\n"
        "end\n";
}

bool begin_playlist_save(const std::wstring& playlist_name) {
    return begin_dialog(DialogKind::Save, playlist_file_name(playlist_name));
}

bool begin_playlist_open() {
    return begin_dialog(DialogKind::Open, L"");
}

std::string playlist_dialog_take(std::wstring& path, std::wstring& error) {
    path.clear();
    error.clear();
    std::thread done;
    PickState state = PickState::Idle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == PickState::Running) {
            return "wait";
        }
        if (g_state == PickState::Idle) {
            return "idle";
        }
        state = g_state;
        path = g_path;
        error = g_error;
        g_state = PickState::Idle;
        g_path.clear();
        g_error.clear();
        done.swap(g_thread);
    }
    if (done.joinable()) {
        done.join();
    }
    if (state == PickState::Path) {
        log_event(L"info", L"playlists", (L"The playlist file window returned '" + path + L"'.").c_str());
        return "path";
    }
    if (state == PickState::Cancel) {
        log_event(L"info", L"playlists", L"The playlist file window was closed without a file.");
        return "cancel";
    }
    log_event(L"error", L"playlists", error.c_str());
    return "failed";
}

}  // namespace doomnet
