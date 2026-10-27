#include "skin_page.h"

#include "json_wire.h"
#include "log.h"
#include "text.h"
#include "wide_io.h"

#include <cstring>
#include <regex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace doomnet {
namespace {

bool has_dotdot(const std::wstring& path) {
    return path.find(L"..") != std::wstring::npos;
}

bool ends_with_ci(const std::wstring& name, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    if (name.size() < n) {
        return false;
    }
    return _wcsicmp(name.c_str() + (name.size() - n), suffix) == 0;
}

bool plain_file_name(const std::string& name) {
    if (name.empty() || name.size() > 180) {
        return false;
    }
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return name.find("..") == std::string::npos;
}

bool is_player_inject(const std::string& name) {
    return name == "music-player-inject.js" || (name.size() > 22 && name.find("music-player-inject.js") != std::string::npos);
}

struct SkinTag {
    std::string match;
    std::string css_url;
    std::string js_url;
    std::string css_body;
};

std::string guard_markup(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<' && i + 1 < text.size() && text[i + 1] == '/') {
            out += "<\\/";
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string read_skin_text(const std::wstring& folder, const std::string& name) {
    if (folder.empty() || name.empty()) {
        return {};
    }
    std::string bytes;
    std::wstring error;
    if (!read_entire_file(folder + L"\\" + utf8_to_wide_lossy(name), bytes, error)) {
        return {};
    }
    return bytes;
}

std::vector<SkinTag> g_tags;
bool g_player = false;

size_t find_ci(const std::string& text, const char* needle) {
    const size_t n = std::strlen(needle);
    if (n == 0 || text.size() < n) {
        return std::string::npos;
    }
    for (size_t i = 0; i + n <= text.size(); ++i) {
        size_t j = 0;
        for (; j < n; ++j) {
            char a = text[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == n) {
            return i;
        }
    }
    return std::string::npos;
}

}  // namespace

SkinFileCopy copy_skin_files(const std::wstring& skin_dir, const std::wstring& steam_dir) {
    SkinFileCopy result;
    if (skin_dir.empty() || steam_dir.empty() || has_dotdot(skin_dir) || has_dotdot(steam_dir)) {
        result.detail = L"Refusing to copy skin files because a path is empty or contains '..'.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    const DWORD skin_attrs = GetFileAttributesW(skin_dir.c_str());
    if (skin_attrs == INVALID_FILE_ATTRIBUTES || (skin_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        result.detail = L"'" + skin_dir + L"' is not a folder, so no skin files were copied.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (!wide_file_exists(steam_dir + L"\\steam.exe")) {
        result.detail = L"'" + steam_dir + L"' does not contain steam.exe, so no skin files were copied.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }

    const std::wstring steamui = steam_dir + L"\\steamui";
    result.destination = steamui + L"\\doomnet-skin";
    if (!CreateDirectoryW(steamui.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        result.detail = L"Could not create '" + steamui + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    if (!CreateDirectoryW(result.destination.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        result.detail = L"Could not create '" + result.destination + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }

    WIN32_FIND_DATAW found{};
    const std::wstring pattern = skin_dir + L"\\*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        result.detail = L"Could not list '" + skin_dir + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring name = found.cFileName;
        if (!ends_with_ci(name, L".css") && !ends_with_ci(name, L".js")) {
            continue;
        }
        const std::wstring from = skin_dir + L"\\" + name;
        const std::wstring to = result.destination + L"\\" + name;
        if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            FindClose(search);
            result.detail = L"Could not copy '" + from + L"' to '" + to + L"'. " + windows_error_text(GetLastError());
            log_event(L"error", L"payload", result.detail.c_str());
            return result;
        }
        ++result.copied;
    } while (FindNextFileW(search, &found));
    FindClose(search);

    if (result.copied == 0) {
        result.detail = L"'" + skin_dir + L"' has no .css or .js files, so '" + result.destination + L"' is empty.";
        log_event(L"error", L"payload", result.detail.c_str());
        return result;
    }
    result.ok = true;
    result.detail = L"Copied " + std::to_wstring(result.copied) + L" skin file(s) into '" + result.destination + L"'.";
    log_event(L"info", L"payload", result.detail.c_str());
    return result;
}

SkinPreamble load_skin_preamble(const std::wstring& skin_json, bool player) {
    SkinPreamble result;
    if (skin_json.empty() || has_dotdot(skin_json)) {
        result.detail = L"Refusing to read skin.json because the path is empty or contains '..'.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    std::string bytes;
    std::wstring read_error;
    if (!read_entire_file(skin_json, bytes, read_error)) {
        result.detail = read_error.empty() ? L"Could not read '" + skin_json + L"'." : read_error;
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    Json root;
    std::string parse_error;
    if (!json_parse(bytes, root, parse_error) || root.type != Json::Type::Object) {
        result.detail = L"'" + skin_json + L"' is not a skin.json object (" + utf8_to_wide_lossy(parse_error) + L").";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    const Json* patches = root.find("Patches");
    if (patches == nullptr || patches->type != Json::Type::Array) {
        result.detail = L"'" + skin_json + L"' has no Patches array, so no skin tags were prepared.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }

    std::wstring skin_folder = skin_json;
    const size_t folder_slash = skin_folder.find_last_of(L"\\/");
    if (folder_slash != std::wstring::npos) {
        skin_folder.resize(folder_slash);
    }
    std::vector<SkinTag> tags;
    const std::string base = "https://steamloopback.host/doomnet-skin/";
    std::string javascript = std::string("window.__doomnetConfig={\"player\":") + (player ? "true" : "false") + ",\"skin\":true,\"patches\":[";
    bool first = true;
    int skipped_player = 0;
    for (const Json& patch : patches->a) {
        if (patch.type != Json::Type::Object) {
            continue;
        }
        const std::string match = patch.key_string("MatchRegexString");
        const std::string css = patch.key_string("TargetCss");
        const std::string js = patch.key_string("TargetJs");
        if (match.empty()) {
            continue;
        }
        if (is_player_inject(js)) {
            ++skipped_player;
            continue;
        }
        if (!css.empty() && !plain_file_name(css)) {
            result.detail = L"'" + skin_json + L"' has a TargetCss name that is not a plain file name, so no skin tags were prepared.";
            log_event(L"error", L"inject", result.detail.c_str());
            return result;
        }
        if (!js.empty() && !plain_file_name(js)) {
            result.detail = L"'" + skin_json + L"' has a TargetJs name that is not a plain file name, so no skin tags were prepared.";
            log_event(L"error", L"inject", result.detail.c_str());
            return result;
        }
        if (css.empty() && js.empty()) {
            continue;
        }
        SkinTag tag;
        tag.match = match;
        if (!css.empty()) {
            tag.css_url = base + css;
            tag.css_body = read_skin_text(skin_folder, css);
        }
        if (!js.empty()) {
            tag.js_url = base + js;
        }
        tags.push_back(std::move(tag));
        if (!first) {
            javascript += ",";
        }
        first = false;
        javascript += "{\"match\":" + json_quote(match);
        if (!css.empty()) {
            javascript += ",\"css\":" + json_quote(base + css);
        }
        if (!js.empty()) {
            javascript += ",\"js\":" + json_quote(base + js);
        }
        javascript += "}";
        ++result.patches;
    }
    javascript += "]};";
    if (result.patches == 0) {
        result.detail = L"'" + skin_json + L"' produced no skin patches after music-player-inject.js was skipped.";
        log_event(L"error", L"inject", result.detail.c_str());
        return result;
    }
    result.ok = true;
    result.javascript = std::move(javascript);
    g_player = player;
    g_tags = std::move(tags);
    result.detail = L"Prepared " + std::to_wstring(result.patches) + L" skin patch(es) from '" + skin_json + L"' and skipped " + std::to_wstring(skipped_player) + L" music-player-inject.js patch(es). UseDefaultPatches was not applied.";
    log_event(L"info", L"inject", result.detail.c_str());
    return result;
}

bool skin_tags_ready() {
    return !g_tags.empty();
}

bool skin_player_requested() {
    return g_player;
}

std::string skin_tags_for_document(const std::string& url) {
    if (url.find("checkout.steampowered.com") != std::string::npos) {
        return {};
    }
    std::string tags;
    for (const SkinTag& tag : g_tags) {
        bool matched = false;
        try {
            matched = std::regex_search(url, std::regex(tag.match, std::regex::ECMAScript));
        } catch (const std::regex_error&) {
            matched = false;
        }
        if (!matched) {
            continue;
        }
        // CSS is inlined so the first paint is already skinned. Scripts stay
        // as tags. Inlining them made every store response huge and the
        // library tab waited on that rewrite.
        if (!tag.css_body.empty()) {
            tags += "<style id=\"doomnet-skin-css\">" + guard_markup(tag.css_body) + "</style>";
        } else if (!tag.css_url.empty()) {
            tags += "<link rel=\"stylesheet\" href=\"" + tag.css_url + "\">";
        }
        if (!tag.js_url.empty()) {
            tags += "<script src=\"" + tag.js_url + "\"></script>";
        }
    }
    return tags;
}

std::string patched_player_ui_text() {
    const std::wstring dir = L"D:\\Documents\\Repos\\SteamTunes\\backend\\assets\\frontend";
    std::string ui = read_skin_text(dir, "steam-music-player.js");
    if (ui.empty()) {
        return {};
    }
    std::string normalized;
    normalized.reserve(ui.size());
    for (char ch : ui) {
        if (ch != '\r') {
            normalized.push_back(ch);
        }
    }
    ui.swap(normalized);
    const std::string guard = "window !== window.top";
    const size_t guard_at = ui.find(guard);
    if (guard_at != std::string::npos) {
        ui.replace(guard_at, guard.size(), "false");
    }
    const std::string reject_data = "if (!album || !url || url.indexOf(\"data:\") === 0) {\n\t\t\t\treturn;\n\t\t\t}";
    const std::string allow_data = "if (!album || !url) {\n\t\t\t\treturn;\n\t\t\t}";
    const size_t reject_at = ui.find(reject_data);
    if (reject_at != std::string::npos) {
        ui.replace(reject_at, reject_data.size(), allow_data);
    }
    const std::string match_check = "if (!artUrlMatchesAlbum(album, url)) {\n\t\t\t\treturn;\n\t\t\t}";
    const std::string match_allow = "if (url.indexOf(\"data:\") !== 0 && !artUrlMatchesAlbum(album, url)) {\n\t\t\t\treturn;\n\t\t\t}";
    const size_t match_at = ui.find(match_check);
    if (match_at != std::string::npos) {
        ui.replace(match_at, match_check.size(), match_allow);
    }
    const std::string get_check = "if (url && !artUrlMatchesAlbum(album, url)) {";
    const std::string get_allow = "if (url && url.indexOf(\"data:\") !== 0 && !artUrlMatchesAlbum(album, url)) {";
    const size_t get_at = ui.find(get_check);
    if (get_at != std::string::npos) {
        ui.replace(get_at, get_check.size(), get_allow);
    }
    const std::string old_probe = "probeArtUrl: function (url) {\n\t\t\treturn new Promise(function (resolve) {\n\t\t\t\tvar img = new Image();\n\t\t\t\tvar settled = false;\n\t\t\t\tvar finish = function (ok) {\n\t\t\t\t\tif (settled) {\n\t\t\t\t\t\treturn;\n\t\t\t\t\t}\n\t\t\t\t\tsettled = true;\n\t\t\t\t\tresolve(ok ? url : null);\n\t\t\t\t};\n\t\t\t\timg.onload = function () {\n\t\t\t\t\tfinish(true);\n\t\t\t\t};\n\t\t\t\timg.onerror = function () {\n\t\t\t\t\tfinish(false);\n\t\t\t\t};\n\t\t\t\tsetTimeout(function () {\n\t\t\t\t\tfinish(false);\n\t\t\t\t}, 2500);\n\t\t\t\timg.src = url;\n\t\t\t});\n\t\t},";
    const std::string new_probe =
        "probeArtUrl: function (url) {\n"
        "\t\t\treturn new Promise(function (resolve) {\n"
        "\t\t\t\tvar img = new Image();\n"
        "\t\t\t\tvar settled = false;\n"
        "\t\t\t\tvar fromFile = function () {\n"
        "\t\t\t\t\tif (!window.Millennium) { resolve(null); return; }\n"
        "\t\t\t\t\tvar album = String(url).match(/\\/art\\/albums\\/([^./?]+)\\.(jpg|png)/i);\n"
        "\t\t\t\t\tvar track = album ? null : String(url).match(/\\/art\\/([^/?]+)\\.(jpg|png)/i);\n"
        "\t\t\t\t\tvar found = album || track;\n"
        "\t\t\t\t\tif (!found) { resolve(null); return; }\n"
        "\t\t\t\t\tvar key = (album ? \"album:\" : \"track:\") + found[1];\n"
        "\t\t\t\t\twindow.Millennium.callServerMethod(\"SteamMusicPlayer\", \"get_album_art_b64\", [key]).then(function (b64) {\n"
        "\t\t\t\t\t\tif (!b64) { resolve(null); return; }\n"
        "\t\t\t\t\t\tresolve(\"data:image/\" + (String(found[2]).toLowerCase() === \"png\" ? \"png\" : \"jpeg\") + \";base64,\" + b64);\n"
        "\t\t\t\t\t}, function () { resolve(null); });\n"
        "\t\t\t\t};\n"
        "\t\t\t\tvar finish = function (ok) {\n"
        "\t\t\t\t\tif (settled) { return; }\n"
        "\t\t\t\t\tsettled = true;\n"
        "\t\t\t\t\tif (ok) { resolve(url); return; }\n"
        "\t\t\t\t\tfromFile();\n"
        "\t\t\t\t};\n"
        "\t\t\t\timg.onload = function () { finish(true); };\n"
        "\t\t\t\timg.onerror = function () { finish(false); };\n"
        "\t\t\t\tsetTimeout(function () { finish(false); }, 2500);\n"
        "\t\t\t\timg.src = url;\n"
        "\t\t\t});\n"
        "\t\t},";
    const size_t probe_at = ui.find(old_probe);
    if (probe_at != std::string::npos) {
        ui.replace(probe_at, old_probe.size(), new_probe);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the expected cover probe, so store covers still use the page image address.");
    }
    const std::string old_shared = "if (!album || !url || !artUrlMatchesAlbum(album, url)) {";
    const std::string new_shared = "if (!album || !url || (url.indexOf(\"data:\") !== 0 && !artUrlMatchesAlbum(album, url))) {";
    const size_t shared_at = ui.find(old_shared);
    if (shared_at != std::string::npos) {
        ui.replace(shared_at, old_shared.size(), new_shared);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the shared cover paint check, so a store tile can keep a blank cover.");
    }
    const std::string old_cached = "if (cached && artUrlMatchesAlbum(entry.album, cached)) {";
    const std::string new_cached = "if (cached && (cached.indexOf(\"data:\") === 0 || artUrlMatchesAlbum(entry.album, cached))) {";
    const size_t cached_at = ui.find(old_cached);
    if (cached_at != std::string::npos) {
        ui.replace(cached_at, old_cached.size(), new_cached);
    }
    const std::string old_pointer_send = "UI.pointerWriteTimer = null;\n\t\t\t\tcallServer(\"set_ui_pointer\", [";
    const std::string new_pointer_send =
        "UI.pointerWriteTimer = null;\n"
        "\t\t\t\tUI.pointerWrites = (UI.pointerWrites || 0) + 1;\n"
        "\t\t\t\tvar write = UI.pointerWrites;\n"
        "\t\t\t\tif (!UI.pointerBusy) {\n"
        "\t\t\t\t\tUI.pointerBusy = true;\n"
        "\t\t\t\t\tUI.pointerBusyRev = Number(App.playerState && App.playerState.pointerRev) || UI.minPointerRev || 0;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tcallServer(\"set_ui_pointer\", [";
    const size_t pointer_send_at = ui.find(old_pointer_send);
    if (pointer_send_at != std::string::npos) {
        ui.replace(pointer_send_at, old_pointer_send.size(), new_pointer_send);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the pointer send, so a late page reply can still overwrite the page you just opened.");
    }
    const std::string old_pointer_catch = "]).catch(function () {\n\t\t\t\t\t/* next local change retries */\n\t\t\t\t});";
    const std::string new_pointer_catch =
        "]).then(function (raw) {\n"
        "\t\t\t\t\tvar res = raw;\n"
        "\t\t\t\t\tif (typeof raw === \"string\") {\n"
        "\t\t\t\t\t\ttry { res = JSON.parse(raw); } catch (e) { res = null; }\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar rev = res && Number(res.rev);\n"
        "\t\t\t\t\tif (rev) {\n"
        "\t\t\t\t\t\tUI.minPointerRev = Math.max(UI.minPointerRev || 0, rev);\n"
        "\t\t\t\t\t\tUI.pointerRevFloor = Math.max(UI.pointerRevFloor || 0, rev);\n"
        "\t\t\t\t\t\tUI.pointerSawChrome = true;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tif (write === UI.pointerWrites) { UI.pointerAck = write; UI.pointerBusy = false; }\n"
        "\t\t\t\t}).catch(function () {\n"
        "\t\t\t\t\tif (write === UI.pointerWrites) { UI.pointerAck = write; UI.pointerBusy = false; }\n"
        "\t\t\t\t});";
    const size_t pointer_catch_at = ui.find(old_pointer_catch);
    if (pointer_catch_at != std::string::npos) {
        ui.replace(pointer_catch_at, old_pointer_catch.size(), new_pointer_catch);
    }
    const std::string old_pointer_apply = "if (!state || !UI.root || UI.searchHold) {\n\t\t\t\treturn;\n\t\t\t}\n\t\t\tvar pointer = pointerFromState(state);";
    const std::string new_pointer_apply =
        "if (!state || !UI.root || UI.searchHold) {\n\t\t\treturn;\n\t\t\t}\n"
        "\t\t\tif ((UI.pointerWrites || 0) > (UI.pointerAck || 0)) {\n\t\t\t\treturn;\n\t\t\t}\n"
        "\t\t\tvar incomingRev = Number(state.pointerRev) || 0;\n"
        "\t\t\tif (UI.pointerBusy && incomingRev <= (UI.pointerBusyRev || 0)) {\n\t\t\t\treturn;\n\t\t\t}\n"
        "\t\t\tif (UI.pointerSawChrome && (!incomingRev || incomingRev <= (UI.pointerRevFloor || 0))) {\n\t\t\t\treturn;\n\t\t\t}\n"
        "\t\t\tUI.pointerSawChrome = true;\n"
        "\t\t\tif (incomingRev) { UI.pointerRevFloor = incomingRev; }\n"
        "\t\t\tvar pointer = pointerFromState(state);";
    const size_t pointer_apply_at = ui.find(old_pointer_apply);
    if (pointer_apply_at != std::string::npos) {
        ui.replace(pointer_apply_at, old_pointer_apply.size(), new_pointer_apply);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the pointer apply, so a late page reply can still overwrite the page you just opened.");
    }
    const std::string old_load_all =
        "\t\t\tPromise.all([\n"
        "\t\t\t\tcallServer(\"get_settings\"),\n"
        "\t\t\t\tcallServer(\"get_player_state_full\"),\n"
        "\t\t\t\tcallServer(\"get_music_folders\"),\n"
        "\t\t\t\tcallServer(\"get_playlists\"),\n"
        "\t\t\t]).then(function (results) {\n"
        "\t\t\t\tassignSettings(adoptSettings(results[0]) || {});\n";
    const std::string new_load_all =
        "\t\t\tcallServer(\"get_settings\").then(function (raw) {\n"
        "\t\t\t\tassignSettings(adoptSettings(raw) || {});\n"
        "\t\t\t\tUI.applySettingsToEngine();\n"
        "\t\t\t\tif (UI.currentTab === \"settings\") {\n"
        "\t\t\t\t\tUI.safely(\"renderSettingsView\", UI.renderSettingsView);\n"
        "\t\t\t\t}\n"
        "\t\t\t}).catch(function () {});\n"
        "\t\t\tcallServer(\"get_music_folders\").then(function (folders) {\n"
        "\t\t\t\tUI.applyFolders(folders);\n"
        "\t\t\t}).catch(function () {});\n"
        "\t\t\tcallServer(\"get_playlists\").then(function (lists) {\n"
        "\t\t\t\tApp.playlists = asArray(lists);\n"
        "\t\t\t}).catch(function () {});\n"
        "\t\t\tPromise.all([\n"
        "\t\t\t\tcallServer(\"get_settings\"),\n"
        "\t\t\t\tcallServer(\"get_player_state_full\"),\n"
        "\t\t\t\tcallServer(\"get_music_folders\"),\n"
        "\t\t\t\tcallServer(\"get_playlists\"),\n"
        "\t\t\t]).then(function (results) {\n"
        "\t\t\t\tassignSettings(adoptSettings(results[0]) || {});\n";
    const size_t load_all_at = ui.find(old_load_all);
    if (load_all_at != std::string::npos) {
        ui.replace(load_all_at, old_load_all.size(), new_load_all);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not load settings on their own, so the overlay waits on the full queue before it can show folders or the color theme.");
    }
    const std::string old_settings_tab =
        "\t\t\tif (tab === \"settings\") {\n"
        "\t\t\t\tUI.ensureFoldersLoaded();\n"
        "\t\t\t}\n";
    const std::string new_settings_tab =
        "\t\t\tif (tab === \"settings\") {\n"
        "\t\t\t\tUI.ensureFoldersLoaded();\n"
        "\t\t\t\tUI.safely(\"renderSettingsView\", UI.renderSettingsView);\n"
        "\t\t\t}\n";
    const size_t settings_tab_at = ui.find(old_settings_tab);
    if (settings_tab_at != std::string::npos) {
        ui.replace(settings_tab_at, old_settings_tab.size(), new_settings_tab);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not draw Settings when that tab was opened, so the page stays empty until a later poll.");
    }
    const std::string old_library_cache =
        "\t\t\tvar cacheReady = readCachedLibrary();\n"
        "\t\t\tif (!asFolderList(App.folders).length) {\n";
    const std::string new_library_cache =
        "\t\t\tvar cacheReady = readCachedLibrary();\n"
        "\t\t\tcacheReady.then(function (cached) {\n"
        "\t\t\t\tif (!cached || (cached.v !== 1 && cached.v !== LIBRARY_SNAPSHOT_VERSION) || !cached.t || UI.libraryAppliedFromServer) {\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tUI.applyLibraryPayload(cached, { skipCache: true, fromCache: true });\n"
        "\t\t\t\treportEvent(\"library load cache count=\" + asArray(App.library).length);\n"
        "\t\t\t});\n"
        "\t\t\tif (!asFolderList(App.folders).length) {\n";
    const size_t library_cache_at = ui.find(old_library_cache);
    if (library_cache_at != std::string::npos) {
        ui.replace(library_cache_at, old_library_cache.size(), new_library_cache);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not read the saved library first, so the overlay stays on Loading library until the server answers.");
    }
    const std::string old_album_safe =
        "\t\t\tif (group.kind === \"album\") {\n"
        "\t\t\t\tvar coverTrack = (group.tracks && group.tracks[0]) || null;\n"
        "\t\t\t\tvar coverTrackId = (coverTrack && coverTrack.id) || group.artTrackId;\n"
        "\t\t\t\tvar albumSafe = UI.albumArtSafe(\n"
        "\t\t\t\t\tcoverTrack || {\n"
        "\t\t\t\t\t\tartist: group.artist,\n"
        "\t\t\t\t\t\talbum: group.album || group.title,\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t);\n";
    const std::string new_album_safe =
        "\t\t\tif (group.kind === \"album\") {\n"
        "\t\t\t\tvar albumSafe = UI.albumArtSafe({\n"
        "\t\t\t\t\tartist: group.artist,\n"
        "\t\t\t\t\talbumArtist: group.artist,\n"
        "\t\t\t\t\talbum: group.album || group.title,\n"
        "\t\t\t\t});\n"
        "\t\t\t\tvar coverTrack = null;\n"
        "\t\t\t\tvar coverList = group.tracks || [];\n"
        "\t\t\t\tvar coverIndex;\n"
        "\t\t\t\tfor (coverIndex = 0; coverIndex < coverList.length; coverIndex++) {\n"
        "\t\t\t\t\tif (UI.albumArtSafe(coverList[coverIndex]) === albumSafe) {\n"
        "\t\t\t\t\t\tcoverTrack = coverList[coverIndex];\n"
        "\t\t\t\t\t\tbreak;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (!coverTrack) {\n"
        "\t\t\t\t\tcoverTrack = coverList[0] || null;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar coverTrackId = (coverTrack && coverTrack.id) || group.artTrackId;\n";
    const size_t album_safe_at = ui.find(old_album_safe);
    if (album_safe_at != std::string::npos) {
        ui.replace(album_safe_at, old_album_safe.size(), new_album_safe);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not name album covers from the album artist, so Album and Genre tiles can miss a cover the Artist page shows.");
    }
    const std::string old_grid_cache =
        "\t\t\tif (page.metrics && page.metrics.width === width) {\n"
        "\t\t\t\treturn page.metrics;\n"
        "\t\t\t}\n";
    const std::string new_grid_cache =
        "\t\t\tvar wantCols = UI.doomnetArtCols || 14;\n"
        "\t\t\tif (document.documentElement.classList.contains(\"smp-ctx-overlay\")) {\n"
        "\t\t\t\twantCols = 4;\n"
        "\t\t\t}\n"
        "\t\t\tif (page.metrics && page.metrics.width === width && page.metrics.doomnetCols === wantCols) {\n"
        "\t\t\t\treturn page.metrics;\n"
        "\t\t\t}\n";
    const size_t grid_cache_at = ui.find(old_grid_cache);
    if (grid_cache_at != std::string::npos) {
        ui.replace(grid_cache_at, old_grid_cache.size(), new_grid_cache);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not measure the library grid, so the artwork size control cannot change how many covers fit across.");
    }
    const std::string old_grid_cols = "\t\t\tvar cols = Math.max(1, Math.floor((width + gap) / (minCard + gap)));\n";
    const std::string new_grid_cols =
        "\t\t\tvar cols = wantCols;\n"
        "\t\t\tvar cardGuess = (width - gap * (cols - 1)) / cols;\n"
        "\t\t\tvar minCardPx = document.documentElement.classList.contains(\"smp-ctx-overlay\") ? 128 : 72;\n"
        "\t\t\twhile (cardGuess < minCardPx && cols > 2) {\n"
        "\t\t\t\tcols -= 1;\n"
        "\t\t\t\tcardGuess = (width - gap * (cols - 1)) / cols;\n"
        "\t\t\t}\n"
        "\t\t\tcols = Math.max(1, cols);\n";
    const size_t grid_cols_at = ui.find(old_grid_cols);
    if (grid_cols_at != std::string::npos) {
        ui.replace(grid_cols_at, old_grid_cols.size(), new_grid_cols);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not choose the library column count, so the artwork size control cannot change how many covers fit across.");
    }
    const std::string old_set_cover = "\t\t\t\tif (coverTrackId != null) {\n";
    const std::string new_set_cover = "\t\t\t\tif (coverTrackId != null && !document.documentElement.classList.contains(\"smp-ctx-overlay\")) {\n";
    const size_t set_cover_at = ui.find(old_set_cover);
    if (set_cover_at != std::string::npos) {
        ui.replace(set_cover_at, old_set_cover.size(), new_set_cover);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the album Set Cover button, so the mini player can still change covers.");
    }
    const std::string old_drill_cover = "\t\t\tactions.appendChild(setCoverBtn);\n";
    const std::string new_drill_cover =
        "\t\t\tif (!document.documentElement.classList.contains(\"smp-ctx-overlay\")) {\n"
        "\t\t\t\tactions.appendChild(setCoverBtn);\n"
        "\t\t\t}\n";
    const size_t drill_cover_at = ui.find(old_drill_cover);
    if (drill_cover_at != std::string::npos) {
        ui.replace(drill_cover_at, old_drill_cover.size(), new_drill_cover);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the album page Set Cover button, so the mini player can still change covers.");
    }
    const std::string old_metrics_cols = "\t\t\t\tcols: cols,\n";
    const std::string new_metrics_cols = "\t\t\t\tcols: cols,\n\t\t\t\tdoomnetCols: wantCols,\n";
    const size_t metrics_cols_at = ui.find(old_metrics_cols);
    if (metrics_cols_at != std::string::npos) {
        ui.replace(metrics_cols_at, old_metrics_cols.size(), new_metrics_cols);
    }
    const std::string old_subtabs_end =
        "\t\t\t\tchrome.subtabs.appendChild(btn);\n"
        "\t\t\t});\n"
        "\n"
        "\t\t\tvar pane = chrome.pane;\n";
    const std::string new_subtabs_end =
        "\t\t\t\tchrome.subtabs.appendChild(btn);\n"
        "\t\t\t});\n"
        "\t\t\tif (!document.documentElement.classList.contains(\"smp-ctx-overlay\") && (UI.libraryGroupBy === \"artist\" || UI.libraryGroupBy === \"album\" || UI.libraryGroupBy === \"genre\")) {\n"
        "\t\t\t\tUI.doomnetMountArtSize(chrome.subtabs);\n"
        "\t\t\t}\n"
        "\n"
        "\t\t\tvar pane = chrome.pane;\n";
    const size_t subtabs_end_at = ui.find(old_subtabs_end);
    if (subtabs_end_at != std::string::npos) {
        ui.replace(subtabs_end_at, old_subtabs_end.size(), new_subtabs_end);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not build the Artist row, so the artwork size control has nowhere to sit.");
    }
    const std::string old_seek_step =
        "\t\t\t\tmax: String(duration > 0 ? duration : 1),\n"
        "\t\t\t\tstep: \"0.1\",\n";
    const std::string new_seek_step =
        "\t\t\t\tmax: String(duration > 0 ? duration : 1),\n"
        "\t\t\t\tstep: \"0.01\",\n";
    const size_t seek_step_at = ui.find(old_seek_step);
    if (seek_step_at != std::string::npos) {
        ui.replace(seek_step_at, old_seek_step.size(), new_seek_step);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not set the time bar step, so scrubbing stays on tenths of a second.");
    }
    const std::string old_volume_step =
        "\t\t\t\tmax: \"1500\",\n"
        "\t\t\t\tstep: \"1\",\n"
        "\t\t\t\ttitle: \"Volume\",\n";
    const std::string new_volume_step =
        "\t\t\t\tmax: \"1500\",\n"
        "\t\t\t\tstep: \"0.01\",\n"
        "\t\t\t\ttitle: \"Volume\",\n";
    const size_t volume_step_at = ui.find(old_volume_step);
    if (volume_step_at != std::string::npos) {
        ui.replace(volume_step_at, old_volume_step.size(), new_volume_step);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not set the volume step, so the slider stays on coarse ticks.");
    }
    const std::string old_np_clock =
        "\t\t\t\tvar bar = UI.root && UI.root.querySelector(\".smp-nowplaying\");\n"
        "\t\t\t\tif (!bar || UI.scrubbing) {\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n";
    const std::string new_np_clock =
        "\t\t\t\tvar bar = UI.root && UI.root.querySelector(\".smp-nowplaying\");\n"
        "\t\t\t\tUI.doomnetSyncNpTransport();\n"
        "\t\t\t\tif (!bar || UI.scrubbing) {\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n";
    const size_t np_clock_at = ui.find(old_np_clock);
    if (np_clock_at != std::string::npos) {
        ui.replace(np_clock_at, old_np_clock.size(), new_np_clock);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not tick the progress clock, so the Now Playing transport stays on the last time.");
    }
    const std::string old_np_mount =
        "\t\t\tview.appendChild(wrap);\n"
        "\t\t\tUI.updateNowPlayingHero(wrap, track);\n";
    const std::string new_np_mount =
        "\t\t\tview.appendChild(wrap);\n"
        "\t\t\tUI.doomnetMountNowPlayingFlanks(wrap);\n"
        "\t\t\tUI.doomnetMountNpTransport(wrap);\n"
        "\t\t\tUI.updateNowPlayingHero(wrap, track);\n";
    const size_t np_mount_at = ui.find(old_np_mount);
    if (np_mount_at != std::string::npos) {
        ui.replace(np_mount_at, old_np_mount.size(), new_np_mount);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not attach the Now Playing page, so the EQ and spectrum have nowhere to sit.");
    }
    const std::string old_np_update =
        "\t\t\tUI.updateNowPlayingHero(wrap, track);\n"
        "\t\t\tUI.renderNowPlayingQueue(wrap);\n";
    const std::string new_np_update =
        "\t\t\tUI.updateNowPlayingHero(wrap, track);\n"
        "\t\t\tUI.doomnetMountNowPlayingFlanks(wrap);\n"
        "\t\t\tUI.doomnetMountNpTransport(wrap);\n"
        "\t\t\tUI.renderNowPlayingQueue(wrap);\n";
    const size_t np_update_at = ui.find(old_np_update);
    if (np_update_at != std::string::npos) {
        ui.replace(np_update_at, old_np_update.size(), new_np_update);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not refresh Now Playing, so the EQ stays on the last preset.");
    }
    const std::string old_apply_mix =
        "\t\tapplySettingsToEngine: function () {\n"
        "\t\t\tUI.applyAppearanceSettings();\n";
    const std::string new_apply_mix =
        "\t\tapplySettingsToEngine: function () {\n"
        "\t\t\tUI.applyAppearanceSettings();\n"
        "\t\t\tUI.doomnetRefreshMixBars();\n";
    const size_t apply_mix_at = ui.find(old_apply_mix);
    if (apply_mix_at != std::string::npos) {
        ui.replace(apply_mix_at, old_apply_mix.size(), new_apply_mix);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not apply settings to the engine, so the Now Playing mix bars stay on the last values.");
    }
    const std::string old_settings_eq =
        "\t\t\t\tslider.addEventListener(\"input\", function () {\n"
        "\t\t\t\t\tvar value = parseFloat(slider.value);\n"
        "\t\t\t\t\treadout.textContent = (value > 0 ? \"+\" : \"\") + value;\n"
        "\t\t\t\t\tApp.settings[band[0]] = value;\n"
        "\t\t\t\t\tpushMixToAudioOwner(App.settings);\n"
        "\t\t\t\t});\n";
    const std::string new_settings_eq =
        "\t\t\t\tslider.addEventListener(\"input\", function () {\n"
        "\t\t\t\t\tvar value = parseFloat(slider.value);\n"
        "\t\t\t\t\treadout.textContent = (value > 0 ? \"+\" : \"\") + value;\n"
        "\t\t\t\t\treadout.style.color = value === 0 ? \"\" : \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\t\t\tUI.doomnetPaintEqSlider(slider);\n"
        "\t\t\t\t\tApp.settings[band[0]] = value;\n"
        "\t\t\t\t\tpushMixToAudioOwner(App.settings);\n"
        "\t\t\t\t});\n";
    const size_t settings_eq_at = ui.find(old_settings_eq);
    if (settings_eq_at != std::string::npos) {
        ui.replace(settings_eq_at, old_settings_eq.size(), new_settings_eq);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not paint the settings equalizer while a band moves, so those faders stay on the browser slider.");
    }
    const std::string old_settings_eq_mount = "\t\t\t\ttrack.appendChild(slider);\n";
    const std::string new_settings_eq_mount =
        "\t\t\t\ttrack.appendChild(slider);\n"
        "\t\t\t\tUI.doomnetPaintEqSlider(slider);\n"
        "\t\t\t\tif (current !== 0) {\n"
        "\t\t\t\t\treadout.style.color = \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\t\t}\n";
    const size_t settings_eq_mount_at = ui.find(old_settings_eq_mount);
    if (settings_eq_mount_at != std::string::npos) {
        ui.replace(settings_eq_mount_at, old_settings_eq_mount.size(), new_settings_eq_mount);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not build the settings equalizer tracks, so those faders stay on the browser slider.");
    }
    const std::string old_marquee =
        "\t\t\t\t\tvar overflow = inner.scrollWidth - line.clientWidth;\n"
        "\t\t\t\t\tif (overflow <= 2) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n";
    const std::string new_marquee =
        "\t\t\t\t\tvar box = line.clientWidth;\n"
        "\t\t\t\t\tif (box < 8) {\n"
        "\t\t\t\t\t\tvar tries = (line._smpMarqueeTries || 0) + 1;\n"
        "\t\t\t\t\t\tline._smpMarqueeTries = tries;\n"
        "\t\t\t\t\t\tif (tries < 8) {\n"
        "\t\t\t\t\t\t\tsetTimeout(function () { UI.armMarquee(line); }, 60);\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tline._smpMarqueeTries = 0;\n"
        "\t\t\t\t\tvar overflow = inner.scrollWidth - box;\n"
        "\t\t\t\t\tif (overflow <= 4) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n";
    const size_t marquee_at = ui.find(old_marquee);
    if (marquee_at != std::string::npos) {
        ui.replace(marquee_at, old_marquee.size(), new_marquee);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not measure scrolling titles, so a title can scroll even when it already fits.");
    }
    const std::string old_put = "ArtStore.put(albumSafe, UI.artUrlForAlbum(albumSafe, ext));";
    const std::string new_put = "ArtStore.put(albumSafe, url.indexOf(\"data:\") === 0 ? url : UI.artUrlForAlbum(albumSafe, ext));";
    const size_t put_at = ui.find(old_put);
    if (put_at != std::string::npos) {
        ui.replace(put_at, old_put.size(), new_put);
    }
    const std::string old_library_boot = "attemptsLeft = attemptsLeft == null ? 8 : attemptsLeft;";
    const std::string new_library_boot =
        "attemptsLeft = attemptsLeft == null ? 8 : attemptsLeft;\n"
        "\t\t\tUI.ensureLibraryLoaded();";
    const size_t library_boot_at = ui.find(old_library_boot);
    if (library_boot_at != std::string::npos) {
        ui.replace(library_boot_at, old_library_boot.size(), new_library_boot);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not start the library load at boot, so the first library open still waits on the snapshot.");
    }
    const std::string old_warm = "UI.invalidateBrowseCache();";
    const std::string new_warm =
        "UI.invalidateBrowseCache();\n"
        "\t\t\t\tUI.ensureBrowseCards(\"artist\");\n"
        "\t\t\t\tUI.ensureBrowseCards(\"album\");\n"
        "\t\t\t\tUI.ensureBrowseCards(\"genre\");";
    const size_t warm_at = ui.find(old_warm);
    if (warm_at != std::string::npos) {
        ui.replace(warm_at, old_warm.size(), new_warm);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not warm the library cards after the snapshot loaded.");
    }
    const std::string old_artist_title = "artistEl.title = \"Albums by \" + artistName;";
    const std::string new_artist_title =
        "artistEl.title = \"Albums by \" + artistName;\n"
        "\t\t\t\t\tvar artistText = artistEl.querySelector(\".smp-marquee-inner\");\n"
        "\t\t\t\t\tif (artistText) { artistText.title = artistEl.title; artistEl.removeAttribute(\"title\"); }";
    const size_t artist_title_at = ui.find(old_artist_title);
    if (artist_title_at != std::string::npos) {
        ui.replace(artist_title_at, old_artist_title.size(), new_artist_title);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the now-playing artist title, so the artist link still uses the whole row.");
    }
    const std::string old_paint =
        "\t\t\timg.onerror = finishFail;\n"
        "\t\t\tif (img.getAttribute(\"src\") === dataUrl && img.complete) {\n"
        "\t\t\t\tif (img.naturalWidth) {\n"
        "\t\t\t\t\tfinishOk();\n"
        "\t\t\t\t} else {\n"
        "\t\t\t\t\tfinishFail();\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\timg.src = dataUrl;\n"
        "\t\t},";
    const std::string new_paint =
        "\t\t\tvar thumbUrl = UI.doomnetThumbUrl(artEl, dataUrl);\n"
        "\t\t\timg.onerror = function () {\n"
        "\t\t\t\tif (thumbUrl !== dataUrl && img.getAttribute(\"src\") === thumbUrl) {\n"
        "\t\t\t\t\tUI.doomnetNoThumb[thumbUrl] = true;\n"
        "\t\t\t\t\timg.src = dataUrl;\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tfinishFail();\n"
        "\t\t\t};\n"
        "\t\t\tif ((img.getAttribute(\"src\") === thumbUrl || img.getAttribute(\"src\") === dataUrl) && img.complete) {\n"
        "\t\t\t\tif (img.naturalWidth) {\n"
        "\t\t\t\t\tfinishOk();\n"
        "\t\t\t\t} else {\n"
        "\t\t\t\t\tfinishFail();\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\timg.src = thumbUrl;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetNoThumb: {},\n"
        "\n"
        "\t\tdoomnetThumbUrl: function (artEl, url) {\n"
        "\t\t\tif (!artEl || typeof url !== \"string\" || url.indexOf(\"/steam-music-player/art/albums/\") === -1) {\n"
        "\t\t\t\treturn url;\n"
        "\t\t\t}\n"
        "\t\t\tif (artEl.classList.contains(\"smp-nptab-art\") || artEl.classList.contains(\"smp-nptab-backdrop\")) {\n"
        "\t\t\t\treturn url;\n"
        "\t\t\t}\n"
        "\t\t\tif ((artEl.clientWidth || 0) * (window.devicePixelRatio || 1) > 400) {\n"
        "\t\t\t\treturn url;\n"
        "\t\t\t}\n"
        "\t\t\tvar thumb = url.replace(\"/steam-music-player/art/albums/\", \"/steam-music-player/art/thumbs/\").replace(/\\.(jpg|jpeg|png)(\\?|$)/i, \".jpg$2\");\n"
        "\t\t\treturn UI.doomnetNoThumb[thumb] ? url : thumb;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEqCurve: function () {\n"
        "\t\t\tvar keys = [\"eq32\", \"eq64\", \"eq125\", \"eq250\", \"eq500\", \"eq1000\", \"eq2000\", \"eq4000\", \"eq8000\", \"eq16000\"];\n"
        "\t\t\tvar labels = [\"32\", \"64\", \"125\", \"250\", \"500\", \"1k\", \"2k\", \"4k\", \"8k\", \"16k\"];\n"
        "\t\t\tvar presets = {\n"
        "\t\t\t\tflat: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],\n"
        "\t\t\t\tgame: [0, 0, -1, -1, -2, -3, -4, -3, -1, 0],\n"
        "\t\t\t\tsoft: [0, 0, 0, 0, -1, -1, -2, -3, -4, -4],\n"
        "\t\t\t\tpunch: [3, 2, 1, 0, -1, -3, -4, -2, 0, 1],\n"
        "\t\t\t\tbass: [6, 5, 4, 2, 0, -1, -1, 0, 1, 2],\n"
        "\t\t\t\ttreble: [-2, -2, -1, 0, 1, 2, 3, 4, 5, 5],\n"
        "\t\t\t\tvocal: [-3, -2, -1, 0, 2, 4, 4, 2, 0, -2],\n"
        "\t\t\t\trock: [4, 3, 1, -1, -2, 1, 3, 4, 4, 3]\n"
        "\t\t\t};\n"
        "\t\t\tvar names = { flat: \"Flat\", game: \"Game Mix\", soft: \"Soft\", punch: \"Punch\", bass: \"Bass Boost\", treble: \"Treble Boost\", vocal: \"Vocal Clarity\", rock: \"Rock\", custom: \"Custom\" };\n"
        "\t\t\tvar order = [\"flat\", \"game\", \"soft\", \"punch\", \"bass\", \"treble\", \"vocal\", \"rock\", \"custom\"];\n"
        "\t\t\tvar options = [];\n"
        "\t\t\tvar n;\n"
        "\t\t\tfor (n = 0; n < order.length; n++) {\n"
        "\t\t\t\toptions.push({ value: order[n], name: names[order[n]] });\n"
        "\t\t\t}\n"
        "\t\t\tvar preset = (App.settings && App.settings.eqPreset) || \"flat\";\n"
        "\t\t\tvar engine = App.engine;\n"
        "\t\t\tvar gains;\n"
        "\t\t\tvar eqOn = settingFlag(App.settings && App.settings.eqEnabled, true);\n"
        "\t\t\tif (preset !== \"custom\" && presets[preset]) {\n"
        "\t\t\t\tgains = presets[preset].slice();\n"
        "\t\t\t} else if (preset !== \"custom\" && eqOn && engine && engine.eqFilters && engine.eqFilters.length === 10) {\n"
        "\t\t\t\tgains = [];\n"
        "\t\t\t\tvar band;\n"
        "\t\t\t\tfor (band = 0; band < 10; band++) {\n"
        "\t\t\t\t\tgains.push(Math.round(engine.eqFilters[band].gain.value));\n"
        "\t\t\t\t}\n"
        "\t\t\t} else {\n"
        "\t\t\t\tgains = keys.map(function (key) {\n"
        "\t\t\t\t\treturn Math.round(asSettingNumber(App.settings[key], 0));\n"
        "\t\t\t\t});\n"
        "\t\t\t}\n"
        "\t\t\treturn { keys: keys, labels: labels, preset: preset, name: names[preset] || \"Custom\", gains: gains, options: options };\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetApplyEqBand: function (index, value, commit) {\n"
        "\t\t\tvar curve = UI.doomnetEqCurve();\n"
        "\t\t\tvar next = Math.max(-12, Math.min(12, value));\n"
        "\t\t\tif (curve.preset !== \"custom\") {\n"
        "\t\t\t\tcurve.gains[index] = next;\n"
        "\t\t\t\tvar i;\n"
        "\t\t\t\tfor (i = 0; i < curve.keys.length; i++) {\n"
        "\t\t\t\t\tApp.settings[curve.keys[i]] = curve.gains[i];\n"
        "\t\t\t\t}\n"
        "\t\t\t\tApp.settings.eqPreset = \"custom\";\n"
        "\t\t\t\tApp.settings.eqEnabled = true;\n"
        "\t\t\t\tUI._doomnetEqBecameCustom = true;\n"
        "\t\t\t} else {\n"
        "\t\t\t\tApp.settings[curve.keys[index]] = next;\n"
        "\t\t\t}\n"
        "\t\t\tUI.applySettingsToEngine();\n"
        "\t\t\tpushMixToAudioOwner(App.settings);\n"
        "\t\t\tif (!commit) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (UI._doomnetEqBecameCustom) {\n"
        "\t\t\t\tUI._doomnetEqBecameCustom = false;\n"
        "\t\t\t\tvar writes = [[\"eqPreset\", \"custom\"], [\"eqEnabled\", true]];\n"
        "\t\t\t\tvar band;\n"
        "\t\t\t\tfor (band = 0; band < curve.keys.length; band++) {\n"
        "\t\t\t\t\twrites.push([curve.keys[band], App.settings[curve.keys[band]]]);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar chain = Promise.resolve();\n"
        "\t\t\t\twrites.forEach(function (pair) {\n"
        "\t\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\t\treturn UI.commitSetting(pair[0], pair[1], false);\n"
        "\t\t\t\t\t}).catch(function () {});\n"
        "\t\t\t\t});\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.commitSetting(curve.keys[index], next, false);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPaintEqSlider: function (slider) {\n"
        "\t\t\tif (!slider) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar value = Math.max(-12, Math.min(12, parseFloat(slider.value) || 0));\n"
        "\t\t\tvar pct = ((value + 12) / 24) * 100;\n"
        "\t\t\tvar lo = Math.min(50, pct);\n"
        "\t\t\tvar hi = Math.max(50, pct);\n"
        "\t\t\tvar track = \"rgba(255,255,255,0.16)\";\n"
        "\t\t\tvar accent = \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\tslider.style.setProperty(\"--doomnet-eq-fill\", \"linear-gradient(90deg, \" + track + \" 0%, \" + track + \" \" + lo + \"%, \" + accent + \" \" + lo + \"%, \" + accent + \" \" + hi + \"%, \" + track + \" \" + hi + \"%, \" + track + \" 100%)\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetCloseEqMenu: function () {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar menu = host && host.querySelector(\".doomnet-eq-menu\");\n"
        "\t\t\tvar btn = host && host.querySelector(\".doomnet-eq-preset\");\n"
        "\t\t\tif (menu) {\n"
        "\t\t\t\tmenu.hidden = true;\n"
        "\t\t\t}\n"
        "\t\t\tif (btn) {\n"
        "\t\t\t\tbtn.setAttribute(\"aria-expanded\", \"false\");\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetToggleEqMenu: function (root) {\n"
        "\t\t\tvar menu = root && root.querySelector(\".doomnet-eq-menu\");\n"
        "\t\t\tvar btn = root && root.querySelector(\".doomnet-eq-preset\");\n"
        "\t\t\tif (!menu || !btn) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar wasHidden = menu.hidden;\n"
        "\t\t\tUI.doomnetCloseEqMenu();\n"
        "\t\t\tif (!wasHidden) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar curve = UI.doomnetEqCurve();\n"
        "\t\t\tvar items = menu.querySelectorAll(\"button\");\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < items.length; i++) {\n"
        "\t\t\t\titems[i].classList.toggle(\"on\", items[i].getAttribute(\"data-preset\") === curve.preset);\n"
        "\t\t\t}\n"
        "\t\t\tmenu.hidden = false;\n"
        "\t\t\tbtn.setAttribute(\"aria-expanded\", \"true\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetChooseEqPreset: function (value) {\n"
        "\t\t\tUI.doomnetCloseEqMenu();\n"
        "\t\t\tif (!value || !App.settings || App.settings.eqPreset === value) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar turnOn = !settingFlag(App.settings.eqEnabled, true);\n"
        "\t\t\tApp.settings.eqPreset = value;\n"
        "\t\t\tif (turnOn) {\n"
        "\t\t\t\tApp.settings.eqEnabled = true;\n"
        "\t\t\t}\n"
        "\t\t\tUI.applySettingsToEngine();\n"
        "\t\t\tpushMixToAudioOwner(App.settings);\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar page = host && host.querySelector(\".smp-nptab\");\n"
        "\t\t\tif (page) {\n"
        "\t\t\t\tUI.doomnetRefreshEqFlank(page);\n"
        "\t\t\t}\n"
        "\t\t\tvar chain = Promise.resolve();\n"
        "\t\t\tif (turnOn) {\n"
        "\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\treturn UI.commitSetting(\"eqEnabled\", true, false);\n"
        "\t\t\t\t}).catch(function () {});\n"
        "\t\t\t}\n"
        "\t\t\tchain.then(function () {\n"
        "\t\t\t\treturn UI.commitSetting(\"eqPreset\", value, false);\n"
        "\t\t\t}).catch(function () {});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetRefreshEqFlank: function (wrap) {\n"
        "\t\t\tvar panel = wrap && wrap.querySelector(\".doomnet-flank-eq\");\n"
        "\t\t\tif (!panel || UI._doomnetEqDragging) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar curve = UI.doomnetEqCurve();\n"
        "\t\t\tvar title = panel.querySelector(\".doomnet-eq-preset-name\");\n"
        "\t\t\tif (title) {\n"
        "\t\t\t\ttitle.textContent = curve.name;\n"
        "\t\t\t}\n"
        "\t\t\tvar sliders = panel.querySelectorAll(\".doomnet-eq-slider\");\n"
        "\t\t\tvar readouts = panel.querySelectorAll(\".doomnet-eq-db\");\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < sliders.length; i++) {\n"
        "\t\t\t\tvar gain = curve.gains[i] || 0;\n"
        "\t\t\t\tsliders[i].value = String(gain);\n"
        "\t\t\t\tUI.doomnetPaintEqSlider(sliders[i]);\n"
        "\t\t\t\tif (readouts[i]) {\n"
        "\t\t\t\t\treadouts[i].textContent = (gain > 0 ? \"+\" : \"\") + gain;\n"
        "\t\t\t\t\treadouts[i].style.color = gain === 0 ? \"\" : \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMixSpec: function () {\n"
        "\t\t\treturn [\n"
        "\t\t\t\t{ key: \"targetLufs\", label: \"Target Loudness\", min: -31, max: -10, step: 1, fallback: -20, format: function (v) {\n"
        "\t\t\t\t\treturn v + \" LUFS\";\n"
        "\t\t\t\t} },\n"
        "\t\t\t\t{ key: \"duckStrength\", label: \"Ducking Strength\", min: 0, max: 1, step: 0.05, fallback: 0.8, format: function (v) {\n"
        "\t\t\t\t\treturn Math.round(v * 100) + \"%\";\n"
        "\t\t\t\t} },\n"
        "\t\t\t\t{ key: \"reverbAmount\", label: \"Space Amount\", min: 0, max: 1, step: 0.05, fallback: 0.4, format: function (v) {\n"
        "\t\t\t\t\treturn Math.round(v * 100) + \"%\";\n"
        "\t\t\t\t} }\n"
        "\t\t\t];\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPaintMixSlider: function (slider) {\n"
        "\t\t\tif (!slider) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar min = parseFloat(slider.min);\n"
        "\t\t\tvar max = parseFloat(slider.max);\n"
        "\t\t\tvar value = parseFloat(slider.value);\n"
        "\t\t\tvar pct = max === min ? 0 : ((value - min) / (max - min)) * 100;\n"
        "\t\t\tvar accent = \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\tvar track = \"rgba(255,255,255,0.16)\";\n"
        "\t\t\tslider.style.setProperty(\"--doomnet-mix-fill\", \"linear-gradient(90deg, \" + accent + \" 0%, \" + accent + \" \" + pct + \"%, \" + track + \" \" + pct + \"%, \" + track + \" 100%)\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetRefreshMixBars: function () {\n"
        "\t\t\tif (UI._doomnetMixDragging) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar root = host && host.querySelector(\".doomnet-mix\");\n"
        "\t\t\tif (!root || !App.settings) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar spec = UI.doomnetMixSpec();\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < spec.length; i++) {\n"
        "\t\t\t\tvar item = spec[i];\n"
        "\t\t\t\tvar slider = root.querySelector(\".doomnet-mix-slider[data-key='\" + item.key + \"']\");\n"
        "\t\t\t\tvar valueEl = root.querySelector(\".doomnet-mix-value[data-key='\" + item.key + \"']\");\n"
        "\t\t\t\tvar current = App.settings[item.key];\n"
        "\t\t\t\tif (typeof current !== \"number\") {\n"
        "\t\t\t\t\tcurrent = item.fallback;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (slider) {\n"
        "\t\t\t\t\tslider.value = String(current);\n"
        "\t\t\t\t\tUI.doomnetPaintMixSlider(slider);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (valueEl) {\n"
        "\t\t\t\t\tvalueEl.textContent = item.format(current);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetApplyMixBar: function (key, value, commit) {\n"
        "\t\t\tif (!App.settings || !key) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tApp.settings[key] = value;\n"
        "\t\t\tif (commit) {\n"
        "\t\t\t\tUI.commitSetting(key, value, false);\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tpushMixToAudioOwner(App.settings);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMountMixBars: function (parent) {\n"
        "\t\t\tif (!parent || parent.querySelector(\".doomnet-mix\")) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar spec = UI.doomnetMixSpec();\n"
        "\t\t\tvar box = el(\"div\", \"doomnet-mix\");\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < spec.length; i++) {\n"
        "\t\t\t\t(function (item) {\n"
        "\t\t\t\t\tvar current = App.settings && App.settings[item.key];\n"
        "\t\t\t\t\tif (typeof current !== \"number\") {\n"
        "\t\t\t\t\t\tcurrent = item.fallback;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar row = el(\"div\", \"doomnet-mix-row\");\n"
        "\t\t\t\t\trow.appendChild(el(\"span\", \"doomnet-mix-label\", { text: item.label }));\n"
        "\t\t\t\t\tvar line = el(\"div\", \"doomnet-mix-line\");\n"
        "\t\t\t\t\tvar valueEl = el(\"span\", \"doomnet-mix-value\", { text: item.format(current) });\n"
        "\t\t\t\t\tvalueEl.setAttribute(\"data-key\", item.key);\n"
        "\t\t\t\t\tvar slider = el(\"input\", \"doomnet-mix-slider\", {\n"
        "\t\t\t\t\t\ttype: \"range\",\n"
        "\t\t\t\t\t\tmin: String(item.min),\n"
        "\t\t\t\t\t\tmax: String(item.max),\n"
        "\t\t\t\t\t\tstep: String(item.step),\n"
        "\t\t\t\t\t\tvalue: String(current),\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.setAttribute(\"data-key\", item.key);\n"
        "\t\t\t\t\tslider.setAttribute(\"aria-label\", item.label);\n"
        "\t\t\t\t\tslider.addEventListener(\"pointerdown\", function () {\n"
        "\t\t\t\t\t\tUI._doomnetMixDragging = true;\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.addEventListener(\"input\", function () {\n"
        "\t\t\t\t\t\tUI._doomnetMixDragging = true;\n"
        "\t\t\t\t\t\tvar value = parseFloat(slider.value);\n"
        "\t\t\t\t\t\tvalueEl.textContent = item.format(value);\n"
        "\t\t\t\t\t\tUI.doomnetPaintMixSlider(slider);\n"
        "\t\t\t\t\t\tUI.doomnetApplyMixBar(item.key, value, false);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tvar commit = function () {\n"
        "\t\t\t\t\t\tUI._doomnetMixDragging = false;\n"
        "\t\t\t\t\t\tUI.doomnetApplyMixBar(item.key, parseFloat(slider.value), true);\n"
        "\t\t\t\t\t};\n"
        "\t\t\t\t\tslider.addEventListener(\"change\", commit);\n"
        "\t\t\t\t\tslider.addEventListener(\"keyup\", commit);\n"
        "\t\t\t\t\tUI.doomnetPaintMixSlider(slider);\n"
        "\t\t\t\t\tline.appendChild(slider);\n"
        "\t\t\t\t\tline.appendChild(valueEl);\n"
        "\t\t\t\t\trow.appendChild(line);\n"
        "\t\t\t\t\tbox.appendChild(row);\n"
        "\t\t\t\t})(spec[i]);\n"
        "\t\t\t}\n"
        "\t\t\tparent.appendChild(box);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEnsureAnalyser: function () {\n"
        "\t\t\tvar engine = App.engine;\n"
        "\t\t\tif (!engine || !engine.masterGain || !engine.audioContext) {\n"
        "\t\t\t\treturn null;\n"
        "\t\t\t}\n"
        "\t\t\tif (engine._doomnetMeter) {\n"
        "\t\t\t\treturn engine._doomnetMeter;\n"
        "\t\t\t}\n"
        "\t\t\tvar node = engine.audioContext.createAnalyser();\n"
        "\t\t\tnode.fftSize = 256;\n"
        "\t\t\tnode.smoothingTimeConstant = 0.8;\n"
        "\t\t\ttry {\n"
        "\t\t\t\tengine.masterGain.connect(node);\n"
        "\t\t\t} catch (err) {\n"
        "\t\t\t\treturn null;\n"
        "\t\t\t}\n"
        "\t\t\tengine._doomnetMeter = node;\n"
        "\t\t\treturn node;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetCenterNpBlock: function (wrap) {\n"
        "\t\t\tvar hero = wrap && wrap.querySelector(\".smp-nptab-hero\");\n"
        "\t\t\tvar queue = wrap && wrap.querySelector(\".smp-nptab-queue\");\n"
        "\t\t\tvar info = wrap && wrap.querySelector(\".smp-nptab-info\");\n"
        "\t\t\tif (!hero) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar host = wrap.closest && wrap.closest(\"#doomnet-steamtunes-host\");\n"
        "\t\t\thero.style.transform = \"\";\n"
        "\t\t\thero.style.width = \"\";\n"
        "\t\t\tif (info) {\n"
        "\t\t\t\tinfo.style.width = \"\";\n"
        "\t\t\t}\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tqueue.style.transform = \"\";\n"
        "\t\t\t\tqueue.style.width = \"\";\n"
        "\t\t\t}\n"
        "\t\t\tif (!host || !host.classList.contains(\"doomnet-np-horizontal\")) {\n"
        "\t\t\t\tUI.doomnetSlideNpBlock(wrap);\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (info) {\n"
        "\t\t\t\tinfo.style.width = Math.round(info.getBoundingClientRect().width + 100) + \"px\";\n"
        "\t\t\t}\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tqueue.style.width = Math.round(hero.getBoundingClientRect().width) + \"px\";\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetSlideNpBlock(wrap);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetSlideNpBlock: function (wrap) {\n"
        "\t\t\tvar hero = wrap && wrap.querySelector(\".smp-nptab-hero\");\n"
        "\t\t\tvar queue = wrap && wrap.querySelector(\".smp-nptab-queue\");\n"
        "\t\t\tif (!hero) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\thero.style.transform = \"none\";\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tqueue.style.transform = \"none\";\n"
        "\t\t\t}\n"
        "\t\t\tvar inline = wrap.querySelector(\".doomnet-flanks-inline\");\n"
        "\t\t\tvar eq = wrap.querySelector(\".doomnet-flank-eq\");\n"
        "\t\t\tvar lyrics = wrap.querySelector(\".doomnet-flank-meter\");\n"
        "\t\t\tif (inline || !eq || !lyrics) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar eqBox = eq.getBoundingClientRect();\n"
        "\t\t\tvar lyricBox = lyrics.getBoundingClientRect();\n"
        "\t\t\tif (eqBox.width < 8 || lyricBox.width < 8) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar heroBox = hero.getBoundingClientRect();\n"
        "\t\t\tvar left = heroBox.left;\n"
        "\t\t\tvar right = heroBox.right;\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tvar queueBox = queue.getBoundingClientRect();\n"
        "\t\t\t\tif (queueBox.width > 8 && queueBox.height > 8) {\n"
        "\t\t\t\t\tleft = Math.min(left, queueBox.left);\n"
        "\t\t\t\t\tright = Math.max(right, queueBox.right);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tvar mid = (eqBox.right + lyricBox.left) / 2;\n"
        "\t\t\tvar blockMid = (left + right) / 2;\n"
        "\t\t\tvar dx = Math.round(mid - blockMid);\n"
        "\t\t\tvar slide = \"translateX(\" + dx + \"px)\";\n"
        "\t\t\thero.style.transform = slide;\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tqueue.style.transform = slide;\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetLayoutFlanks: function (wrap) {\n"
        "\t\t\tvar row = wrap && wrap.querySelector(\".doomnet-flank-row\");\n"
        "\t\t\tvar hero = wrap && wrap.querySelector(\".smp-nptab-hero\");\n"
        "\t\t\tif (!row || !hero) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetCenterNpBlock(wrap);\n"
        "\t\t\tvar hostBox = wrap.getBoundingClientRect();\n"
        "\t\t\tvar art = wrap.querySelector(\".smp-nptab-art-wrap\");\n"
        "\t\t\tvar queue = wrap.querySelector(\".smp-nptab-queue\");\n"
        "\t\t\tvar hostEl = wrap.closest && wrap.closest(\"#doomnet-steamtunes-host\");\n"
        "\t\t\tvar parkedHorizontal = !!(hostEl && hostEl.classList.contains(\"doomnet-np-horizontal\"));\n"
        "\t\t\tvar heroWidth = hero.getBoundingClientRect().width;\n"
        "\t\t\tif (parkedHorizontal) {\n"
        "\t\t\t\thostEl.classList.remove(\"doomnet-np-horizontal\");\n"
        "\t\t\t}\n"
        "\t\t\tvar artBox = art ? art.getBoundingClientRect() : hero.getBoundingClientRect();\n"
        "\t\t\tif (parkedHorizontal) {\n"
        "\t\t\t\thostEl.classList.add(\"doomnet-np-horizontal\");\n"
        "\t\t\t}\n"
        "\t\t\tvar middle = parkedHorizontal ? heroWidth : artBox.width;\n"
        "\t\t\tif (queue) {\n"
        "\t\t\t\tmiddle = Math.max(middle, queue.getBoundingClientRect().width);\n"
        "\t\t\t}\n"
        "\t\t\tvar gap = 28;\n"
        "\t\t\tvar avail = Math.max(0, (hostBox.width - middle) / 2 - gap);\n"
        "\t\t\tvar inline = avail - 16 < 150;\n"
        "\t\t\trow.classList.toggle(\"doomnet-flanks-inline\", inline);\n"
        "\t\t\tvar eq = row.querySelector(\".doomnet-flank-eq\");\n"
        "\t\t\tvar meter = row.querySelector(\".doomnet-flank-meter\");\n"
        "\t\t\tvar fader;\n"
        "\t\t\tvar col;\n"
        "\t\t\tif (inline) {\n"
        "\t\t\t\tfader = Math.round(Math.max(72, Math.min(110, hostBox.height * 0.12)));\n"
        "\t\t\t\tcol = hostBox.width < 900 ? 20 : 26;\n"
        "\t\t\t\tif (eq) {\n"
        "\t\t\t\t\teq.style.left = \"\";\n"
        "\t\t\t\t\teq.style.top = \"\";\n"
        "\t\t\t\t\teq.style.width = \"\";\n"
        "\t\t\t\t\teq.style.height = \"\";\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (meter) {\n"
        "\t\t\t\t\tmeter.style.left = \"\";\n"
        "\t\t\t\t\tmeter.style.top = \"\";\n"
        "\t\t\t\t\tmeter.style.width = \"\";\n"
        "\t\t\t\t\tmeter.style.height = \"\";\n"
        "\t\t\t\t\tmeter.style.paddingTop = \"\";\n"
        "\t\t\t\t}\n"
        "\t\t\t} else {\n"
        "\t\t\t\tvar cover = Math.max(artBox.height, 120);\n"
        "\t\t\t\tfader = Math.round(Math.max(110, Math.min(280, Math.min(cover * 0.72, hostBox.height * 0.34))));\n"
        "\t\t\t\tvar edge = Math.max(16, Math.min(100, avail - 150));\n"
        "\t\t\t\tvar side = Math.max(0, avail - edge);\n"
        "\t\t\t\tvar wing = Math.min(side, Math.max(190, Math.min(320, hostBox.width * 0.17)));\n"
        "\t\t\t\tvar eqWidth = wing;\n"
        "\t\t\t\tvar meterWidth = eqWidth;\n"
        "\t\t\t\tcol = eqWidth < 240 ? 18 : 26;\n"
        "\t\t\t\tvar top = (artBox.top - hostBox.top) + \"px\";\n"
        "\t\t\t\tvar tallPx = artBox.height + \"px\";\n"
        "\t\t\t\tif (eq) {\n"
        "\t\t\t\t\teq.style.left = edge + \"px\";\n"
        "\t\t\t\t\teq.style.top = top;\n"
        "\t\t\t\t\teq.style.width = eqWidth + \"px\";\n"
        "\t\t\t\t\teq.style.height = tallPx;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (meter) {\n"
        "\t\t\t\t\tvar meterLeft = hostBox.width - edge - meterWidth;\n"
        "\t\t\t\t\tmeter.style.left = meterLeft + \"px\";\n"
        "\t\t\t\t\tmeter.style.top = top;\n"
        "\t\t\t\t\tmeter.style.width = meterWidth + \"px\";\n"
        "\t\t\t\t\tmeter.style.height = tallPx;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\trow.style.setProperty(\"--doomnet-fader\", fader + \"px\");\n"
        "\t\t\trow.style.setProperty(\"--doomnet-meter\", Math.round(fader * 0.82) + \"px\");\n"
        "\t\t\trow.style.setProperty(\"--doomnet-col\", col + \"px\");\n"
        "\t\t\tif (!inline && eq && meter) {\n"
        "\t\t\t\tvar preset = eq.querySelector(\".doomnet-eq-preset\");\n"
        "\t\t\t\tif (preset) {\n"
        "\t\t\t\t\tvar presetTop = preset.getBoundingClientRect().top - eq.getBoundingClientRect().top;\n"
        "\t\t\t\t\tmeter.style.paddingTop = Math.max(0, Math.round(presetTop)) + \"px\";\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tif (artBox.height < 40 && !row._doomnetLaidOut) {\n"
        "\t\t\t\trow._doomnetLaidOut = true;\n"
        "\t\t\t\trequestAnimationFrame(function () {\n"
        "\t\t\t\t\tUI.doomnetLayoutFlanks(wrap);\n"
        "\t\t\t\t});\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetSlideNpBlock(wrap);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMetersWanted: function () {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tif (!host || !host.classList.contains(\"doomnet-on\") || UI.currentTab !== \"library\" || UI.libraryGroupBy !== \"nowplaying\") {\n"
        "\t\t\t\treturn false;\n"
        "\t\t\t}\n"
        "\t\t\treturn !!host.querySelector(\".doomnet-spectrum\") && !host.querySelector(\".smp-nptab.doomnet-flanks-off\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPaintMeters: function () {\n"
        "\t\t\tUI._doomnetMeterFrame = null;\n"
        "\t\t\tif (!UI.doomnetMetersWanted()) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar node = UI.doomnetEnsureAnalyser();\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar canvas = host && host.querySelector(\".doomnet-spectrum\");\n"
        "\t\t\tvar fill = host && host.querySelector(\".doomnet-loudness-fill\");\n"
        "\t\t\tvar label = host && host.querySelector(\".doomnet-loudness-db\");\n"
        "\t\t\tif (node && canvas) {\n"
        "\t\t\t\tvar bins = node.frequencyBinCount;\n"
        "\t\t\t\tvar freq = UI._doomnetFreq;\n"
        "\t\t\t\tif (!freq || freq.length !== bins) {\n"
        "\t\t\t\t\tfreq = UI._doomnetFreq = new Uint8Array(bins);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tnode.getByteFrequencyData(freq);\n"
        "\t\t\t\tvar g = canvas.getContext(\"2d\");\n"
        "\t\t\t\tvar w = canvas.width;\n"
        "\t\t\t\tvar h = canvas.height;\n"
        "\t\t\t\tg.clearRect(0, 0, w, h);\n"
        "\t\t\t\tvar bars = 32;\n"
        "\t\t\t\tvar step = Math.floor(bins / bars);\n"
        "\t\t\t\tvar gap = 2;\n"
        "\t\t\t\tvar barW = (w - gap * (bars - 1)) / bars;\n"
        "\t\t\t\tvar b;\n"
        "\t\t\t\tvar accent = \"#66c0f4\";\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tvar read = getComputedStyle(document.documentElement).getPropertyValue(\"--smp-accent\");\n"
        "\t\t\t\t\tif (read && read.replace) {\n"
        "\t\t\t\t\t\tread = read.replace(/^\\s+|\\s+$/g, \"\");\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tif (read) {\n"
        "\t\t\t\t\t\taccent = read;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t} catch (err) {}\n"
        "\t\t\t\tg.globalAlpha = 0.9;\n"
        "\t\t\t\tg.fillStyle = accent;\n"
        "\t\t\t\tfor (b = 0; b < bars; b++) {\n"
        "\t\t\t\t\tvar mag = 0;\n"
        "\t\t\t\t\tvar s;\n"
        "\t\t\t\t\tfor (s = 0; s < step; s++) {\n"
        "\t\t\t\t\t\tmag += freq[b * step + s] || 0;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tmag = mag / step / 255;\n"
        "\t\t\t\t\tvar bh = Math.max(1, mag * h);\n"
        "\t\t\t\t\tg.fillRect(b * (barW + gap), h - bh, barW, bh);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tg.globalAlpha = 1;\n"
        "\t\t\t\tvar time = UI._doomnetTime;\n"
        "\t\t\t\tif (!time || time.length !== node.fftSize) {\n"
        "\t\t\t\t\ttime = UI._doomnetTime = new Uint8Array(node.fftSize);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tnode.getByteTimeDomainData(time);\n"
        "\t\t\t\tvar sum = 0;\n"
        "\t\t\t\tvar n;\n"
        "\t\t\t\tfor (n = 0; n < time.length; n++) {\n"
        "\t\t\t\t\tvar sample = (time[n] - 128) / 128;\n"
        "\t\t\t\t\tsum += sample * sample;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar rms = Math.sqrt(sum / time.length);\n"
        "\t\t\t\tUI._doomnetRmsSum = (UI._doomnetRmsSum || 0) + rms;\n"
        "\t\t\t\tUI._doomnetRmsCount = (UI._doomnetRmsCount || 0) + 1;\n"
        "\t\t\t\tvar frameDb = rms > 0.00001 ? 20 * Math.log(rms) / Math.LN10 : -60;\n"
        "\t\t\t\tframeDb = Math.max(-60, Math.min(0, frameDb));\n"
        "\t\t\t\tif (fill) {\n"
        "\t\t\t\t\tfill.style.width = ((frameDb + 60) / 60 * 100) + \"%\";\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar nowMs = (window.performance && performance.now) ? performance.now() : Date.now();\n"
        "\t\t\t\tif (label && (!UI._doomnetMeterAt || nowMs - UI._doomnetMeterAt >= 250)) {\n"
        "\t\t\t\t\tvar mean = UI._doomnetRmsSum / Math.max(1, UI._doomnetRmsCount);\n"
        "\t\t\t\t\tvar db = mean > 0.00001 ? 20 * Math.log(mean) / Math.LN10 : -60;\n"
        "\t\t\t\t\tdb = Math.max(-60, Math.min(0, db));\n"
        "\t\t\t\t\tlabel.textContent = (db <= -59.5 ? \"-inf\" : db.toFixed(1)) + \" dB\";\n"
        "\t\t\t\t\tUI._doomnetRmsSum = 0;\n"
        "\t\t\t\t\tUI._doomnetRmsCount = 0;\n"
        "\t\t\t\t\tUI._doomnetMeterAt = nowMs;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tvar lyricBox = host && host.querySelector(\".doomnet-lyrics\");\n"
        "\t\t\tvar lyricBody = host && host.querySelector(\".doomnet-lyrics-body\");\n"
        "\t\t\tvar lyricLines = UI._doomnetLyricLines;\n"
        "\t\t\tif (lyricBox && lyricBody && lyricLines && lyricLines.length) {\n"
        "\t\t\t\tif (!UI._doomnetLyricSynced) {\n"
        "\t\t\t\t\tUI._doomnetMeterFrame = requestAnimationFrame(UI.doomnetPaintMeters);\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar elapsed = App.engine && App.engine.getElapsedSeconds ? (App.engine.getElapsedSeconds() || 0) : 0;\n"
        "\t\t\t\tvar view = lyricBox.clientHeight;\n"
        "\t\t\t\tvar textH = lyricBody.scrollHeight;\n"
        "\t\t\t\tvar shift = view * 0.28;\n"
        "\t\t\t\tif (UI._doomnetLyricSynced) {\n"
        "\t\t\t\t\tvar idx = 0;\n"
        "\t\t\t\t\tvar li;\n"
        "\t\t\t\t\tfor (li = 0; li < lyricLines.length; li++) {\n"
        "\t\t\t\t\t\tif (lyricLines[li].t <= elapsed) {\n"
        "\t\t\t\t\t\t\tidx = li;\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar rows = lyricBody.children;\n"
        "\t\t\t\t\tfor (li = 0; li < rows.length; li++) {\n"
        "\t\t\t\t\t\trows[li].classList.toggle(\"on\", li === idx);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar row = rows[idx];\n"
        "\t\t\t\t\tif (row) {\n"
        "\t\t\t\t\t\tshift = view * 0.5 - row.offsetTop - row.offsetHeight * 0.5;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar lowest = Math.min(view * 0.22, view - textH);\n"
        "\t\t\t\t\tif (shift < lowest) {\n"
        "\t\t\t\t\t\tshift = lowest;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tif (shift > view * 0.34) {\n"
        "\t\t\t\t\t\tshift = view * 0.34;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tlyricBody.style.transform = \"translateY(\" + shift + \"px)\";\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tUI._doomnetMeterFrame = requestAnimationFrame(UI.doomnetPaintMeters);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetShowLyrics: function (payload) {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar body = host && host.querySelector(\".doomnet-lyrics-body\");\n"
        "\t\t\tif (!body) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (body._doomnetLyricAnim) {\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tbody._doomnetLyricAnim.cancel();\n"
        "\t\t\t\t} catch (e) {}\n"
        "\t\t\t\tbody._doomnetLyricAnim = null;\n"
        "\t\t\t}\n"
        "\t\t\tvar text = payload && payload.text ? String(payload.text) : \"\";\n"
        "\t\t\tvar rows = text.split(/\\r?\\n/);\n"
        "\t\t\tvar synced = [];\n"
        "\t\t\tvar plain = [];\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < rows.length; i++) {\n"
        "\t\t\t\tvar row = rows[i];\n"
        "\t\t\t\tvar mark = row.match(/^\\[(\\d+):(\\d+(?:\\.\\d+)?)\\](.*)$/);\n"
        "\t\t\t\tif (mark) {\n"
        "\t\t\t\t\tsynced.push({ t: Number(mark[1]) * 60 + Number(mark[2]), text: mark[3] });\n"
        "\t\t\t\t} else if (row && !/^\\[[A-Za-z]+:/.test(row)) {\n"
        "\t\t\t\t\tplain.push(row);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tbody.textContent = \"\";\n"
        "\t\t\tif (synced.length) {\n"
        "\t\t\t\tUI._doomnetLyricSynced = true;\n"
        "\t\t\t\tUI._doomnetLyricLines = synced;\n"
        "\t\t\t\tfor (i = 0; i < synced.length; i++) {\n"
        "\t\t\t\t\tbody.appendChild(el(\"div\", \"doomnet-lyric-line\", { text: synced[i].text || \" \" }));\n"
        "\t\t\t\t}\n"
        "\t\t\t} else {\n"
        "\t\t\t\tUI._doomnetLyricSynced = false;\n"
        "\t\t\t\tUI._doomnetLyricLines = plain.length ? plain : null;\n"
        "\t\t\t\tbody.textContent = plain.join(\"\\n\");\n"
        "\t\t\t\tUI.doomnetArmLyricScroll();\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetArmLyricScroll: function () {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar box = host && host.querySelector(\".doomnet-lyrics\");\n"
        "\t\t\tvar body = host && host.querySelector(\".doomnet-lyrics-body\");\n"
        "\t\t\tif (!box || !body) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (body._doomnetLyricAnim) {\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tbody._doomnetLyricAnim.cancel();\n"
        "\t\t\t\t} catch (e) {}\n"
        "\t\t\t\tbody._doomnetLyricAnim = null;\n"
        "\t\t\t}\n"
        "\t\t\tbody.style.transform = \"\";\n"
        "\t\t\tif ((App.settings && App.settings.motionLevel) === \"off\") {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\trequestAnimationFrame(function () {\n"
        "\t\t\t\trequestAnimationFrame(function () {\n"
        "\t\t\t\t\tif (!box.isConnected) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar view = box.clientHeight;\n"
        "\t\t\t\t\tvar textH = body.scrollHeight;\n"
        "\t\t\t\t\tvar pad = Math.max(28, view * 0.22);\n"
        "\t\t\t\t\tvar travel = textH - (view - pad * 2);\n"
        "\t\t\t\t\tif (travel <= 2) {\n"
        "\t\t\t\t\t\tbody.style.transform = \"translateY(\" + Math.max(pad * 0.5, (view - textH) / 2) + \"px)\";\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar start = pad;\n"
        "\t\t\t\t\tvar end = pad - travel;\n"
        "\t\t\t\t\tvar holdStartMs = 4000;\n"
        "\t\t\t\t\tvar holdEndMs = 1500;\n"
        "\t\t\t\t\tvar travelOutMs = Math.max(1200, Math.round((travel / 29) * 1000));\n"
        "\t\t\t\t\tvar travelBackMs = Math.max(500, Math.round(travelOutMs / 2));\n"
        "\t\t\t\t\tvar total = holdStartMs + travelOutMs + holdEndMs + travelBackMs;\n"
        "\t\t\t\t\tvar startHold = holdStartMs / total;\n"
        "\t\t\t\t\tvar atEnd = (holdStartMs + travelOutMs) / total;\n"
        "\t\t\t\t\tvar endHold = (holdStartMs + travelOutMs + holdEndMs) / total;\n"
        "\t\t\t\t\tif (typeof body.animate !== \"function\") {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tbody._doomnetLyricAnim = body.animate([\n"
        "\t\t\t\t\t\t{ transform: \"translateY(\" + start + \"px)\", offset: 0 },\n"
        "\t\t\t\t\t\t{ transform: \"translateY(\" + start + \"px)\", offset: startHold },\n"
        "\t\t\t\t\t\t{ transform: \"translateY(\" + end + \"px)\", offset: atEnd },\n"
        "\t\t\t\t\t\t{ transform: \"translateY(\" + end + \"px)\", offset: endHold },\n"
        "\t\t\t\t\t\t{ transform: \"translateY(\" + start + \"px)\", offset: 1 },\n"
        "\t\t\t\t\t], { duration: total, iterations: Infinity, easing: \"linear\" });\n"
        "\t\t\t\t});\n"
        "\t\t\t});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetSyncLyrics: function () {\n"
        "\t\t\tvar id = (App.engine && App.engine.currentTrackId && App.engine.currentTrackId()) || (App.playerState && App.playerState.currentTrackId);\n"
        "\t\t\tif (id == null || id === \"\") {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tid = String(id);\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar body = host && host.querySelector(\".doomnet-lyrics-body\");\n"
        "\t\t\tif (!body) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar filled = body.childNodes.length > 0 || (body.textContent && body.textContent.length);\n"
        "\t\t\tif (UI._doomnetLyricFor === id && filled) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (UI._doomnetLyricFor === id && UI._doomnetLyricPayload) {\n"
        "\t\t\t\tUI.doomnetShowLyrics(UI._doomnetLyricPayload);\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (UI._doomnetLyricFor === id) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI._doomnetLyricFor = id;\n"
        "\t\t\tUI._doomnetLyricPayload = null;\n"
        "\t\t\tUI._doomnetLyricLines = null;\n"
        "\t\t\tUI._doomnetLyricSynced = false;\n"
        "\t\t\tcallServer(\"get_track_lyrics\", [id]).then(function (payload) {\n"
        "\t\t\t\tif (UI._doomnetLyricFor !== id) {\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tUI._doomnetLyricPayload = payload || { text: \"\" };\n"
        "\t\t\t\tUI.doomnetShowLyrics(UI._doomnetLyricPayload);\n"
        "\t\t\t}).catch(function () {});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEnsureMeterLoop: function () {\n"
        "\t\t\tif (UI._doomnetMeterFrame || !UI.doomnetMetersWanted()) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI._doomnetMeterFrame = requestAnimationFrame(UI.doomnetPaintMeters);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetFlanksShown: function () {\n"
        "\t\t\tif (UI._doomnetFlanksShown == null) {\n"
        "\t\t\t\tUI._doomnetFlanksShown = true;\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tUI._doomnetFlanksShown = window.localStorage.getItem(\"smp-flanks-shown\") !== \"0\";\n"
        "\t\t\t\t} catch (e) {}\n"
        "\t\t\t}\n"
        "\t\t\treturn UI._doomnetFlanksShown !== false;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPaintFlankEye: function (btn, shown) {\n"
        "\t\t\tif (!btn) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tbtn.innerHTML = shown\n"
        "\t\t\t\t? '<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" aria-hidden=\"true\"><path fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" d=\"M2.5 12S6 6.5 12 6.5 21.5 12 21.5 12 18 17.5 12 17.5 2.5 12 2.5 12z\"/><circle fill=\"currentColor\" cx=\"12\" cy=\"12\" r=\"2.2\"/></svg>'\n"
        "\t\t\t\t: '<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" aria-hidden=\"true\"><path fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" d=\"M3 14c2.6-4.2 5.6-6.2 9-6.2s6.4 2 9 6.2\"/><path fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" d=\"M7 16.2c1.5.9 3.1 1.4 5 1.4s3.5-.5 5-1.4\"/></svg>';\n"
        "\t\t\tbtn.setAttribute(\"aria-pressed\", shown ? \"true\" : \"false\");\n"
        "\t\t\tbtn.setAttribute(\"aria-label\", shown ? \"Hide equalizer and lyrics\" : \"Show equalizer and lyrics\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetApplyFlankVisibility: function (wrap) {\n"
        "\t\t\tif (!wrap) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar shown = UI.doomnetFlanksShown();\n"
        "\t\t\twrap.classList.toggle(\"doomnet-flanks-off\", !shown);\n"
        "\t\t\tUI.doomnetPaintFlankEye(wrap.querySelector(\".doomnet-flank-eye\"), shown);\n"
        "\t\t\tif (shown) {\n"
        "\t\t\t\tUI.doomnetEnsureMeterLoop();\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetToggleFlanks: function (wrap) {\n"
        "\t\t\tUI._doomnetFlanksShown = !UI.doomnetFlanksShown();\n"
        "\t\t\ttry {\n"
        "\t\t\t\twindow.localStorage.setItem(\"smp-flanks-shown\", UI._doomnetFlanksShown ? \"1\" : \"0\");\n"
        "\t\t\t} catch (e) {}\n"
        "\t\t\tUI.doomnetApplyFlankVisibility(wrap);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEnsureFlankEye: function (wrap) {\n"
        "\t\t\tif (!wrap) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (!wrap.querySelector(\".doomnet-flank-eye\")) {\n"
        "\t\t\t\tvar btn = el(\"button\", \"doomnet-flank-eye\", { type: \"button\" });\n"
        "\t\t\t\tbtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tUI.doomnetToggleFlanks(wrap);\n"
        "\t\t\t\t});\n"
        "\t\t\t\twrap.appendChild(btn);\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetApplyFlankVisibility(wrap);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetNpVertical: function () {\n"
        "\t\t\tif (UI._doomnetNpVertical == null) {\n"
        "\t\t\t\tUI._doomnetNpVertical = true;\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tUI._doomnetNpVertical = window.localStorage.getItem(\"smp-np-orient\") !== \"horizontal\";\n"
        "\t\t\t\t} catch (e) {}\n"
        "\t\t\t}\n"
        "\t\t\treturn UI._doomnetNpVertical !== false;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPaintNpOrient: function (btn, vertical) {\n"
        "\t\t\tif (!btn) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tbtn.innerHTML = vertical\n"
        "\t\t\t\t? '<svg viewBox=\"0 0 24 24\" width=\"20\" height=\"20\" aria-hidden=\"true\"><path fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\" d=\"M5 12h12M13 7l5 5-5 5\"/></svg>'\n"
        "\t\t\t\t: '<svg viewBox=\"0 0 24 24\" width=\"20\" height=\"20\" aria-hidden=\"true\"><path fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\" d=\"M12 5v12M7 13l5 5 5-5\"/></svg>';\n"
        "\t\t\tbtn.setAttribute(\"aria-label\", vertical ? \"Place the song beside the cover\" : \"Place the song under the cover\");\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetApplyNpOrient: function () {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tif (!host) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar vertical = UI.doomnetNpVertical();\n"
        "\t\t\thost.classList.toggle(\"doomnet-np-horizontal\", !vertical);\n"
        "\t\t\tUI.doomnetPaintNpOrient(host.querySelector(\".doomnet-np-orient\"), vertical);\n"
        "\t\t\tvar page = host.querySelector(\".smp-nptab\");\n"
        "\t\t\tif (page && page.querySelector(\".doomnet-flank-eq\")) {\n"
        "\t\t\t\twindow.requestAnimationFrame(function () {\n"
        "\t\t\t\t\tUI.doomnetLayoutFlanks(page);\n"
        "\t\t\t\t});\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetToggleNpOrient: function () {\n"
        "\t\t\tUI._doomnetNpVertical = !UI.doomnetNpVertical();\n"
        "\t\t\ttry {\n"
        "\t\t\t\twindow.localStorage.setItem(\"smp-np-orient\", UI._doomnetNpVertical ? \"vertical\" : \"horizontal\");\n"
        "\t\t\t} catch (e) {}\n"
        "\t\t\tUI.doomnetApplyNpOrient();\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEnsureNpOrient: function () {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\tvar view = host && host.querySelector(\".smp-view-library\");\n"
        "\t\t\tif (!view) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar btn = view.querySelector(\".doomnet-np-orient\");\n"
        "\t\t\tif (!btn) {\n"
        "\t\t\t\tbtn = el(\"button\", \"doomnet-np-orient\", { type: \"button\" });\n"
        "\t\t\t\tbtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tUI.doomnetToggleNpOrient();\n"
        "\t\t\t\t});\n"
        "\t\t\t\tview.appendChild(btn);\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetApplyNpOrient();\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMountNpTransport: function (wrap) {\n"
        "\t\t\tif (!wrap || !wrap.closest || !wrap.closest(\"#doomnet-steamtunes-host\")) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetEnsureNpOrient();\n"
        "\t\t\tvar modes = wrap.querySelector(\".smp-nptab-modes\");\n"
        "\t\t\tvar info = wrap.querySelector(\".smp-nptab-info\");\n"
        "\t\t\tif (!modes || !info) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (!modes.querySelector(\".doomnet-np-prev\")) {\n"
        "\t\t\t\tvar prev = iconButton(\"prev\", ICONS.prev, \"Previous\");\n"
        "\t\t\t\tvar play = iconButton(\"toggle\", ICONS.play, \"Play\");\n"
        "\t\t\t\tvar next = iconButton(\"next\", ICONS.next, \"Next\");\n"
        "\t\t\t\tprev.classList.add(\"doomnet-np-prev\");\n"
        "\t\t\t\tplay.classList.add(\"doomnet-np-play\");\n"
        "\t\t\t\tnext.classList.add(\"doomnet-np-next\");\n"
        "\t\t\t\tprev.addEventListener(\"click\", function () {\n"
        "\t\t\t\t\tEngine.applyCommand({ action: \"prev\" });\n"
        "\t\t\t\t});\n"
        "\t\t\t\tplay.addEventListener(\"click\", function () {\n"
        "\t\t\t\t\tEngine.applyCommand(Engine.playPauseCommand());\n"
        "\t\t\t\t});\n"
        "\t\t\t\tnext.addEventListener(\"click\", function () {\n"
        "\t\t\t\t\tEngine.applyCommand({ action: \"next\" });\n"
        "\t\t\t\t});\n"
        "\t\t\t\tvar cluster = el(\"div\", \"doomnet-np-cluster\");\n"
        "\t\t\t\tcluster.appendChild(prev);\n"
        "\t\t\t\tcluster.appendChild(play);\n"
        "\t\t\t\tcluster.appendChild(next);\n"
        "\t\t\t\tvar repeat = modes.querySelector(\".smp-repeat-btn\");\n"
        "\t\t\t\tif (repeat) {\n"
        "\t\t\t\t\tmodes.insertBefore(cluster, repeat);\n"
        "\t\t\t\t} else {\n"
        "\t\t\t\t\tmodes.appendChild(cluster);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tif (!info.querySelector(\".doomnet-np-seekline\")) {\n"
        "\t\t\t\tvar line = el(\"div\", \"doomnet-np-seekline\");\n"
        "\t\t\t\tvar seek = el(\"input\", \"smp-seek doomnet-np-seek\", {\n"
        "\t\t\t\t\ttype: \"range\",\n"
        "\t\t\t\t\tmin: \"0\",\n"
        "\t\t\t\t\tmax: \"1\",\n"
        "\t\t\t\t\tstep: \"0.01\",\n"
        "\t\t\t\t});\n"
        "\t\t\t\tseek.addEventListener(\"pointerdown\", function () {\n"
        "\t\t\t\t\tUI.scrubbing = true;\n"
        "\t\t\t\t});\n"
        "\t\t\t\tseek.addEventListener(\"pointerup\", function () {\n"
        "\t\t\t\t\tUI.scrubbing = false;\n"
        "\t\t\t\t});\n"
        "\t\t\t\tseek.addEventListener(\"input\", function () {\n"
        "\t\t\t\t\tvar t = parseFloat(seek.value) || 0;\n"
        "\t\t\t\t\tvar timeEl = line.querySelector(\".smp-time\");\n"
        "\t\t\t\t\tif (timeEl) {\n"
        "\t\t\t\t\t\ttimeEl.textContent = fmtTime(t) + \" / \" + fmtTime(Number(seek.max) || 0);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t});\n"
        "\t\t\t\tseek.addEventListener(\"change\", function () {\n"
        "\t\t\t\t\tUI.scrubbing = false;\n"
        "\t\t\t\t\tEngine.applyCommand({ action: \"seek\", value: parseFloat(seek.value) || 0 });\n"
        "\t\t\t\t});\n"
        "\t\t\t\tline.appendChild(el(\"div\", \"smp-time\", { text: \"0:00 / 0:00\" }));\n"
        "\t\t\t\tline.appendChild(seek);\n"
        "\t\t\t\tline.appendChild(UI.buildVolumeSlider());\n"
        "\t\t\t\tinfo.appendChild(line);\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetSyncNpTransport(wrap);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetSyncNpTransport: function (wrap) {\n"
        "\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\twrap = wrap && wrap.querySelector ? wrap : null;\n"
        "\t\t\tif (!wrap || !wrap.classList || !wrap.classList.contains(\"smp-nptab\")) {\n"
        "\t\t\t\twrap = host && host.querySelector(\".smp-nptab\");\n"
        "\t\t\t}\n"
        "\t\t\tif (!wrap) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar play = wrap.querySelector(\".doomnet-np-play\");\n"
        "\t\t\tif (play) {\n"
        "\t\t\t\tvar on = Engine.isUiPlaying();\n"
        "\t\t\t\tvar want = on ? \"pause\" : \"play\";\n"
        "\t\t\t\tif (play.getAttribute(\"data-doomnet-state\") !== want) {\n"
        "\t\t\t\t\tplay.setAttribute(\"data-doomnet-state\", want);\n"
        "\t\t\t\t\tplay.innerHTML = on ? ICONS.pause : ICONS.play;\n"
        "\t\t\t\t\tplay.title = on ? \"Pause\" : \"Play\";\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tif (UI.scrubbing) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar seek = wrap.querySelector(\".doomnet-np-seek\");\n"
        "\t\t\tvar time = wrap.querySelector(\".doomnet-np-seekline .smp-time\");\n"
        "\t\t\tvar progress = UI.playbackProgress();\n"
        "\t\t\tvar elapsed = progress.elapsed;\n"
        "\t\t\tvar duration = progress.duration;\n"
        "\t\t\tif (seek) {\n"
        "\t\t\t\tvar max = duration > 0 ? duration : 1;\n"
        "\t\t\t\tif (Number(seek.max) !== max) {\n"
        "\t\t\t\t\tseek.max = String(max);\n"
        "\t\t\t\t\tseek.setAttribute(\"max\", String(max));\n"
        "\t\t\t\t}\n"
        "\t\t\t\tseek.value = duration > 0 ? String(Math.max(0, Math.min(elapsed, max))) : \"0\";\n"
        "\t\t\t}\n"
        "\t\t\tif (time) {\n"
        "\t\t\t\ttime.textContent = UI.formatProgress(progress);\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMountNowPlayingFlanks: function (wrap) {\n"
        "\t\t\tif (!UI._doomnetFlankWatch) {\n"
        "\t\t\t\tUI._doomnetFlankWatch = true;\n"
        "\t\t\t\twindow.addEventListener(\"pointerup\", function () {\n"
        "\t\t\t\t\tUI._doomnetEqDragging = false;\n"
        "\t\t\t\t});\n"
        "\t\t\t\twindow.addEventListener(\"pointerdown\", function (event) {\n"
        "\t\t\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\t\t\tvar wrap = host && host.querySelector(\".doomnet-eq-preset-wrap\");\n"
        "\t\t\t\t\tif (!wrap || wrap.contains(event.target)) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tUI.doomnetCloseEqMenu();\n"
        "\t\t\t\t});\n"
        "\t\t\t\twindow.addEventListener(\"resize\", function () {\n"
        "\t\t\t\t\tvar host = document.getElementById(\"doomnet-steamtunes-host\");\n"
        "\t\t\t\t\tvar page = host && host.querySelector(\".smp-nptab\");\n"
        "\t\t\t\t\tif (page) {\n"
        "\t\t\t\t\t\tUI.doomnetMountNowPlayingFlanks(page);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t});\n"
        "\t\t\t}\n"
        "\t\t\tif (!wrap || !wrap.closest || !wrap.closest(\"#doomnet-steamtunes-host\")) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetEnsureFlankEye(wrap);\n"
        "\t\t\tif (wrap.querySelector(\".doomnet-flank-eq\")) {\n"
        "\t\t\t\tUI.doomnetRefreshEqFlank(wrap);\n"
        "\t\t\t\tUI.doomnetRefreshMixBars();\n"
        "\t\t\t\tUI.doomnetLayoutFlanks(wrap);\n"
        "\t\t\t\tUI.doomnetSyncLyrics();\n"
        "\t\t\t\tUI.doomnetEnsureMeterLoop();\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar curve = UI.doomnetEqCurve();\n"
        "\t\t\tvar eq = el(\"div\", \"doomnet-flank doomnet-flank-eq\");\n"
        "\t\t\tvar presetWrap = el(\"div\", \"doomnet-eq-preset-wrap\");\n"
        "\t\t\tvar presetBtn = el(\"button\", \"doomnet-eq-preset\", { type: \"button\" });\n"
        "\t\t\tpresetBtn.setAttribute(\"aria-haspopup\", \"listbox\");\n"
        "\t\t\tpresetBtn.setAttribute(\"aria-expanded\", \"false\");\n"
        "\t\t\tpresetBtn.appendChild(el(\"span\", \"doomnet-eq-preset-name\", { text: curve.name }));\n"
        "\t\t\tvar caret = el(\"span\", \"doomnet-eq-caret\");\n"
        "\t\t\tcaret.setAttribute(\"aria-hidden\", \"true\");\n"
        "\t\t\tpresetBtn.appendChild(caret);\n"
        "\t\t\tpresetBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\tUI.doomnetToggleEqMenu(eq);\n"
        "\t\t\t});\n"
        "\t\t\tvar menu = el(\"div\", \"doomnet-eq-menu\");\n"
        "\t\t\tmenu.hidden = true;\n"
        "\t\t\tmenu.setAttribute(\"role\", \"listbox\");\n"
        "\t\t\tvar p;\n"
        "\t\t\tfor (p = 0; p < curve.options.length; p++) {\n"
        "\t\t\t\t(function (option) {\n"
        "\t\t\t\t\tvar item = el(\"button\", option.value === curve.preset ? \"on\" : \"\", { type: \"button\", text: option.name });\n"
        "\t\t\t\t\titem.setAttribute(\"data-preset\", option.value);\n"
        "\t\t\t\t\titem.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tUI.doomnetChooseEqPreset(option.value);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tmenu.appendChild(item);\n"
        "\t\t\t\t})(curve.options[p]);\n"
        "\t\t\t}\n"
        "\t\t\tpresetWrap.appendChild(presetBtn);\n"
        "\t\t\tpresetWrap.appendChild(menu);\n"
        "\t\t\teq.appendChild(presetWrap);\n"
        "\t\t\tvar bank = el(\"div\", \"doomnet-eq-bank\");\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < curve.keys.length; i++) {\n"
        "\t\t\t\t(function (index) {\n"
        "\t\t\t\t\tvar col = el(\"div\", \"doomnet-eq-col\");\n"
        "\t\t\t\t\tvar gain = curve.gains[index] || 0;\n"
        "\t\t\t\t\tvar readout = el(\"span\", \"doomnet-eq-db\", { text: (gain > 0 ? \"+\" : \"\") + gain });\n"
        "\t\t\t\t\tif (gain !== 0) {\n"
        "\t\t\t\t\t\treadout.style.color = \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar slider = el(\"input\", \"doomnet-eq-slider\", {\n"
        "\t\t\t\t\t\ttype: \"range\",\n"
        "\t\t\t\t\t\tmin: \"-12\",\n"
        "\t\t\t\t\t\tmax: \"12\",\n"
        "\t\t\t\t\t\tstep: \"1\",\n"
        "\t\t\t\t\t\tvalue: String(gain),\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.setAttribute(\"aria-label\", curve.labels[index] + \" Hz\");\n"
        "\t\t\t\t\tslider.addEventListener(\"pointerdown\", function () {\n"
        "\t\t\t\t\t\tUI._doomnetEqDragging = true;\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.addEventListener(\"pointerup\", function () {\n"
        "\t\t\t\t\t\tUI._doomnetEqDragging = false;\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.addEventListener(\"input\", function () {\n"
        "\t\t\t\t\t\tvar value = parseFloat(slider.value) || 0;\n"
        "\t\t\t\t\t\treadout.textContent = (value > 0 ? \"+\" : \"\") + value;\n"
        "\t\t\t\t\t\treadout.style.color = value === 0 ? \"\" : \"var(--smp-accent, #66c0f4)\";\n"
        "\t\t\t\t\t\tUI.doomnetPaintEqSlider(slider);\n"
        "\t\t\t\t\t\tUI.doomnetApplyEqBand(index, value, false);\n"
        "\t\t\t\t\t\tvar title = eq.querySelector(\".doomnet-eq-preset-name\");\n"
        "\t\t\t\t\t\tif (title) {\n"
        "\t\t\t\t\t\t\ttitle.textContent = \"Custom\";\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tslider.addEventListener(\"change\", function () {\n"
        "\t\t\t\t\t\tUI._doomnetEqDragging = false;\n"
        "\t\t\t\t\t\tUI.doomnetApplyEqBand(index, parseFloat(slider.value) || 0, true);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tUI.doomnetPaintEqSlider(slider);\n"
        "\t\t\t\t\tvar slot = el(\"div\", \"doomnet-eq-slot\");\n"
        "\t\t\t\t\tslot.appendChild(slider);\n"
        "\t\t\t\t\tcol.appendChild(readout);\n"
        "\t\t\t\t\tcol.appendChild(slot);\n"
        "\t\t\t\t\tcol.appendChild(el(\"span\", \"doomnet-eq-hz\", { text: curve.labels[index] }));\n"
        "\t\t\t\t\tbank.appendChild(col);\n"
        "\t\t\t\t})(i);\n"
        "\t\t\t}\n"
        "\t\t\teq.appendChild(bank);\n"
        "\t\t\tvar under = el(\"div\", \"doomnet-eq-meter\");\n"
        "\t\t\tvar canvas = el(\"canvas\", \"doomnet-spectrum\");\n"
        "\t\t\tcanvas.width = 480;\n"
        "\t\t\tcanvas.height = 96;\n"
        "\t\t\tunder.appendChild(canvas);\n"
        "\t\t\tvar loud = el(\"div\", \"doomnet-loudness\");\n"
        "\t\t\tvar track = el(\"div\", \"doomnet-loudness-track\");\n"
        "\t\t\ttrack.appendChild(el(\"div\", \"doomnet-loudness-fill\"));\n"
        "\t\t\tloud.appendChild(track);\n"
        "\t\t\tloud.appendChild(el(\"span\", \"doomnet-loudness-db\", { text: \"-inf dB\" }));\n"
        "\t\t\tunder.appendChild(loud);\n"
        "\t\t\tUI.doomnetMountMixBars(under);\n"
        "\t\t\teq.appendChild(under);\n"
        "\t\t\tvar meter = el(\"div\", \"doomnet-flank doomnet-flank-meter doomnet-flank-lyrics\");\n"
        "\t\t\tmeter.appendChild(el(\"div\", \"doomnet-lyrics-label\", { text: \"Lyrics\" }));\n"
        "\t\t\tvar lyricBox = el(\"div\", \"doomnet-lyrics\");\n"
        "\t\t\tlyricBox.appendChild(el(\"div\", \"doomnet-lyrics-body\"));\n"
        "\t\t\tmeter.appendChild(lyricBox);\n"
        "\t\t\tvar row = el(\"div\", \"doomnet-flank-row\");\n"
        "\t\t\trow.appendChild(eq);\n"
        "\t\t\trow.appendChild(meter);\n"
        "\t\t\tvar hero = wrap.querySelector(\".smp-nptab-hero\");\n"
        "\t\t\tif (hero && hero.nextSibling) {\n"
        "\t\t\t\twrap.insertBefore(row, hero.nextSibling);\n"
        "\t\t\t} else {\n"
        "\t\t\t\twrap.appendChild(row);\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetLayoutFlanks(wrap);\n"
        "\t\t\tUI.doomnetSyncLyrics();\n"
        "\t\t\tUI.doomnetEnsureMeterLoop();\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetArtCols: 14,\n"
        "\n"
        "\t\tdoomnetMountArtSize: function (subtabs) {\n"
        "\t\t\tif (!subtabs) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (!UI.doomnetArtColsLoaded) {\n"
        "\t\t\t\tUI.doomnetArtColsLoaded = true;\n"
        "\t\t\t\ttry {\n"
        "\t\t\t\t\tvar saved = Number(window.localStorage.getItem(\"smp-art-cols\"));\n"
        "\t\t\t\t\tif (saved === 14 || saved === 12 || saved === 10 || saved === 8 || saved === 6) {\n"
        "\t\t\t\t\t\tUI.doomnetArtCols = saved;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t} catch (e) {}\n"
        "\t\t\t}\n"
        "\t\t\tvar box = el(\"div\", \"smp-art-size\");\n"
        "\t\t\tbox.title = \"Artwork size\";\n"
        "\t\t\t[\n"
        "\t\t\t\t{ count: 14, size: \"sm\", title: \"Small artwork\" },\n"
        "\t\t\t\t{ count: 12, size: \"smd\", title: \"Medium-small artwork\" },\n"
        "\t\t\t\t{ count: 10, size: \"md\", title: \"Medium artwork\" },\n"
        "\t\t\t\t{ count: 8, size: \"mdl\", title: \"Medium-large artwork\" },\n"
        "\t\t\t\t{ count: 6, size: \"lg\", title: \"Large artwork\" }\n"
        "\t\t\t].forEach(function (choice) {\n"
        "\t\t\t\tvar count = choice.count;\n"
        "\t\t\t\tvar btn = el(\"button\", \"smp-art-size-btn\" + (UI.doomnetArtCols === count ? \" on\" : \"\"), {\n"
        "\t\t\t\t\ttype: \"button\",\n"
        "\t\t\t\t\ttitle: choice.title,\n"
        "\t\t\t\t});\n"
        "\t\t\t\tbtn.appendChild(el(\"span\", \"smp-art-size-mark smp-art-size-\" + choice.size));\n"
        "\t\t\t\tbtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tif (UI.doomnetArtCols === count) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tUI.doomnetArtCols = count;\n"
        "\t\t\t\t\ttry {\n"
        "\t\t\t\t\t\twindow.localStorage.setItem(\"smp-art-cols\", String(count));\n"
        "\t\t\t\t\t} catch (err) {}\n"
        "\t\t\t\t\tvar pages = (UI.browseCache && UI.browseCache.pages) || {};\n"
        "\t\t\t\t\tObject.keys(pages).forEach(function (key) {\n"
        "\t\t\t\t\t\tif (pages[key]) {\n"
        "\t\t\t\t\t\t\tpages[key].metrics = null;\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tUI.renderLibrary();\n"
        "\t\t\t\t});\n"
        "\t\t\t\tbox.appendChild(btn);\n"
        "\t\t\t});\n"
        "\t\t\tsubtabs.appendChild(box);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetPageWarmDisabled: true,\n"
        "\t\tdoomnetWarmThumbs: function () {\n"
        "\t\t\treturn;\n"
        "\t\t\tif (!window.__doomnetTabHost || !cards || !cards.length) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar seen = UI.doomnetWarmSeen || (UI.doomnetWarmSeen = {});\n"
        "\t\t\tvar queue = UI.doomnetWarmQueue || (UI.doomnetWarmQueue = []);\n"
        "\t\t\tvar ahead = [];\n"
        "\t\t\tvar from = Math.max(0, focus || 0);\n"
        "\t\t\tvar to = Math.min(cards.length, from + 80);\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = from; i < to; i++) {\n"
        "\t\t\t\tahead.push(cards[i]);\n"
        "\t\t\t}\n"
        "\t\t\tif (!cards.__doomnetQueued) {\n"
        "\t\t\t\tcards.__doomnetQueued = true;\n"
        "\t\t\t\tfor (i = 0; i < cards.length; i++) {\n"
        "\t\t\t\t\tahead.push(cards[i]);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetWarmQueue = ahead.concat(queue).slice(0, 12000);\n"
        "\t\t\tif (UI.doomnetWarmTimer) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar pump = function () {\n"
        "\t\t\t\tUI.doomnetWarmTimer = null;\n"
        "\t\t\t\tvar started = 0;\n"
        "\t\t\t\twhile (started < 8 && UI.doomnetWarmQueue.length) {\n"
        "\t\t\t\t\tvar card = UI.doomnetWarmQueue.shift();\n"
        "\t\t\t\t\tvar track = card && card.tracks && card.tracks[0];\n"
        "\t\t\t\t\tvar album = \"\";\n"
        "\t\t\t\t\tif (card && card.kind === \"album\" && card.artist) {\n"
        "\t\t\t\t\t\talbum = UI.albumArtSafe({ artist: card.artist, albumArtist: card.artist, album: card.album || card.title });\n"
        "\t\t\t\t\t} else if (track) {\n"
        "\t\t\t\t\t\talbum = UI.albumArtSafe(track);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tif (!album || seen[album]) {\n"
        "\t\t\t\t\t\tcontinue;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tseen[album] = true;\n"
        "\t\t\t\t\tvar thumb = UI.artUrlForAlbum(album, \"jpg\").replace(\"/steam-music-player/art/albums/\", \"/steam-music-player/art/thumbs/\");\n"
        "\t\t\t\t\tfetch(thumb, { priority: \"low\", credentials: \"omit\", cache: \"force-cache\" }).catch(function () {});\n"
        "\t\t\t\t\tstarted++;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tif (UI.doomnetWarmQueue.length) {\n"
        "\t\t\t\t\tUI.doomnetWarmTimer = setTimeout(pump, 40);\n"
        "\t\t\t\t}\n"
        "\t\t\t};\n"
        "\t\t\tpump();\n"
        "\t\t},";
    const size_t paint_at = ui.find(old_paint);
    if (paint_at != std::string::npos) {
        ui.replace(paint_at, old_paint.size(), new_paint);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the cover paint tail, so the grids keep loading full-size covers instead of thumbnails.");
    }
    const std::string old_probe_album =
        "\t\t\tvar bust = UI.artProbeGen ? (\"?r=\" + UI.artProbeGen) : \"\";\n"
        "\t\t\treturn UI.probeArtUrl(UI.artUrlForAlbum(albumSafe, \"jpg\") + bust).then(function (url) {\n"
        "\t\t\t\treturn url || UI.probeArtUrl(UI.artUrlForAlbum(albumSafe, \"png\") + bust);\n"
        "\t\t\t}).then(function (url) {\n";
    const std::string new_probe_album =
        "\t\t\tvar bust = UI.artProbeGen ? (\"?r=\" + UI.artProbeGen) : \"\";\n"
        "\t\t\tvar thumbFirst = window.__doomnetTabHost ? UI.probeArtUrl(UI.artUrlForAlbum(albumSafe, \"jpg\").replace(\"/steam-music-player/art/albums/\", \"/steam-music-player/art/thumbs/\")) : Promise.resolve(null);\n"
        "\t\t\treturn thumbFirst.then(function (thumb) {\n"
        "\t\t\t\tif (thumb) {\n"
        "\t\t\t\t\tArtStore.put(albumSafe, UI.artUrlForAlbum(albumSafe, \"jpg\"));\n"
        "\t\t\t\t\treturn ArtStore.get(albumSafe);\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn UI.probeArtUrl(UI.artUrlForAlbum(albumSafe, \"jpg\") + bust).then(function (url) {\n"
        "\t\t\t\t\treturn url || UI.probeArtUrl(UI.artUrlForAlbum(albumSafe, \"png\") + bust);\n"
        "\t\t\t\t});\n"
        "\t\t\t}).then(function (url) {\n";
    const size_t probe_album_at = ui.find(old_probe_album);
    if (probe_album_at != std::string::npos) {
        ui.replace(probe_album_at, old_probe_album.size(), new_probe_album);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the album cover probe, so a fast scroll still waits on full-size covers.");
    }
    const std::string old_watch = "\t\t\tUI.watchArtElement(artEl);\n";
    const std::string new_watch =
        "\t\t\tif (window.__doomnetTabHost) {\n"
        "\t\t\t\tUI.applyArtToElement(artEl, tracks, companionEl);\n"
        "\t\t\t} else {\n"
        "\t\t\tUI.watchArtElement(artEl);\n"
        "\t\t\t}\n";
    const size_t watch_at = ui.find(old_watch);
    if (watch_at != std::string::npos) {
        ui.replace(watch_at, old_watch.size(), new_watch);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not wait on the art observer, so grid covers were not switched to load as soon as a card exists.");
    }
    const std::string old_cards = "\t\t\tUI.browseCache.cards[mode] = cards;\n";
    const std::string new_cards =
        "\t\t\tUI.browseCache.cards[mode] = cards;\n"
        "\t\t\tif (UI.doomnetWarmThumbs) {\n"
        "\t\t\t\tUI.doomnetWarmThumbs(cards, 0);\n"
        "\t\t\t}\n";
    const size_t cards_at = ui.find(old_cards);
    if (cards_at != std::string::npos) {
        ui.replace(cards_at, old_cards.size(), new_cards);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not cache the library cards, so covers are not preloaded.");
    }
    const std::string old_sync_art = "\t\t\tUI.syncVisibleArt(page.grid);\n";
    const std::string new_sync_art =
        "\t\t\tUI.syncVisibleArt(page.grid);\n"
        "\t\t\tif (UI.doomnetWarmThumbs) {\n"
        "\t\t\t\tUI.doomnetWarmThumbs(cards, start);\n"
        "\t\t\t}\n";
    const size_t sync_art_at = ui.find(old_sync_art);
    if (sync_art_at != std::string::npos) {
        ui.replace(sync_art_at, old_sync_art.size(), new_sync_art);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not refresh visible covers, so a fast scroll cannot preload the rows ahead.");
    }
    const std::string old_launcher =
        "\t\t\tvar launcher = el(\"button\", \"smp-launcher\", { title: \"Steam Music Player\", type: \"button\" });\n"
        "\t\t\tlauncher.innerHTML = \"&#9835;\";\n"
        "\t\t\tlauncher.addEventListener(\"click\", function () {\n"
        "\t\t\t\tUI.setPanelOpen(!UI.panelOpen);\n"
        "\t\t\t});\n"
        "\t\t\tdocument.body.appendChild(launcher);\n"
        "\t\t\tUI.launcher = launcher;\n";
    const std::string new_launcher =
        "\t\t\tvar tabHost = window.__doomnetTabHost || null;\n"
        "\t\t\tif (!tabHost) {\n"
        "\t\t\tvar launcher = el(\"button\", \"smp-launcher\", { title: \"Steam Music Player\", type: \"button\" });\n"
        "\t\t\tlauncher.innerHTML = \"&#9835;\";\n"
        "\t\t\tlauncher.addEventListener(\"click\", function () {\n"
        "\t\t\t\tUI.setPanelOpen(!UI.panelOpen);\n"
        "\t\t\t});\n"
        "\t\t\tdocument.body.appendChild(launcher);\n"
        "\t\t\tUI.launcher = launcher;\n"
        "\t\t\t}\n";
    const size_t launcher_at = ui.find(old_launcher);
    if (launcher_at != std::string::npos) {
        ui.replace(launcher_at, old_launcher.size(), new_launcher);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the floating launcher, so the SteamTunes tab cannot replace it.");
    }
    const std::string old_root = "\t\t\tvar root = el(\"div\", \"smp-panel smp-hidden\");";
    const std::string new_root = "\t\t\tvar root = el(\"div\", tabHost ? \"smp-panel smp-tab-page\" : \"smp-panel smp-hidden\");";
    const size_t root_at = ui.find(old_root);
    if (root_at != std::string::npos) {
        ui.replace(root_at, old_root.size(), new_root);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the main panel, so the SteamTunes tab has nothing to fill.");
    }
    const std::string old_append = "\t\t\tdocument.body.appendChild(root);\n\t\t\tUI.root = root;";
    const std::string new_append = "\t\t\t(tabHost || document.body).appendChild(root);\n\t\t\tUI.root = root;";
    const size_t append_at = ui.find(old_append);
    if (append_at != std::string::npos) {
        ui.replace(append_at, old_append.size(), new_append);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not append the main panel, so the SteamTunes tab cannot hold it.");
    }
    const std::string old_save_cover =
        "\t\t\t\t\t\tif (!file.path) {\n"
        "\t\t\t\t\t\t\tfail(new Error(\"Steam didn't provide that file's location.\"));\n"
        "\t\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\tcallServer(\"set_display_art\", [id, file.path]).then(function (result) {\n"
        "\t\t\t\t\t\t\tif (!result || result.ok === false) {\n"
        "\t\t\t\t\t\t\t\tthrow new Error((result && result.error) || \"Couldn't use that image from its folder.\");\n"
        "\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\treturn result;\n"
        "\t\t\t\t\t\t}).then(finish).catch(fail);\n";
    const std::string new_save_cover =
        "\t\t\t\t\t\tvar uploadBytes = function (buffer) {\n"
        "\t\t\t\t\t\t\tvar bytes = new Uint8Array(buffer);\n"
        "\t\t\t\t\t\t\tvar binary = \"\";\n"
        "\t\t\t\t\t\t\tvar step = 8192;\n"
        "\t\t\t\t\t\t\tvar i;\n"
        "\t\t\t\t\t\t\tfor (i = 0; i < bytes.length; i += step) {\n"
        "\t\t\t\t\t\t\t\tvar slice = bytes.subarray(i, Math.min(i + step, bytes.length));\n"
        "\t\t\t\t\t\t\t\tvar piece = \"\";\n"
        "\t\t\t\t\t\t\t\tvar j;\n"
        "\t\t\t\t\t\t\t\tfor (j = 0; j < slice.length; j++) {\n"
        "\t\t\t\t\t\t\t\t\tpiece += String.fromCharCode(slice[j]);\n"
        "\t\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\t\tbinary += piece;\n"
        "\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\tvar b64 = btoa(binary);\n"
        "\t\t\t\t\t\t\tvar partSize = 48 * 1024;\n"
        "\t\t\t\t\t\t\treturn callServer(\"begin_display_art\", [id]).then(function (started) {\n"
        "\t\t\t\t\t\t\t\tif (!started || started.ok === false) {\n"
        "\t\t\t\t\t\t\t\t\tthrow new Error((started && started.error) || \"Couldn't start saving that cover.\");\n"
        "\t\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\t\tvar chain = Promise.resolve();\n"
        "\t\t\t\t\t\t\t\tvar offset;\n"
        "\t\t\t\t\t\t\t\tfor (offset = 0; offset < b64.length; offset += partSize) {\n"
        "\t\t\t\t\t\t\t\t\t(function (part) {\n"
        "\t\t\t\t\t\t\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\t\t\t\t\t\t\treturn callServer(\"append_display_art\", [id, part]);\n"
        "\t\t\t\t\t\t\t\t\t\t});\n"
        "\t\t\t\t\t\t\t\t\t})(b64.substring(offset, offset + partSize));\n"
        "\t\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\t\treturn chain.then(function () {\n"
        "\t\t\t\t\t\t\t\t\treturn callServer(\"finish_display_art\", [id]);\n"
        "\t\t\t\t\t\t\t\t});\n"
        "\t\t\t\t\t\t\t});\n"
        "\t\t\t\t\t\t};\n"
        "\t\t\t\t\t\tvar reader = new FileReader();\n"
        "\t\t\t\t\t\treader.onload = function () {\n"
        "\t\t\t\t\t\t\tuploadBytes(reader.result).then(function (result) {\n"
        "\t\t\t\t\t\t\t\tif (!result || result.ok === false) {\n"
        "\t\t\t\t\t\t\t\t\tthrow new Error((result && result.error) || \"Couldn't save that cover.\");\n"
        "\t\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\t\treturn result;\n"
        "\t\t\t\t\t\t\t}).then(finish).catch(fail);\n"
        "\t\t\t\t\t\t};\n"
        "\t\t\t\t\t\treader.onerror = function () {\n"
        "\t\t\t\t\t\t\tfail(new Error(\"Couldn't read that image.\"));\n"
        "\t\t\t\t\t\t};\n"
        "\t\t\t\t\t\treader.readAsArrayBuffer(file);\n";
    const size_t save_cover_at = ui.find(old_save_cover);
    if (save_cover_at != std::string::npos) {
        ui.replace(save_cover_at, old_save_cover.size(), new_save_cover);
    } else {
        log_event(L"warn", L"inject", L"The player UI still asks Steam for the image path, so Set Cover puts the old picture back.");
    }
    const std::string old_paint_art =
        "\t\t\tif (!artEl || !dataUrl) {\n"
        "\t\t\t\tif (onFail) {\n"
        "\t\t\t\t\tonFail();\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar img = artEl.querySelector(\".smp-album-art-img\");\n";
    const std::string new_paint_art =
        "\t\t\tif (!artEl || !dataUrl) {\n"
        "\t\t\t\tif (onFail) {\n"
        "\t\t\t\t\tonFail();\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar pinnedAlbum = artEl.getAttribute && artEl.getAttribute(\"data-art-album\");\n"
        "\t\t\tvar pinnedCover = pinnedAlbum && App.coverPreview && App.coverPreview[pinnedAlbum];\n"
        "\t\t\tif (pinnedCover) {\n"
        "\t\t\t\tdataUrl = pinnedCover;\n"
        "\t\t\t}\n"
        "\t\t\tvar img = artEl.querySelector(\".smp-album-art-img\");\n";
    const size_t paint_art_at = ui.find(old_paint_art);
    if (paint_art_at != std::string::npos) {
        ui.replace(paint_art_at, old_paint_art.size(), new_paint_art);
    } else {
        log_event(L"warn", L"inject", L"The player UI can still paint the old album picture over a cover the user just chose.");
    }
    const std::string old_drills = "var POINTER_DRILLS = { artistAlbums: true, genreAlbums: true, genreTracks: true, albumTracks: true };";
    const std::string new_drills = "var POINTER_DRILLS = { artistAlbums: true, genreAlbums: true, genreTracks: true, albumTracks: true, playlistTracks: true };";
    const size_t drills_at = ui.find(old_drills);
    if (drills_at != std::string::npos) {
        ui.replace(drills_at, old_drills.size(), new_drills);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not keep playlist pages in the shared pointer, so opening a playlist would be forgotten.");
    }
    const std::string old_drill_else =
        "\t\t} else {\n"
        "\t\t\tdrill.artist = value.artist || \"\";\n"
        "\t\t\tdrill.album = value.album || \"\";\n"
        "\t\t}\n";
    const std::string new_drill_else =
        "\t\t} else if (value.mode === \"playlistTracks\") {\n"
        "\t\t\tdrill.playlistId = value.playlistId || \"\";\n"
        "\t\t\tdrill.playlistName = value.playlistName || \"\";\n"
        "\t\t} else {\n"
        "\t\t\tdrill.artist = value.artist || \"\";\n"
        "\t\t\tdrill.album = value.album || \"\";\n"
        "\t\t}\n";
    const size_t drill_else_at = ui.find(old_drill_else);
    if (drill_else_at != std::string::npos) {
        ui.replace(drill_else_at, old_drill_else.size(), new_drill_else);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not keep a playlist id on the shared page, so the song list would not reopen.");
    }
    const std::string old_drill_sig = "drill ? [drill.mode, drill.artist || \"\", drill.album || \"\", drill.genre || \"\"].join(\"/\") : \"\",";
    const std::string new_drill_sig = "drill ? [drill.mode, drill.artist || \"\", drill.album || \"\", drill.genre || \"\", drill.playlistId || \"\"].join(\"/\") : \"\",";
    size_t drill_sig_at = 0;
    int drill_sig_count = 0;
    while ((drill_sig_at = ui.find(old_drill_sig, drill_sig_at)) != std::string::npos) {
        ui.replace(drill_sig_at, old_drill_sig.size(), new_drill_sig);
        drill_sig_at += new_drill_sig.size();
        ++drill_sig_count;
    }
    if (drill_sig_count == 0) {
        log_event(L"warn", L"inject", L"The player UI did not include the playlist in the page signature, so two playlists would look like the same page.");
    }
    const std::string old_load = "\t\t\tUI.loadAll();\n";
    const std::string new_load =
        "\t\t\twindow.__doomnetShowPlayer = function () { UI.setPanelOpen(true); };\n"
        "\t\t\twindow.__doomnetHidePlayer = function () { UI.setPanelOpen(false); };\n"
        "\t\t\tif (window.__doomnetTabHost) {\n"
        "\t\t\t\tUI.setPanelOpen(true);\n"
        "\t\t\t}\n"
        "\t\t\tUI.loadAll();\n";
    const size_t load_at = ui.find(old_load);
    if (load_at != std::string::npos) {
        ui.replace(load_at, old_load.size(), new_load);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not load the library at the end of the main panel, so the SteamTunes tab stays empty.");
    }
    const std::string old_hydrate_genre =
        "\t\t\tif (next.mode === \"genreTracks\") {\n"
        "\t\t\t\tnext.tracks = UI.genreTracksFor(next.genre);\n"
        "\t\t\t}\n";
    const std::string new_hydrate_genre =
        "\t\t\tif (next.mode === \"genreTracks\") {\n"
        "\t\t\t\tnext.tracks = UI.genreTracksFor(next.genre);\n"
        "\t\t\t}\n"
        "\t\t\tif (next.mode === \"playlistTracks\") {\n"
        "\t\t\t\tvar foundPlaylist = null;\n"
        "\t\t\t\tasArray(App.playlists).forEach(function (playlist) {\n"
        "\t\t\t\t\tif (playlist && String(playlist.id) === String(next.playlistId)) {\n"
        "\t\t\t\t\t\tfoundPlaylist = playlist;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t});\n"
        "\t\t\t\tif (foundPlaylist) {\n"
        "\t\t\t\t\tnext.playlistName = foundPlaylist.name || next.playlistName || \"\";\n"
        "\t\t\t\t\tnext.sourcePath = foundPlaylist.sourcePath || \"\";\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n";
    const size_t hydrate_genre_at = ui.find(old_hydrate_genre);
    if (hydrate_genre_at != std::string::npos) {
        ui.replace(hydrate_genre_at, old_hydrate_genre.size(), new_hydrate_genre);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not refresh an open playlist from the saved list, so a removed song would stay on screen.");
    }
    const std::string old_enqueue_end =
        "\t\t\t\tactions.appendChild(queueBtn);\n"
        "\t\t\t}\n"
        "\t\t\tif (opts.queueEdit) {\n";
    const std::string new_enqueue_end =
        "\t\t\t\tactions.appendChild(queueBtn);\n"
        "\t\t\t}\n"
        "\t\t\tif (track && track.id != null && track.id !== \"\") {\n"
        "\t\t\t\tvar addPlaylistBtn = el(\"button\", \"smp-btn-small\", { type: \"button\", text: \"Add to playlist\", title: \"Add to playlist\" });\n"
        "\t\t\t\taddPlaylistBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tUI.doomnetOpenPlaylistMenu(addPlaylistBtn, [track.id]);\n"
        "\t\t\t\t});\n"
        "\t\t\t\tactions.appendChild(addPlaylistBtn);\n"
        "\t\t\t}\n"
        "\t\t\tif (opts.queueEdit) {\n";
    const size_t enqueue_end_at = ui.find(old_enqueue_end);
    if (enqueue_end_at != std::string::npos) {
        ui.replace(enqueue_end_at, old_enqueue_end.size(), new_enqueue_end);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not add Add to playlist on a track row, so a song cannot be saved to a playlist.");
    }
    const std::string old_album_queue = "\t\t\tactions.appendChild(queueAlbumBtn);\n";
    const std::string new_album_queue =
        "\t\t\tactions.appendChild(queueAlbumBtn);\n"
        "\t\t\tvar addAlbumPlaylistBtn = el(\"button\", \"smp-btn-small\", { type: \"button\", text: \"Add to playlist\" });\n"
        "\t\t\taddAlbumPlaylistBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\tvar ids = [];\n"
        "\t\t\t\tsortedTracks.forEach(function (track) {\n"
        "\t\t\t\t\tif (track && track.id != null && track.id !== \"\") {\n"
        "\t\t\t\t\t\tids.push(track.id);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t});\n"
        "\t\t\t\tUI.doomnetOpenPlaylistMenu(addAlbumPlaylistBtn, ids);\n"
        "\t\t\t});\n"
        "\t\t\tactions.appendChild(addAlbumPlaylistBtn);\n";
    const size_t album_queue_at = ui.find(old_album_queue);
    if (album_queue_at != std::string::npos) {
        ui.replace(album_queue_at, old_album_queue.size(), new_album_queue);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not add Add to playlist on an album, so the album's songs cannot be saved together.");
    }
    const std::string old_playlist_name = "var name = el(\"span\", \"smp-playlist-name\", { text: (playlist.name || \"Untitled\") + \" (\" + trackCount + \")\" });";
    const std::string new_playlist_name =
        "var name = el(\"button\", \"smp-playlist-name\", { type: \"button\", text: (playlist.name || \"Untitled\") + \" (\" + trackCount + \")\" });\n"
        "\t\t\t\tname.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tUI.navigateBrowse({ mode: \"playlistTracks\", playlistId: playlist.id, playlistName: playlist.name || \"Untitled\" }, { libraryGroupBy: \"playlists\" });\n"
        "\t\t\t\t});";
    const size_t playlist_name_at = ui.find(old_playlist_name);
    if (playlist_name_at != std::string::npos) {
        ui.replace(playlist_name_at, old_playlist_name.size(), new_playlist_name);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not open a playlist from its name, so the songs on it stay hidden.");
    }
    const std::string old_playlist_delete =
        "\t\t\t\tvar deleteBtn = el(\"button\", \"smp-btn-small smp-btn-danger\", { text: \"Delete\" });\n"
        "\t\t\t\tdeleteBtn.addEventListener(\"click\", function () {\n"
        "\t\t\t\t\tcallServer(\"delete_playlist\", [playlist.id]).then(function () {\n"
        "\t\t\t\t\t\treturn callServer(\"get_playlists\");\n"
        "\t\t\t\t\t}).then(function (playlists) {\n"
        "\t\t\t\t\t\tApp.playlists = asArray(playlists);\n"
        "\t\t\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t});\n"
        "\t\t\t\tif (playlist.sourcePath) {\n"
        "\t\t\t\t\trow.title = playlist.sourcePath;\n"
        "\t\t\t\t}\n"
        "\t\t\t\trow.appendChild(art);\n"
        "\t\t\t\trow.appendChild(name);\n"
        "\t\t\t\trow.appendChild(playBtn);\n"
        "\t\t\t\trow.appendChild(playNextBtn);\n"
        "\t\t\t\trow.appendChild(queueBtn);\n"
        "\t\t\t\tif (!playlist.sourcePath) {\n"
        "\t\t\t\t\trow.appendChild(deleteBtn);\n"
        "\t\t\t\t}\n";
    const std::string new_playlist_delete =
        "\t\t\t\tvar deleteBtn = el(\"button\", \"smp-btn-small smp-btn-danger\", { type: \"button\", text: playlist.sourcePath ? \"Remove\" : \"Delete\" });\n"
        "\t\t\t\tdeleteBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\tUI.doomnetConfirmPlaylist(playlist);\n"
        "\t\t\t\t});\n"
        "\t\t\t\tif (playlist.sourcePath) {\n"
        "\t\t\t\t\trow.title = playlist.sourcePath;\n"
        "\t\t\t\t}\n"
        "\t\t\t\trow.appendChild(art);\n"
        "\t\t\t\trow.appendChild(name);\n"
        "\t\t\t\trow.appendChild(playBtn);\n"
        "\t\t\t\trow.appendChild(playNextBtn);\n"
        "\t\t\t\trow.appendChild(queueBtn);\n"
        "\t\t\t\trow.appendChild(deleteBtn);\n";
    const size_t playlist_delete_at = ui.find(old_playlist_delete);
    if (playlist_delete_at != std::string::npos) {
        ui.replace(playlist_delete_at, old_playlist_delete.size(), new_playlist_delete);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not offer Remove on a playlist that came from a file, so that list stays in the player.");
    }
    const std::string old_render_playlists =
        "\t\trenderPlaylists: function (view) {\n"
        "\t\t\tif (!view) {\n"
        "\t\t\t\tview = UI.root && UI.root.querySelector(\".smp-view-library\");\n"
        "\t\t\t}\n"
        "\t\t\tif (!view) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n";
    const std::string new_render_playlists =
        "\t\tdoomnetConfirmPlaylist: function (playlist) {\n"
        "\t\t\tvar fromFile = !!(playlist && playlist.sourcePath);\n"
        "\t\t\tvar name = (playlist && playlist.name) || \"Untitled\";\n"
        "\t\t\tvar message = fromFile\n"
        "\t\t\t\t? \"Remove \\\"\" + name + \"\\\" from the player? The playlist file stays where it is.\"\n"
        "\t\t\t\t: \"Delete \\\"\" + name + \"\\\"? This removes it from the player.\";\n"
        "\t\t\tvar open = document.querySelector(\".doomnet-confirm-back\");\n"
        "\t\t\tif (open && open.parentNode) {\n"
        "\t\t\t\topen.parentNode.removeChild(open);\n"
        "\t\t\t}\n"
        "\t\t\tvar back = el(\"div\", \"doomnet-confirm-back\");\n"
        "\t\t\tvar box = el(\"div\", \"doomnet-confirm\");\n"
        "\t\t\tbox.setAttribute(\"role\", \"dialog\");\n"
        "\t\t\tbox.appendChild(el(\"p\", \"\", { text: message }));\n"
        "\t\t\tvar actions = el(\"div\", \"doomnet-confirm-actions\");\n"
        "\t\t\tvar cancel = el(\"button\", \"smp-btn-small\", { type: \"button\", text: \"Cancel\" });\n"
        "\t\t\tvar yes = el(\"button\", \"smp-btn-small smp-btn-danger\", { type: \"button\", text: fromFile ? \"Remove\" : \"Delete\" });\n"
        "\t\t\tcancel.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\tif (back.parentNode) {\n"
        "\t\t\t\t\tback.parentNode.removeChild(back);\n"
        "\t\t\t\t}\n"
        "\t\t\t});\n"
        "\t\t\tyes.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\tif (back.parentNode) {\n"
        "\t\t\t\t\tback.parentNode.removeChild(back);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tcallServer(fromFile ? \"dismiss_playlist\" : \"delete_playlist\", [playlist.id]).then(function () {\n"
        "\t\t\t\t\treturn callServer(\"get_playlists\");\n"
        "\t\t\t\t}).then(function (list) {\n"
        "\t\t\t\t\tApp.playlists = asArray(list);\n"
        "\t\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t\t});\n"
        "\t\t\t});\n"
        "\t\t\tactions.appendChild(cancel);\n"
        "\t\t\tactions.appendChild(yes);\n"
        "\t\t\tbox.appendChild(actions);\n"
        "\t\t\tback.appendChild(box);\n"
        "\t\t\tback.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\tif (event.target === back && back.parentNode) {\n"
        "\t\t\t\t\tback.parentNode.removeChild(back);\n"
        "\t\t\t\t}\n"
        "\t\t\t});\n"
        "\t\t\tdocument.body.appendChild(back);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetEditablePlaylists: function () {\n"
        "\t\t\treturn asArray(App.playlists).filter(function (playlist) {\n"
        "\t\t\t\treturn playlist && !playlist.sourcePath;\n"
        "\t\t\t}).sort(function (a, b) {\n"
        "\t\t\t\treturn String((a && a.name) || \"\").localeCompare(String((b && b.name) || \"\"), undefined, { sensitivity: \"base\", numeric: true });\n"
        "\t\t\t});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetTrackOnPlaylist: function (playlist, trackId) {\n"
        "\t\t\tvar ids = asArray(playlist && playlist.trackIds);\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = 0; i < ids.length; i++) {\n"
        "\t\t\t\tif (sameTrackId(ids[i], trackId)) {\n"
        "\t\t\t\t\treturn true;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\treturn false;\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetClosePlaylistMenu: function () {\n"
        "\t\t\tvar open = document.querySelector(\".doomnet-playlist-menu\");\n"
        "\t\t\tif (open && open.parentNode) {\n"
        "\t\t\t\topen.parentNode.removeChild(open);\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetAddTracksToPlaylist: function (playlist, trackIds) {\n"
        "\t\t\tvar pending = [];\n"
        "\t\t\tasArray(trackIds).forEach(function (id) {\n"
        "\t\t\t\tif (id == null || id === \"\" || UI.doomnetTrackOnPlaylist(playlist, id)) {\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tpending.push(id);\n"
        "\t\t\t});\n"
        "\t\t\tvar chain = Promise.resolve();\n"
        "\t\t\tpending.forEach(function (id) {\n"
        "\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\treturn callServer(\"playlist_add_track\", [playlist.id, id]);\n"
        "\t\t\t\t});\n"
        "\t\t\t});\n"
        "\t\t\treturn chain.then(function () {\n"
        "\t\t\t\treturn callServer(\"get_playlists\");\n"
        "\t\t\t}).then(function (list) {\n"
        "\t\t\t\tApp.playlists = asArray(list);\n"
        "\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetOpenPlaylistMenu: function (anchor, trackIds) {\n"
        "\t\t\tUI.doomnetClosePlaylistMenu();\n"
        "\t\t\tvar menu = el(\"div\", \"doomnet-playlist-menu\");\n"
        "\t\t\tvar editable = UI.doomnetEditablePlaylists();\n"
        "\t\t\tvar ids = asArray(trackIds).filter(function (id) {\n"
        "\t\t\t\treturn id != null && id !== \"\";\n"
        "\t\t\t});\n"
        "\t\t\tif (!editable.length) {\n"
        "\t\t\t\tmenu.appendChild(el(\"div\", \"doomnet-playlist-menu-empty\", { text: \"Create a playlist on the Playlists tab.\" }));\n"
        "\t\t\t} else {\n"
        "\t\t\t\teditable.forEach(function (playlist) {\n"
        "\t\t\t\t\tvar allIn = ids.length > 0;\n"
        "\t\t\t\t\tvar n;\n"
        "\t\t\t\t\tfor (n = 0; n < ids.length; n++) {\n"
        "\t\t\t\t\t\tif (!UI.doomnetTrackOnPlaylist(playlist, ids[n])) {\n"
        "\t\t\t\t\t\t\tallIn = false;\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tvar item = el(\"div\", \"doomnet-playlist-menu-item\" + (allIn ? \" added\" : \"\"), {\n"
        "\t\t\t\t\t\trole: \"menuitem\",\n"
        "\t\t\t\t\t\ttext: (playlist.name || \"Untitled\") + (allIn ? \" (added)\" : \"\"),\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\titem.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tif (allIn) {\n"
        "\t\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\tUI.doomnetClosePlaylistMenu();\n"
        "\t\t\t\t\t\tUI.doomnetAddTracksToPlaylist(playlist, ids);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tmenu.appendChild(item);\n"
        "\t\t\t\t});\n"
        "\t\t\t}\n"
        "\t\t\tmenu.style.position = \"fixed\";\n"
        "\t\t\tmenu.setAttribute(\"role\", \"menu\");\n"
        "\t\t\tdocument.body.appendChild(menu);\n"
        "\t\t\tvar rect = anchor && anchor.getBoundingClientRect ? anchor.getBoundingClientRect() : { top: 8, bottom: 8, left: 8, width: 180 };\n"
        "\t\t\tvar menuRect = menu.getBoundingClientRect();\n"
        "\t\t\tvar top = rect.bottom + 2;\n"
        "\t\t\tif (top + menuRect.height > window.innerHeight - 8) {\n"
        "\t\t\t\ttop = rect.top - menuRect.height - 2;\n"
        "\t\t\t}\n"
        "\t\t\tvar left = rect.left;\n"
        "\t\t\tvar width = Math.max(rect.width || 0, menuRect.width);\n"
        "\t\t\tif (left + width > window.innerWidth - 8) {\n"
        "\t\t\t\tleft = window.innerWidth - width - 8;\n"
        "\t\t\t}\n"
        "\t\t\tleft = Math.max(8, left);\n"
        "\t\t\ttop = Math.max(8, top);\n"
        "\t\t\tmenu.style.left = left + \"px\";\n"
        "\t\t\tmenu.style.top = top + \"px\";\n"
        "\t\t\tmenu.style.minWidth = width + \"px\";\n"
        "\t\t\tvar placed = menu.getBoundingClientRect();\n"
        "\t\t\tif (Math.abs(placed.left - left) > 1 || Math.abs(placed.top - top) > 1) {\n"
        "\t\t\t\tmenu.style.left = (left + (left - placed.left)) + \"px\";\n"
        "\t\t\t\tmenu.style.top = (top + (top - placed.top)) + \"px\";\n"
        "\t\t\t}\n"
        "\t\t\tsetTimeout(function () {\n"
        "\t\t\t\tfunction closeMenu(event) {\n"
        "\t\t\t\t\tif (menu.contains(event.target)) {\n"
        "\t\t\t\t\t\treturn;\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tUI.doomnetClosePlaylistMenu();\n"
        "\t\t\t\t\tdocument.removeEventListener(\"pointerdown\", closeMenu, true);\n"
        "\t\t\t\t}\n"
        "\t\t\t\tdocument.addEventListener(\"pointerdown\", closeMenu, true);\n"
        "\t\t\t}, 0);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetFinishPlaylistDrag: function (playlist) {\n"
        "\t\t\tif (UI.doomnetPlaylistDragDone) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetPlaylistDragDone = true;\n"
        "\t\t\tvar from = UI.doomnetPlaylistDragFrom;\n"
        "\t\t\tvar over = UI.doomnetPlaylistDragOver;\n"
        "\t\t\tif (from == null || over == null || from === over) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tUI.doomnetMovePlaylistTrack(playlist, from, over - from);\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetMovePlaylistTrack: function (playlist, index, delta) {\n"
        "\t\t\tif (UI.doomnetPlaylistBusy || !playlist || playlist.sourcePath) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar ids = asArray(playlist.trackIds).slice();\n"
        "\t\t\tvar target = index + delta;\n"
        "\t\t\tif (target < 0 || target >= ids.length || delta === 0) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar moved = ids.splice(index, 1)[0];\n"
        "\t\t\tids.splice(target, 0, moved);\n"
        "\t\t\tvar start = Math.min(index, target);\n"
        "\t\t\tvar tail = ids.slice(start);\n"
        "\t\t\tvar current = asArray(playlist.trackIds);\n"
        "\t\t\tUI.doomnetPlaylistBusy = true;\n"
        "\t\t\tvar chain = Promise.resolve();\n"
        "\t\t\tvar i;\n"
        "\t\t\tfor (i = current.length - 1; i >= start; i--) {\n"
        "\t\t\t\t(function (id) {\n"
        "\t\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\t\treturn callServer(\"playlist_remove_track\", [playlist.id, id]);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t})(current[i]);\n"
        "\t\t\t}\n"
        "\t\t\ttail.forEach(function (id) {\n"
        "\t\t\t\tchain = chain.then(function () {\n"
        "\t\t\t\t\treturn callServer(\"playlist_add_track\", [playlist.id, id]);\n"
        "\t\t\t\t});\n"
        "\t\t\t});\n"
        "\t\t\tchain.then(function () {\n"
        "\t\t\t\treturn callServer(\"get_playlists\");\n"
        "\t\t\t}).then(function (list) {\n"
        "\t\t\t\tApp.playlists = asArray(list);\n"
        "\t\t\t\tUI.doomnetPlaylistBusy = false;\n"
        "\t\t\t\tUI.doomnetKeepPlaylistScroll = (UI.libraryScrollEl() && UI.libraryScrollEl().scrollTop) || 0;\n"
        "\t\t\t\tif (document.activeElement && document.activeElement.blur) {\n"
        "\t\t\t\t\tdocument.activeElement.blur();\n"
        "\t\t\t\t}\n"
        "\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t}).catch(function () {\n"
        "\t\t\t\tUI.doomnetPlaylistBusy = false;\n"
        "\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t});\n"
        "\t\t},\n"
        "\n"
        "\t\tdoomnetRenderPlaylistTracks: function (view) {\n"
        "\t\t\tvar drill = UI.libraryDrill || {};\n"
        "\t\t\tvar playlist = null;\n"
        "\t\t\tasArray(App.playlists).forEach(function (item) {\n"
        "\t\t\t\tif (item && String(item.id) === String(drill.playlistId)) {\n"
        "\t\t\t\t\tplaylist = item;\n"
        "\t\t\t\t}\n"
        "\t\t\t});\n"
        "\t\t\tvar title = (playlist && playlist.name) || drill.playlistName || \"Untitled\";\n"
        "\t\t\tUI.renderDrillBackBar(view, title, UI.goBack);\n"
        "\t\t\tif (!playlist) {\n"
        "\t\t\t\tview.appendChild(el(\"div\", \"smp-empty\", { text: \"This playlist is no longer available.\" }));\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar ids = asArray(playlist.trackIds);\n"
        "\t\t\tif (!ids.length) {\n"
        "\t\t\t\tview.appendChild(el(\"div\", \"smp-empty\", { text: \"This playlist has no songs yet. Add them from a track.\" }));\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar tracks = [];\n"
        "\t\t\tids.forEach(function (id) {\n"
        "\t\t\t\ttracks.push(trackById(id) || { id: id, title: \"Unknown title\", artist: \"\", album: \"\" });\n"
        "\t\t\t});\n"
        "\t\t\tvar list = el(\"div\", \"smp-track-list\");\n"
        "\t\t\ttracks.forEach(function (track, idx) {\n"
        "\t\t\t\tvar row = UI.buildTrackRow(track, idx, tracks, {\n"
        "\t\t\t\t\tshowArtistAlbum: true,\n"
        "\t\t\t\t\tallowEnqueue: true,\n"
        "\t\t\t\t\tallowPlayNext: true,\n"
        "\t\t\t\t\tplayInOrder: true,\n"
        "\t\t\t\t});\n"
        "\t\t\t\tif (!playlist.sourcePath) {\n"
        "\t\t\t\t\tvar upBtn = el(\"button\", \"smp-btn-small smp-btn-iconish\", { text: \"\\u2191\", type: \"button\", title: \"Move Up\" });\n"
        "\t\t\t\t\tupBtn.disabled = idx === 0;\n"
        "\t\t\t\t\tupBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tUI.doomnetMovePlaylistTrack(playlist, idx, -1);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tvar downBtn = el(\"button\", \"smp-btn-small smp-btn-iconish\", { text: \"\\u2193\", type: \"button\", title: \"Move Down\" });\n"
        "\t\t\t\t\tdownBtn.disabled = idx >= ids.length - 1;\n"
        "\t\t\t\t\tdownBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tUI.doomnetMovePlaylistTrack(playlist, idx, 1);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tvar removeBtn = el(\"button\", \"smp-btn-small smp-btn-danger\", { type: \"button\", text: \"Remove\" });\n"
        "\t\t\t\t\tremoveBtn.addEventListener(\"click\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tcallServer(\"playlist_remove_track\", [playlist.id, track.id]).then(function () {\n"
        "\t\t\t\t\t\t\treturn callServer(\"get_playlists\");\n"
        "\t\t\t\t\t\t}).then(function (list) {\n"
        "\t\t\t\t\t\t\tApp.playlists = asArray(list);\n"
        "\t\t\t\t\t\t\tUI.doomnetKeepPlaylistScroll = (UI.libraryScrollEl() && UI.libraryScrollEl().scrollTop) || 0;\n"
        "\t\t\t\t\t\t\tif (document.activeElement && document.activeElement.blur) {\n"
        "\t\t\t\t\t\t\t\tdocument.activeElement.blur();\n"
        "\t\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\t\tUI.safely(\"renderLibrary\", UI.renderLibrary);\n"
        "\t\t\t\t\t\t});\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\tvar actions = row.querySelector(\".smp-track-actions\");\n"
        "\t\t\t\t\tif (!actions) {\n"
        "\t\t\t\t\t\tactions = el(\"div\", \"smp-track-actions\");\n"
        "\t\t\t\t\t\trow.appendChild(actions);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\tactions.appendChild(upBtn);\n"
        "\t\t\t\t\tactions.appendChild(downBtn);\n"
        "\t\t\t\t\tactions.appendChild(removeBtn);\n"
        "\t\t\t\t\trow.classList.add(\"doomnet-playlist-row\");\n"
        "\t\t\t\t\trow.setAttribute(\"draggable\", \"true\");\n"
        "\t\t\t\t\trow.addEventListener(\"dragstart\", function (event) {\n"
        "\t\t\t\t\t\tif (event.dataTransfer) {\n"
        "\t\t\t\t\t\t\tevent.dataTransfer.setData(\"text/plain\", String(idx));\n"
        "\t\t\t\t\t\t\tevent.dataTransfer.effectAllowed = \"copyMove\";\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragFrom = idx;\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragOver = idx;\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragDone = false;\n"
        "\t\t\t\t\t\trow.classList.add(\"dragging\");\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\trow.addEventListener(\"dragover\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tif (event.dataTransfer) {\n"
        "\t\t\t\t\t\t\tevent.dataTransfer.dropEffect = \"move\";\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragOver = idx;\n"
        "\t\t\t\t\t\tvar marked = row.parentNode ? row.parentNode.querySelectorAll(\".doomnet-playlist-row\") : [];\n"
        "\t\t\t\t\t\tvar mark;\n"
        "\t\t\t\t\t\tfor (mark = 0; mark < marked.length; mark++) {\n"
        "\t\t\t\t\t\t\tmarked[mark].classList.remove(\"drag-over\");\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\trow.classList.add(\"drag-over\");\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\trow.addEventListener(\"drop\", function (event) {\n"
        "\t\t\t\t\t\tevent.preventDefault();\n"
        "\t\t\t\t\t\tevent.stopPropagation();\n"
        "\t\t\t\t\t\tUI.doomnetFinishPlaylistDrag(playlist);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t\trow.addEventListener(\"dragend\", function () {\n"
        "\t\t\t\t\t\trow.classList.remove(\"dragging\");\n"
        "\t\t\t\t\t\tvar parent = row.parentNode;\n"
        "\t\t\t\t\t\tif (parent) {\n"
        "\t\t\t\t\t\t\tparent.querySelectorAll(\".drag-over\").forEach(function (elRow) {\n"
        "\t\t\t\t\t\t\t\telRow.classList.remove(\"drag-over\");\n"
        "\t\t\t\t\t\t\t});\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t\tUI.doomnetFinishPlaylistDrag(playlist);\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragFrom = null;\n"
        "\t\t\t\t\t\tUI.doomnetPlaylistDragOver = null;\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t}\n"
        "\t\t\t\tlist.appendChild(row);\n"
        "\t\t\t});\n"
        "\t\t\tview.appendChild(list);\n"
        "\t\t},\n"
        "\n"
        "\t\trenderPlaylists: function (view) {\n"
        "\t\t\tif (!view) {\n"
        "\t\t\t\tview = UI.root && UI.root.querySelector(\".smp-view-library\");\n"
        "\t\t\t}\n"
        "\t\t\tif (!view) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tif (UI.libraryDrill && UI.libraryDrill.mode === \"playlistTracks\") {\n"
        "\t\t\t\tUI.doomnetRenderPlaylistTracks(view);\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n";
    const size_t render_playlists_at = ui.find(old_render_playlists);
    if (render_playlists_at != std::string::npos) {
        ui.replace(render_playlists_at, old_render_playlists.size(), new_render_playlists);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not render the playlist list, so a song cannot be added or opened.");
    }
    const std::string old_playlist_scroll =
        "\t\t\tUI.detachBrowsePages();\n"
        "\t\t\twhile (chrome.pane.firstChild) {\n";
    const std::string new_playlist_scroll =
        "\t\t\tUI.detachBrowsePages();\n"
        "\t\t\tvar doomnetPlaylistScroll = null;\n"
        "\t\t\tif (UI.libraryGroupBy === \"playlists\" && UI.libraryDrill && UI.libraryDrill.mode === \"playlistTracks\" && typeof UI.doomnetKeepPlaylistScroll === \"number\") {\n"
        "\t\t\t\tdoomnetPlaylistScroll = UI.doomnetKeepPlaylistScroll;\n"
        "\t\t\t\tUI.doomnetKeepPlaylistScroll = null;\n"
        "\t\t\t}\n"
        "\t\t\twhile (chrome.pane.firstChild) {\n";
    const size_t playlist_scroll_at = ui.find(old_playlist_scroll);
    if (playlist_scroll_at != std::string::npos) {
        ui.replace(playlist_scroll_at, old_playlist_scroll.size(), new_playlist_scroll);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not keep the playlist scroll, so moving a song jumps back to the top.");
    }
    const std::string old_playlist_top =
        "\t\t\tif (UI.libraryGroupBy === \"playlists\") {\n"
        "\t\t\t\tUI.renderPlaylists(pane);\n"
        "\t\t\t\tpane.scrollTop = 0;\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n";
    const std::string new_playlist_top =
        "\t\t\tif (UI.libraryGroupBy === \"playlists\") {\n"
        "\t\t\t\tUI.renderPlaylists(pane);\n"
        "\t\t\t\tif (typeof doomnetPlaylistScroll === \"number\") {\n"
        "\t\t\t\t\tpane.scrollTop = doomnetPlaylistScroll;\n"
        "\t\t\t\t\trequestAnimationFrame(function () {\n"
        "\t\t\t\t\t\tpane.scrollTop = doomnetPlaylistScroll;\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t} else {\n"
        "\t\t\t\t\tpane.scrollTop = 0;\n"
        "\t\t\t\t}\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n";
    const size_t playlist_top_at = ui.find(old_playlist_top);
    if (playlist_top_at != std::string::npos) {
        ui.replace(playlist_top_at, old_playlist_top.size(), new_playlist_top);
    } else {
        log_event(L"warn", L"inject", L"The player UI still resets the playlist scroll after a song moves.");
    }
    const std::string old_album_art =
        "\t\talbumArtSafe: function (track) {\n"
        "\t\t\tif (!track) {\n"
        "\t\t\t\treturn \"\";\n"
        "\t\t\t}\n"
        "\t\t\tif (typeof track === \"string\") {\n"
        "\t\t\t\treturn track;\n"
        "\t\t\t}\n"
        "\t\t\tvar artist = track.albumArtist || track.artist || \"Unknown Artist\";\n"
        "\t\t\tvar album = track.album || \"Unknown Album\";\n"
        "\t\t\tvar label = String(artist).toLowerCase() + \"\\t\" + String(album).toLowerCase();\n"
        "\t\t\tvar safe = label.replace(/[^A-Za-z0-9]+/g, \"_\").replace(/^_+|_+$/g, \"\");\n"
        "\t\t\tif (!safe) {\n"
        "\t\t\t\tsafe = \"unknown\";\n"
        "\t\t\t}\n"
        "\t\t\tif (safe.length > 80) {\n"
        "\t\t\t\tsafe = safe.slice(0, 80);\n"
        "\t\t\t}\n"
        "\t\t\treturn safe;\n"
        "\t\t},";
    const std::string new_album_art =
        "\t\talbumArtSafe: function (track) {\n"
        "\t\t\tif (!track) {\n"
        "\t\t\t\treturn \"\";\n"
        "\t\t\t}\n"
        "\t\t\tif (typeof track === \"string\") {\n"
        "\t\t\t\treturn track;\n"
        "\t\t\t}\n"
        "\t\t\tvar artist = track.albumArtist || track.artist || \"Unknown Artist\";\n"
        "\t\t\tvar album = track.album || \"Unknown Album\";\n"
        "\t\t\tvar label = String(artist).toLowerCase() + \"\\t\" + String(album).toLowerCase();\n"
        "\t\t\tvar safe = label.replace(/[^A-Za-z0-9]+/g, \"_\").replace(/^_+|_+$/g, \"\");\n"
        "\t\t\tif (!safe) {\n"
        "\t\t\t\tsafe = \"unknown\";\n"
        "\t\t\t}\n"
        "\t\t\tif (safe.length > 80) {\n"
        "\t\t\t\tsafe = safe.slice(0, 80);\n"
        "\t\t\t}\n"
        "\t\t\tif (!/[A-Za-z0-9]/.test(String(album)) && window.TextEncoder) {\n"
        "\t\t\t\tvar asciiLower = function (value) {\n"
        "\t\t\t\t\treturn String(value).replace(/[A-Z]/g, function (ch) {\n"
        "\t\t\t\t\t\treturn String.fromCharCode(ch.charCodeAt(0) + 32);\n"
        "\t\t\t\t\t});\n"
        "\t\t\t\t};\n"
        "\t\t\t\tvar hashLabel = asciiLower(artist) + \"\\t\" + asciiLower(album);\n"
        "\t\t\t\tvar bytes = new TextEncoder().encode(hashLabel);\n"
        "\t\t\t\tvar h = 2166136261;\n"
        "\t\t\t\tvar i;\n"
        "\t\t\t\tfor (i = 0; i < bytes.length; i++) {\n"
        "\t\t\t\t\th = (h + bytes[i]) >>> 0;\n"
        "\t\t\t\t\tvar aLo = h & 65535;\n"
        "\t\t\t\t\tvar aHi = h >>> 16;\n"
        "\t\t\t\t\th = ((aLo * 403) + (((aLo * 256 + aHi * 403) & 65535) << 16)) >>> 0;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar hex = h.toString(16);\n"
        "\t\t\t\twhile (hex.length < 8) { hex = \"0\" + hex; }\n"
        "\t\t\t\tif (safe.length > 71) { safe = safe.slice(0, 71); }\n"
        "\t\t\t\tsafe = safe + \"_\" + hex;\n"
        "\t\t\t}\n"
        "\t\t\treturn safe;\n"
        "\t\t},";
    const size_t album_art_at = ui.find(old_album_art);
    if (album_art_at != std::string::npos) {
        ui.replace(album_art_at, old_album_art.size(), new_album_art);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not name album covers the same way as the library, so albums with no English letters still share one picture.");
    }
    const std::string old_prev =
        "if ((command.action === \"play\" || command.action === \"pause\" || command.action === \"toggle\") && !(Number(command.positionSeconds) > 0)) {\n"
        "\t\t\t\tcommand.positionSeconds = UI.playbackProgress().elapsed;\n"
        "\t\t\t}";
    const std::string new_prev =
        "if ((command.action === \"play\" || command.action === \"pause\" || command.action === \"toggle\") && !(Number(command.positionSeconds) > 0)) {\n"
        "\t\t\t\tcommand.positionSeconds = UI.playbackProgress().elapsed;\n"
        "\t\t\t}\n"
        "\t\t\tif (command.action === \"prev\" && UI.playbackProgress().elapsed > 5) {\n"
        "\t\t\t\tcommand.action = \"seek\";\n"
        "\t\t\t\tcommand.value = 0;\n"
        "\t\t\t}";
    const size_t prev_at = ui.find(old_prev);
    if (prev_at != std::string::npos) {
        ui.replace(prev_at, old_prev.size(), new_prev);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the Previous command, so that button still skips to the previous song after five seconds.");
    }
    const std::string old_volume =
        "\t\t\t\tcase \"volume\":\n"
        "\t\t\t\t\t// Live drag path - update gain only, no bar rebuild.\n";
    const std::string new_volume =
        "\t\t\t\tcase \"volumeBump\":\n"
        "\t\t\t\t\tEngine.followWheelVolume(Number(command.value) || 0);\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\tcase \"volume\":\n"
        "\t\t\t\t\t// Live drag path - update gain only, no bar rebuild.\n";
    const size_t volume_at = ui.find(old_volume);
    if (volume_at != std::string::npos) {
        ui.replace(volume_at, old_volume.size(), new_volume);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the volume command, so a global volume key cannot change the player.");
    }
    const std::string old_feature =
        "\t\tappendFeatureBlock: function (parent, key, label, hint, buildDependants) {\n";
    const std::string new_feature =
        "\t\tappendGlobalControlBlock: function (section) {\n"
        "\t\t\tvar mediaOn = settingFlag(App.settings.mediaKeysEnabled, true);\n"
        "\t\t\tif (!mediaOn && settingFlag(App.settings.globalControl, false)) {\n"
        "\t\t\t\tUI.commitSetting(\"globalControl\", false, false);\n"
        "\t\t\t}\n"
        "\t\t\tvar block = el(\"div\", \"smp-feature-block\" + (mediaOn ? \"\" : \" is-locked\"));\n"
        "\t\t\tvar row = UI.settingsToggleRow(\n"
        "\t\t\t\t\"globalControl\",\n"
        "\t\t\t\t\"Global Control\",\n"
        "\t\t\t\t\"Play, pause, stop, next, previous, and the volume keys control this player even when Steam is not the window in front. Windows does not change the system volume, and other players do not receive those keys. This follows the Media Keys switch above.\",\n"
        "\t\t\t\ttrue\n"
        "\t\t\t);\n"
        "\t\t\tif (!mediaOn) {\n"
        "\t\t\t\tvar box = row.querySelector(\"input\");\n"
        "\t\t\t\tif (box) {\n"
        "\t\t\t\t\tbox.disabled = true;\n"
        "\t\t\t\t\tbox.checked = false;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar toggle = row.querySelector(\".smp-toggle-row\");\n"
        "\t\t\t\tif (toggle) {\n"
        "\t\t\t\t\ttoggle.setAttribute(\"aria-disabled\", \"true\");\n"
        "\t\t\t\t\ttoggle.setAttribute(\"aria-checked\", \"false\");\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tblock.appendChild(row);\n"
        "\t\t\tsection.appendChild(block);\n"
        "\t\t},\n"
        "\n"
        "\t\tappendFeatureBlock: function (parent, key, label, hint, buildDependants) {\n";
    const size_t feature_at = ui.find(old_feature);
    if (feature_at != std::string::npos) {
        ui.replace(feature_at, old_feature.size(), new_feature);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the settings feature block, so Global Control stays available while Media Keys is off.");
    }
    const std::string old_wheel =
        "\t\tbumpKeyboardVolume: function (delta) {\n";
    const std::string new_wheel =
        "\t\tfollowWheelVolume: function (delta) {\n"
        "\t\t\tif (!delta) {\n"
        "\t\t\t\tEngine.bumpKeyboardVolume(0);\n"
        "\t\t\t\tvar muted = App.engine ? App.engine.volume : 0;\n"
        "\t\t\t\tEngine._wheelTarget = muted;\n"
        "\t\t\t\tEngine._wheelShown = muted;\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar current = Engine._wheelTarget;\n"
        "\t\t\tif (typeof current !== \"number\") {\n"
        "\t\t\t\tcurrent = App.engine ? App.engine.volume : App.playerState && App.playerState.volume;\n"
        "\t\t\t\tcurrent = typeof current === \"number\" ? current : 0.8;\n"
        "\t\t\t}\n"
        "\t\t\tEngine._wheelTarget = Math.max(0, Math.min(1, current + delta));\n"
        "\t\t\tif (typeof Engine._wheelShown !== \"number\") {\n"
        "\t\t\t\tEngine._wheelShown = current;\n"
        "\t\t\t}\n"
        "\t\t\tif (Engine._wheelFrame) {\n"
        "\t\t\t\treturn;\n"
        "\t\t\t}\n"
        "\t\t\tvar last = performance.now();\n"
        "\t\t\tvar step = function (now) {\n"
        "\t\t\t\tvar dt = Math.max(0.001, Math.min(0.05, (now - last) / 1000));\n"
        "\t\t\t\tlast = now;\n"
        "\t\t\t\tvar target = Engine._wheelTarget;\n"
        "\t\t\t\tvar shown = Engine._wheelShown;\n"
        "\t\t\t\tvar blend = 1 - Math.exp(-dt / 0.012);\n"
        "\t\t\t\tshown = shown + (target - shown) * blend;\n"
        "\t\t\t\tif (Math.abs(target - shown) < 0.001) {\n"
        "\t\t\t\t\tshown = target;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tEngine._wheelShown = shown;\n"
        "\t\t\t\tif (App.engine && App.engine.setVolumeSilent) {\n"
        "\t\t\t\t\tApp.engine.setVolumeSilent(shown);\n"
        "\t\t\t\t} else if (App.playerState) {\n"
        "\t\t\t\t\tApp.playerState.volume = shown;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tvar sliders = document.querySelectorAll(\".smp-volume\");\n"
        "\t\t\t\tvar i;\n"
        "\t\t\t\tfor (i = 0; i < sliders.length; i++) {\n"
        "\t\t\t\t\tsliders[i].value = String(Math.round(shown * 1500));\n"
        "\t\t\t\t}\n"
        "\t\t\t\tEngine.schedulePushState(300);\n"
        "\t\t\t\tif (shown === target) {\n"
        "\t\t\t\t\tEngine._wheelFrame = null;\n"
        "\t\t\t\t\treturn;\n"
        "\t\t\t\t}\n"
        "\t\t\t\tEngine._wheelFrame = requestAnimationFrame(step);\n"
        "\t\t\t};\n"
        "\t\t\tEngine._wheelFrame = requestAnimationFrame(step);\n"
        "\t\t},\n"
        "\n"
        "\t\tbumpKeyboardVolume: function (delta) {\n";
    const size_t wheel_at = ui.find(old_wheel);
    if (wheel_at != std::string::npos) {
        ui.replace(wheel_at, old_wheel.size(), new_wheel);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the keyboard volume change, so the volume wheel still jumps.");
    }
    const std::string old_key_guard = "\t\t\tvar onMediaKey = function (event) {\n\t\t\t\tif (!mediaKeysAllowed()) {\n\t\t\t\t\treturn;\n\t\t\t\t}\n";
    const std::string new_key_guard = "\t\t\tvar onMediaKey = function (event) {\n\t\t\t\tif (!mediaKeysAllowed() || settingFlag(App.settings.globalControl, false)) {\n\t\t\t\t\treturn;\n\t\t\t\t}\n";
    const size_t key_guard_at = ui.find(old_key_guard);
    if (key_guard_at != std::string::npos) {
        ui.replace(key_guard_at, old_key_guard.size(), new_key_guard);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the focused media-key handler, so a held key can still skip extra tracks while Steam is in front.");
    }
    const std::string old_session_guard = "\t\tdispatchMediaAction: function (action, extra) {\n\t\t\tif (!mediaKeysAllowed()) {\n\t\t\t\treturn;\n\t\t\t}\n";
    const std::string new_session_guard = "\t\tdispatchMediaAction: function (action, extra) {\n\t\t\tif (!mediaKeysAllowed() || settingFlag(App.settings.globalControl, false)) {\n\t\t\t\treturn;\n\t\t\t}\n";
    const size_t session_guard_at = ui.find(old_session_guard);
    if (session_guard_at != std::string::npos) {
        ui.replace(session_guard_at, old_session_guard.size(), new_session_guard);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the media-session handler, so one press can still be delivered again by Windows.");
    }
    const std::string old_media =
        "\t\t\tUI.appendFeatureBlock(\n"
        "\t\t\t\tsection,\n"
        "\t\t\t\t\"mediaKeysEnabled\",\n"
        "\t\t\t\t\"Media Keys\",\n"
        "\t\t\t\t\"Play, pause, next, and previous, plus a keyboard volume wheel or volume keys when Windows sends them to Steam.\"\n"
        "\t\t\t);\n";
    const std::string new_media =
        "\t\t\tUI.appendFeatureBlock(\n"
        "\t\t\t\tsection,\n"
        "\t\t\t\t\"mediaKeysEnabled\",\n"
        "\t\t\t\t\"Media Keys\",\n"
        "\t\t\t\t\"Play, pause, next, and previous, plus a keyboard volume wheel or volume keys when Windows sends them to Steam.\"\n"
        "\t\t\t);\n"
        "\t\t\tUI.appendGlobalControlBlock(section);\n";
    const size_t media_at = ui.find(old_media);
    if (media_at != std::string::npos) {
        ui.replace(media_at, old_media.size(), new_media);
    } else {
        log_event(L"warn", L"inject", L"The player UI did not contain the Media Keys setting, so Global Control was not added.");
    }
    return ui;
}

std::string patched_player_css_text() {
    const std::wstring dir = L"D:\\Documents\\Repos\\SteamTunes\\backend\\assets\\frontend";
    std::string css = read_skin_text(dir, "steam-music-player.css");
    if (css.empty()) {
        return {};
    }
    css +=
        "\n.smp-feature-block.is-locked{opacity:0.45;}"
        "\n.smp-feature-block.is-locked .smp-toggle-row{pointer-events:none;cursor:default;}"
        "\n.smp-feature-block.is-locked .smp-toggle-checkbox{cursor:default;}"
        "\n.smp-nptab-artist.smp-artist-link{pointer-events:none;}"
        "\n.smp-nptab-artist.smp-artist-link .smp-marquee-inner{pointer-events:auto;cursor:pointer;display:inline-block;}"
        "\n.smp-nptab-artist.smp-artist-link:hover,.smp-nptab-artist.smp-artist-link:focus-visible{color:inherit;text-decoration:none;}"
        "\n.smp-nptab-artist.smp-artist-link .smp-marquee-inner:hover,.smp-nptab-artist.smp-artist-link:focus-visible .smp-marquee-inner{color:var(--smp-accent);text-decoration:underline;}"
        "\n.smp-nptab-artist.smp-artist-link:focus-visible{outline:none;}"
        "\n.smp-nptab-artist.smp-artist-link:focus-visible .smp-marquee-inner{outline:1px solid var(--smp-accent);outline-offset:2px;}\n"
        "\nbutton.smp-artist-link{display:inline-block;width:fit-content;max-width:100%;}\n"
        "\nbutton.smp-album-artist.smp-artist-link{display:inline-block;width:fit-content;max-width:100%;text-align:left;}\n"
        "\nbutton.smp-drill-album-artist.smp-artist-link{display:inline-block;width:fit-content;max-width:100%;}\n"
        "\n.smp-track-sub .smp-artist-link{display:inline;width:auto;max-width:100%;}\n"
        "\n#doomnet-steamtunes-host .smp-panel.smp-tab-page{position:absolute !important;inset:0 !important;width:100% !important;height:100% !important;max-width:none !important;max-height:none !important;border:0 !important;border-radius:0 !important;box-shadow:none !important;}\n"
        "#doomnet-steamtunes-host .smp-nptab{align-items:stretch;padding:28px 32px 36px;}\n"
        "#doomnet-steamtunes-host .smp-nptab-backdrop{display:block;top:0;left:0;right:0;bottom:0;height:auto;transform:scale(1.08);-webkit-mask-image:linear-gradient(180deg,rgba(0,0,0,0.95) 0%,rgba(0,0,0,0.7) 26%,rgba(0,0,0,0.28) 58%,rgba(0,0,0,0) 78%);mask-image:linear-gradient(180deg,rgba(0,0,0,0.95) 0%,rgba(0,0,0,0.7) 26%,rgba(0,0,0,0.28) 58%,rgba(0,0,0,0) 78%);}\n"
        "#doomnet-steamtunes-host .smp-nptab{align-items:center;}\n"
        "#doomnet-steamtunes-host .smp-nptab-hero{flex-direction:column;align-items:center;justify-content:center;gap:22px;width:min(480px,92%);margin:0 auto;}\n"
        "#doomnet-steamtunes-host .smp-nptab-art-wrap{width:min(340px,78%);max-width:340px;flex:0 0 auto;margin:0;transform:none;}\n"
        "#doomnet-steamtunes-host .smp-nptab-info{align-items:center;text-align:center;width:100%;max-width:340px;flex:0 0 auto;padding:0;margin:0;transform:none;}\n"
        "#doomnet-steamtunes-host .smp-nptab-eyebrow,#doomnet-steamtunes-host .smp-nptab-title,#doomnet-steamtunes-host .smp-nptab-artist,#doomnet-steamtunes-host .smp-nptab-album,#doomnet-steamtunes-host .smp-nptab-modes{order:0;}\n"
        "#doomnet-steamtunes-host .smp-nptab-eyebrow{margin-top:0;margin-bottom:12px;}\n"
        "#doomnet-steamtunes-host .smp-nptab-title,#doomnet-steamtunes-host .smp-nptab-artist,#doomnet-steamtunes-host .smp-nptab-album{text-align:center;width:100%;}\n"
        "#doomnet-steamtunes-host .smp-nptab-title.smp-marquee-active,#doomnet-steamtunes-host .smp-nptab-artist.smp-marquee-active,#doomnet-steamtunes-host .smp-nptab-album.smp-marquee-active{text-align:left;}\n"
        "#doomnet-steamtunes-host .smp-search{flex:0 1 260px;width:260px;max-width:min(260px,46%);margin-left:auto;}\n"
        "#doomnet-steamtunes-host .smp-art-size{margin-left:auto;display:flex;align-items:flex-end;gap:6px;height:28px;padding:0 0 5px 10px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-btn{display:block;width:auto;height:auto;min-width:0;padding:0;margin:0;background:transparent !important;border:0;box-shadow:none;line-height:0;cursor:pointer;}\n"
        "#doomnet-steamtunes-host .smp-art-size-mark{display:block;box-sizing:border-box;border:1px solid rgba(255,255,255,0.72);background:transparent !important;border-radius:0;}\n"
        "#doomnet-steamtunes-host .smp-art-size-sm{width:10px;height:10px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-smd{width:12px;height:12px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-md{width:14px;height:14px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-mdl{width:16px;height:16px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-lg{width:18px;height:18px;}\n"
        "#doomnet-steamtunes-host .smp-art-size-btn.on .smp-art-size-mark{border-color:var(--smp-accent,#e35d5d);background:transparent !important;}\n"
        "#doomnet-steamtunes-host .smp-nptab-modes{display:flex;align-items:center;justify-content:center;width:100%;gap:6px;}\n"
        "#doomnet-steamtunes-host .smp-nptab-modes .smp-shuffle-btn{margin-right:8px;}\n"
        "#doomnet-steamtunes-host .smp-nptab-modes .smp-repeat-btn{margin-left:8px;}\n"
        "#doomnet-steamtunes-host .doomnet-np-cluster{display:flex;align-items:center;justify-content:center;gap:4px;}\n"
        "#doomnet-steamtunes-host .doomnet-np-seekline{display:flex;align-items:center;justify-content:center;width:100%;margin-top:10px;gap:10px;}\n"
        "#doomnet-steamtunes-host .doomnet-np-seekline .smp-seek{flex:0 1 220px;width:220px;min-width:0;max-width:220px;}\n"
        "#doomnet-steamtunes-host .doomnet-np-seekline .smp-volume{width:84px;max-width:84px;flex:0 0 84px;}\n"
        "#doomnet-steamtunes-host .smp-panel:has(.smp-nptab) > .smp-nowplaying{display:none !important;}\n"
        ".smp-panel:has(.smp-view-library.active) > .smp-scan-banner{display:none !important;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-row{position:absolute;inset:0;pointer-events:none;z-index:2;}\n"
        "#doomnet-steamtunes-host .doomnet-flank{display:flex;position:absolute;flex-direction:column;justify-content:center;color:var(--smp-text);font-size:11px;pointer-events:auto;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-eq{align-items:center;justify-content:center;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-meter{width:100%;margin-top:10px;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-row:not(.doomnet-flanks-inline) .doomnet-eq-meter{position:absolute;left:0;right:0;top:100%;margin-top:12px;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-row.doomnet-flanks-inline{position:relative;inset:auto;display:flex;justify-content:center;align-items:flex-end;gap:20px;width:min(640px,100%);margin:12px auto 0;pointer-events:auto;height:auto;}\n"
        "#doomnet-steamtunes-host .doomnet-flanks-inline .doomnet-flank{position:relative;left:auto;top:auto;width:auto;height:auto;}\n"
        "#doomnet-steamtunes-host .doomnet-flanks-inline .doomnet-flank-meter{width:min(220px,42%);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-preset-wrap{position:relative;width:100%;margin:0 0 12px;text-align:center;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-preset{display:inline-flex;align-items:center;justify-content:center;gap:6px;margin:0;padding:0;border:0;background:transparent;font:inherit;font-size:10px;font-weight:500;text-transform:uppercase;color:var(--smp-accent,#66c0f4);cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-preset-name{letter-spacing:0.18em;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-caret{width:0;height:0;border-left:4px solid transparent;border-right:4px solid transparent;border-top:5px solid var(--smp-accent,#66c0f4);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-menu{position:absolute;left:50%;top:100%;transform:translateX(-50%);z-index:5;min-width:148px;margin-top:4px;padding:4px;background:var(--smp-bg-elevated,#17181a);border:1px solid var(--smp-border,#2a2b2e);border-radius:6px;box-shadow:0 8px 24px rgba(0,0,0,0.45);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-menu[hidden]{display:none;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-menu button{display:block;width:100%;box-sizing:border-box;text-align:left;background:transparent;border:0;color:var(--smp-text,#d9dadd);font:inherit;font-size:11px;letter-spacing:0.04em;text-transform:none;padding:6px 8px;cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-menu button.on{color:var(--smp-accent,#66c0f4);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-menu button:hover{background:rgba(255,255,255,0.06);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-bank{display:flex;align-items:flex-end;justify-content:space-between;width:100%;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-col{display:flex;flex-direction:column;align-items:center;gap:6px;width:var(--doomnet-col,22px);flex:0 0 var(--doomnet-col,22px);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-slot{position:relative;width:16px;height:var(--doomnet-fader,120px);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-slot::before{content:\"\";position:absolute;left:-3px;right:-3px;top:50%;height:1px;background:rgba(255,255,255,0.22);pointer-events:none;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-hz{width:auto;flex:0 0 auto;color:var(--smp-text-dim);font-size:9px;letter-spacing:0.02em;text-align:center;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-slider{-webkit-appearance:none;appearance:none;position:absolute;width:var(--doomnet-fader,120px);height:16px;left:50%;top:50%;margin:0;background:transparent;outline:none;cursor:pointer;transform:translate(-50%,-50%) rotate(-90deg);}\n"
        "#doomnet-steamtunes-host .doomnet-eq-slider::-webkit-slider-runnable-track{height:2px;border:0;border-radius:1px;background:var(--doomnet-eq-fill, rgba(255,255,255,0.16));}\n"
        "#doomnet-steamtunes-host .doomnet-eq-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;margin-top:-3px;border:0;border-radius:50%;background:var(--smp-accent,#66c0f4);box-shadow:0 0 0 2px rgba(0,0,0,0.45);cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-eq-db{width:auto;flex:0 0 auto;text-align:center;color:var(--smp-text-dim);font-size:9px;font-variant-numeric:tabular-nums;line-height:1;}\n"
        "#doomnet-steamtunes-host .doomnet-spectrum{display:block;width:100%;height:72px;background:transparent;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-lyrics{justify-content:flex-start;box-sizing:border-box;}\n"
        "#doomnet-steamtunes-host .doomnet-lyrics-label{flex:0 0 auto;width:100%;margin:0 0 12px;text-align:center;font-size:10px;font-weight:500;letter-spacing:0.18em;text-transform:uppercase;color:var(--smp-accent,#66c0f4);}\n"
        "#doomnet-steamtunes-host .doomnet-lyrics{flex:1 1 auto;min-height:0;height:auto;overflow:hidden;-webkit-mask-image:linear-gradient(to bottom,transparent 0%,#000 16%,#000 84%,transparent 100%);mask-image:linear-gradient(to bottom,transparent 0%,#000 16%,#000 84%,transparent 100%);}\n"
        "#doomnet-steamtunes-host .doomnet-lyrics-body{will-change:transform;text-align:center;font-size:13px;line-height:1.45;color:rgba(217,218,221,0.72);white-space:pre-wrap;}\n"
        "#doomnet-steamtunes-host .doomnet-lyric-line{min-height:1.45em;padding:2px 8px;}\n"
        "#doomnet-steamtunes-host .doomnet-lyric-line.on{color:var(--smp-text,#fff);}\n"
        "#doomnet-steamtunes-host .doomnet-loudness{display:flex;align-items:center;gap:8px;margin-top:8px;}\n"
        "#doomnet-steamtunes-host .doomnet-loudness-track{flex:1 1 auto;height:6px;background:rgba(255,255,255,0.12);border-radius:3px;overflow:hidden;}\n"
        "#doomnet-steamtunes-host .doomnet-loudness-fill{height:100%;width:0;background:var(--smp-accent,#e35d5d);}\n"
        "#doomnet-steamtunes-host .doomnet-loudness-db{width:58px;flex:0 0 58px;text-align:right;color:var(--smp-text-dim);}\n"
        "#doomnet-steamtunes-host .doomnet-mix{width:100%;max-width:100%;min-width:0;margin-top:calc(10px + 2.4em);}\n"
        "#doomnet-steamtunes-host .doomnet-mix-row{min-width:0;margin-top:8px;}\n"
        "#doomnet-steamtunes-host .doomnet-mix-label{display:block;margin:0 66px 2px 0;text-align:center;color:var(--smp-accent,#66c0f4);font-size:10px;font-weight:500;letter-spacing:0.04em;white-space:nowrap;}\n"
        "#doomnet-steamtunes-host .doomnet-mix-line{display:flex;align-items:center;gap:8px;min-width:0;}\n"
        "#doomnet-steamtunes-host .doomnet-mix-value{width:58px;flex:0 0 58px;color:var(--smp-text-dim);font-size:10px;text-align:right;white-space:nowrap;}\n"
        "#doomnet-steamtunes-host .doomnet-mix-slider{-webkit-appearance:none;appearance:none;flex:1 1 0;width:auto;min-width:0;max-width:100%;height:16px;margin:0;background:transparent;outline:none;cursor:pointer;accent-color:var(--smp-accent,#66c0f4);}\n"
        "#doomnet-steamtunes-host .doomnet-mix-slider::-webkit-slider-runnable-track{height:4px;border:0;border-radius:2px;background:var(--doomnet-mix-fill, rgba(255,255,255,0.16));}\n"
        "#doomnet-steamtunes-host .doomnet-mix-slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:10px;height:10px;margin-top:-3px;border:0;border-radius:50%;background:var(--smp-accent,#66c0f4);box-shadow:0 0 0 2px rgba(0,0,0,0.45);cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-eye{position:absolute;top:8px;left:8px;z-index:5;display:flex;align-items:center;justify-content:center;width:28px;height:28px;margin:0;padding:0;border:0;background:transparent;color:var(--smp-accent,#66c0f4);cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-flank-eye svg{display:block;}\n"
        "#doomnet-steamtunes-host .smp-nptab.doomnet-flanks-off .doomnet-flank-row{display:none;}\n"
        "@media (min-width:1920px) and (min-height:1000px){"
        "#doomnet-steamtunes-host .smp-nptab-hero{flex-direction:column;align-items:center;justify-content:center;gap:22px;width:min(480px,92%);margin:0 auto;}"
        "#doomnet-steamtunes-host .smp-nptab-art-wrap{width:min(440px,100%);max-width:440px;flex:0 0 auto;margin:0;transform:none;}"
        "#doomnet-steamtunes-host .smp-nptab-info{align-items:center;text-align:center;width:100%;max-width:440px;flex:0 0 auto;padding:0;margin:0;transform:none;}"
        "}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-hero{flex-direction:row;align-items:center;justify-content:center;gap:28px;width:max-content;max-width:100%;margin-left:auto;margin-right:auto;}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-art-wrap{width:min(280px,46vw);max-width:300px;}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-info{width:max-content;flex:0 0 auto;max-width:460px;}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .doomnet-np-cluster,#doomnet-steamtunes-host.doomnet-np-horizontal .doomnet-np-seekline{display:none !important;}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-modes .smp-shuffle-btn,#doomnet-steamtunes-host.doomnet-np-horizontal .smp-nptab-modes .smp-repeat-btn{margin:0;}\n"
        "#doomnet-steamtunes-host.doomnet-np-horizontal .smp-panel:has(.smp-nptab) > .smp-nowplaying{display:flex !important;}\n"
        "#doomnet-steamtunes-host .smp-view-library{position:relative;}\n"
        "#doomnet-steamtunes-host .smp-view-library:has(.smp-nptab) > .doomnet-np-orient{position:absolute;right:16px;bottom:16px;z-index:8;display:flex;align-items:center;justify-content:center;width:36px;height:36px;margin:0;padding:0;border:0;background:transparent;color:var(--smp-accent,#66c0f4);cursor:pointer;}\n"
        "#doomnet-steamtunes-host .doomnet-np-orient{display:none;}\n"
        "#doomnet-steamtunes-host .doomnet-np-orient svg{display:block;}\n"
        "#doomnet-steamtunes-host .smp-nptab-queue{width:min(740px,100%);max-width:none;margin:28px auto 0;}\n"
        "#doomnet-steamtunes-host .smp-nowplaying{justify-content:center;flex-wrap:wrap;gap:12px 16px;padding:10px 24px;}\n"
        "#doomnet-steamtunes-host .smp-nowplaying-info{flex:0 1 240px;max-width:240px;}\n"
        "#doomnet-steamtunes-host .smp-nowplaying-controls{flex:0 0 auto;}\n"
        "#doomnet-steamtunes-host .smp-seek{flex:0 1 360px;width:360px;max-width:360px;}\n"
        "#doomnet-steamtunes-host .smp-volume{width:96px;max-width:96px;}\n"
        ".smp-eq-bank-scroll{overflow:visible;padding:0;}\n"
        ".smp-eq-fader-bank{display:grid;grid-template-columns:repeat(10,32px);gap:8px;width:max-content;max-width:100%;min-width:0;padding:4px 0 0;background:transparent;border:0;border-radius:0;}\n"
        ".smp-eq-fader-readout{height:12px;color:var(--smp-text-dim);font-size:9px;font-weight:400;font-variant-numeric:tabular-nums;line-height:1;}\n"
        ".smp-eq-fader-track{position:relative;width:16px;height:168px;}\n"
        ".smp-eq-fader-track::before{content:\"\";position:absolute;left:-3px;right:-3px;top:50%;height:1px;background:rgba(255,255,255,0.22);pointer-events:none;}\n"
        ".smp-eq-fader-input{-webkit-appearance:none;appearance:none;position:absolute;width:168px;height:16px;left:50%;top:50%;margin:0;padding:0;background:transparent;outline:none;cursor:pointer;transform:translate(-50%,-50%) rotate(-90deg);}\n"
        ".smp-eq-fader-input::-webkit-slider-runnable-track{height:2px;border:0;border-radius:1px;background:var(--doomnet-eq-fill, rgba(255,255,255,0.16));}\n"
        ".smp-eq-fader-input::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:8px;height:8px;margin-top:-3px;border:0;border-radius:50%;background:var(--smp-accent,#66c0f4);box-shadow:0 0 0 2px rgba(0,0,0,0.45);cursor:pointer;}\n"
        ".smp-eq-fader-label{margin-top:6px;color:var(--smp-text-dim);font-size:9px;font-weight:400;letter-spacing:0.02em;}\n"
        ".smp-settings-slider{flex:0 1 240px;width:240px;max-width:240px;accent-color:var(--smp-accent);}\n"
        ".smp-settings-readout{width:36ch;min-width:36ch;max-width:36ch;flex:0 0 36ch;overflow:hidden;white-space:nowrap;text-align:left;}\n"
        "html.smp-ctx-overlay .smp-setting-block .smp-settings-row{flex-wrap:nowrap;}\n"
        "html.smp-ctx-overlay .smp-setting-block .smp-settings-label{width:auto;flex:0 0 auto;}\n"
        "html.smp-ctx-overlay .smp-settings-slider{flex:1 1 auto;width:auto;min-width:0;max-width:none;}\n"
        ".smp-select{flex:0 1 360px;width:360px;max-width:min(360px,100%);}\n"
        ".doomnet-playlist-menu{position:fixed;z-index:2147483000;min-width:180px;max-width:280px;max-height:240px;overflow:auto;margin:0;padding:4px;background:#1b1c1f;border:1px solid rgba(255,255,255,0.14);border-radius:6px;box-shadow:0 10px 28px rgba(0,0,0,0.55);}\n"
        ".doomnet-playlist-menu-item,.doomnet-playlist-menu-empty{display:block;width:100%;box-sizing:border-box;margin:0;padding:7px 10px;border:0;border-radius:4px;background:transparent;box-shadow:none;color:#d9dadd;font:inherit;font-size:13px;line-height:1.3;text-align:left;cursor:pointer;}\n"
        ".doomnet-playlist-menu-item.added,.doomnet-playlist-menu-empty{color:#8f98a0;cursor:default;}\n"
        ".doomnet-confirm-back{position:fixed;inset:0;z-index:2147483001;background:rgba(0,0,0,0.45);display:flex;align-items:center;justify-content:center;}\n"
        ".doomnet-confirm{width:min(420px,calc(100vw - 32px));margin:0;padding:16px;background:#1b1c1f;border:1px solid rgba(255,255,255,0.14);border-radius:8px;box-shadow:0 10px 28px rgba(0,0,0,0.55);color:#d9dadd;}\n"
        ".doomnet-confirm p{margin:0 0 14px;font-size:14px;line-height:1.4;}\n"
        ".doomnet-confirm-actions{display:flex;justify-content:flex-end;gap:8px;}\n"
        ".doomnet-playlist-menu-item:hover{background:rgba(255,255,255,0.08);}\n"
        ".doomnet-playlist-menu-item.added:hover{background:transparent;}\n"
        ".doomnet-playlist-row[draggable=\"true\"]{cursor:grab;}\n"
        ".doomnet-playlist-row.dragging{cursor:grabbing;}\n"
        "button.smp-playlist-name{background:transparent;border:0;color:inherit;font:inherit;text-align:left;cursor:pointer;padding:0;}\n";
    return css;
}

std::string store_player_script() {
    static std::string cached;
    if (!cached.empty()) {
        return cached;
    }
    const std::wstring dir = L"D:\\Documents\\Repos\\SteamTunes\\backend\\assets\\frontend";
    const std::string css = patched_player_css_text();
    const std::string mix = read_skin_text(dir, "mix-image.js");
    const std::string engine = read_skin_text(dir, "audio-engine.js");
    const std::string ui = patched_player_ui_text();
    if (css.empty() || engine.empty() || ui.empty()) {
        log_event(L"warn", L"inject", L"The store player files could not be read from the SteamTunes frontend folder, so the store page was not given a player.");
        return {};
    }
    std::string js = "(function(){var href=String(location.href||'');var at=href.indexOf('store.steampowered.com');if(at<0)return 'skip';var path=href.slice(at+'store.steampowered.com'.length);if(path!==''&&path!=='/'&&path.charAt(1)!=='?'&&path.charAt(1)!=='#')return 'skip';if(window.__steamMusicPlayerLoaded)return window.__steamMusicPlayerLoaded?'loaded':'script-only';var head=document.head||document.documentElement;if(!head)return 'nohead';";
    js += "if(!document.getElementById('smp-plugin-css')){var c=document.createElement('style');c.id='smp-plugin-css';c.textContent=" + json_quote(css) + ";head.appendChild(c);}";
    auto add_script = [&](const char* id, const std::string& code) {
        if (code.empty()) {
            return;
        }
        js += "if(!document.getElementById(" + json_quote(id) + ")){var s=document.createElement('script');s.id=" + json_quote(id) + ";s.textContent=" + json_quote(code) + ";head.appendChild(s);}";
    };
    add_script("smp-plugin-mix-image", mix);
    add_script("smp-plugin-engine", engine);
    add_script("smp-plugin-ui", ui);
    js += "return window.__steamMusicPlayerLoaded?'loaded':'script-only';})()";
    cached = std::move(js);
    return cached;
}

std::string skin_style_script(const std::string& url) {
    if (url.find("checkout.steampowered.com") != std::string::npos) {
        return {};
    }
    std::string js = "(function(){var head=document.head||document.documentElement;if(!head)return;";
    std::vector<std::string> seen;
    int index = 0;
    bool any = false;
    for (const SkinTag& tag : g_tags) {
        bool matched = false;
        try {
            matched = std::regex_search(url, std::regex(tag.match, std::regex::ECMAScript));
        } catch (const std::regex_error&) {
            matched = false;
        }
        if (!matched || tag.css_body.empty() || tag.css_url.empty()) {
            continue;
        }
        bool duplicate = false;
        for (const std::string& prior : seen) {
            if (prior == tag.css_url) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        seen.push_back(tag.css_url);
        const std::string id = "doomnet-css-" + std::to_string(index++);
        js += "if(!document.getElementById(" + json_quote(id) + ")){var s=document.createElement('style');s.id=" + json_quote(id) + ";s.textContent=" + json_quote(tag.css_body) + ";head.appendChild(s);}";
        any = true;
    }
    if (!any) {
        return {};
    }
    js += "})()";
    return js;
}

std::string inject_skin_into_html(const std::string& html, const std::string& tags) {
    if (tags.empty()) {
        return html;
    }
    const size_t head = find_ci(html, "<head");
    if (head != std::string::npos) {
        const size_t end = html.find('>', head);
        if (end != std::string::npos) {
            return html.substr(0, end + 1) + tags + html.substr(end + 1);
        }
    }
    const size_t html_tag = find_ci(html, "<html");
    if (html_tag != std::string::npos) {
        const size_t end = html.find('>', html_tag);
        if (end != std::string::npos) {
            return html.substr(0, end + 1) + tags + html.substr(end + 1);
        }
    }
    return tags + html;
}

}  // namespace doomnet
