// One shot against the installed Steam client. Stashes Millennium, copies
// DoomNet's wsock32.dll beside steam.exe, starts that client, and injects the
// boot script. On failure it stops the client it started and restores the stash.
// A successful run leaves Steam open and Millennium stashed.

#include "pipe_session.h"
#include "browser_thread.h"
#include "host_config.h"
#include "page_bridge.h"
#include "skin_page.h"
#include "state_thread.h"
#include "steam_attach.h"
#include "steam_discover.h"
#include "log.h"
#include "text.h"
#include "toggle.h"
#include "wide_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

void print_line(const std::wstring& line) {
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (console != nullptr && console != INVALID_HANDLE_VALUE && WriteConsoleW(console, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        WriteConsoleW(console, L"\r\n", 2, &written, nullptr);
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
        utf8.append("\r\n");
        fwrite(utf8.data(), 1, utf8.size(), stdout);
        fflush(stdout);
    }
}

std::wstring sibling_bootstrap() {
    wchar_t self[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring path(self, n);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash) + L"\\wsock32.dll";
}

bool ensure_dir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    return false;
}

bool copy_tree(const std::wstring& from, const std::wstring& to, int& count, std::wstring& error) {
    if (!ensure_dir(to)) {
        error = L"Could not create '" + to + L"'. " + doomnet::windows_error_text(GetLastError());
        return false;
    }
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW((from + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        error = L"Could not read '" + from + L"'. " + doomnet::windows_error_text(GetLastError());
        return false;
    }
    bool ok = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring src = from + L"\\" + data.cFileName;
        const std::wstring dst = to + L"\\" + data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!copy_tree(src, dst, count, error)) {
                ok = false;
                break;
            }
        } else if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            error = L"Could not copy '" + src + L"' to '" + dst + L"'. " + doomnet::windows_error_text(GetLastError());
            ok = false;
            break;
        } else {
            count++;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
}

bool remove_tree(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return RemoveDirectoryW(path.c_str()) == TRUE;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return DeleteFileW(path.c_str()) == TRUE;
    }
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            if (!remove_tree(path + L"\\" + data.cFileName)) {
                FindClose(find);
                return false;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return RemoveDirectoryW(path.c_str()) == TRUE;
}

bool is_reparse_point(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool is_real_directory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::wstring first_music_folder(const std::wstring& json_path) {
    std::string bytes;
    std::wstring error;
    if (!doomnet::read_entire_file(json_path, bytes, error)) {
        return {};
    }
    const size_t open = bytes.find('"');
    if (open == std::string::npos) {
        return {};
    }
    std::string path;
    for (size_t i = open + 1; i < bytes.size(); ++i) {
        const char ch = bytes[i];
        if (ch == '\\' && i + 1 < bytes.size()) {
            const char next = bytes[++i];
            path.push_back(next == '/' ? '\\' : next);
        } else if (ch == '"') {
            break;
        } else {
            path.push_back(ch);
        }
    }
    if (path.empty()) {
        return {};
    }
    return doomnet::utf8_to_wide_lossy(path);
}

int run_hidden_command(const std::wstring& command, std::wstring& error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = doomnet::windows_error_text(GetLastError());
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    error.clear();
    return static_cast<int>(code);
}

bool move_directory(const std::wstring& from, const std::wstring& to, std::wstring& error) {
    if (!is_real_directory(from)) {
        error.clear();
        return true;
    }
    ensure_dir(to);
    const std::wstring command = L"robocopy \"" + from + L"\" \"" + to + L"\" /E /MOVE /R:1 /W:1 /NFL /NDL /NJH /NJS /MT:8";
    const int code = run_hidden_command(command, error);
    if (code < 0) {
        return false;
    }
    // Robocopy uses 0 through 7 for success, including "extra files" and "mismatched".
    if (code >= 8) {
        error = L"Could not move '" + from + L"' to '" + to + L"'. robocopy exited " + std::to_wstring(code) + L".";
        return false;
    }
    RemoveDirectoryW(from.c_str());
    error.clear();
    return true;
}

bool junction_directory(const std::wstring& link, const std::wstring& target, std::wstring& error) {
    const DWORD target_attrs = GetFileAttributesW(target.c_str());
    if (target_attrs == INVALID_FILE_ATTRIBUTES || (target_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = L"The art cache '" + target + L"' is not a folder, so it was not linked.";
        return false;
    }
    const DWORD link_attrs = GetFileAttributesW(link.c_str());
    if (link_attrs != INVALID_FILE_ATTRIBUTES) {
        if ((link_attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error.clear();
            return true;
        }
        if (!remove_tree(link)) {
            error = L"Could not replace '" + link + L"' with a link to the Millennium art cache. " + doomnet::windows_error_text(GetLastError());
            return false;
        }
    }
    std::wstring command = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = L"Could not link '" + link + L"' to '" + target + L"'. " + doomnet::windows_error_text(GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0 || GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Could not link '" + link + L"' to the Millennium art cache '" + target + L"'.";
        return false;
    }
    error.clear();
    return true;
}

int copy_named_files(const std::wstring& from_dir, const std::wstring& to_dir, const wchar_t* const* names, int name_count, bool overwrite) {
    ensure_dir(to_dir);
    int copied = 0;
    for (int i = 0; i < name_count; ++i) {
        const std::wstring src = from_dir + L"\\" + names[i];
        const std::wstring dst = to_dir + L"\\" + names[i];
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (!overwrite && GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            copied++;
        }
    }
    return copied;
}

void wait_until_steam_exits() {
    for (int i = 0; i < 100; ++i) {
        std::wstring which;
        if (!doomnet::steam_is_running(which)) {
            return;
        }
        Sleep(100);
    }
}

}  // namespace

int wmain() {
    std::wstring steam;
    if (!doomnet::discover_steam_path(steam)) {
        print_line(L"Steam was not found, so the live proof did not start.");
        return 1;
    }
    std::wstring which;
    if (doomnet::steam_is_running(which)) {
        print_line(L"'" + which + L"' is still running. Quit Steam fully, then run the live proof again.");
        return 1;
    }

    const std::wstring dll = sibling_bootstrap();
    print_line(L"Stashing Millennium beside '" + steam + L"'.");
    const doomnet::ToggleResult off = doomnet::millennium_off(steam, true);
    print_line(off.detail);
    if (!off.ok) {
        return 1;
    }

    print_line(L"Copying '" + dll + L"' beside steam.exe.");
    const doomnet::PlaceBootstrapResult placed = doomnet::place_doomnet_bootstrap(steam, dll, true);
    print_line(placed.detail);
    if (!placed.ok) {
        const doomnet::ToggleResult back = doomnet::millennium_on(steam, true);
        print_line(back.detail);
        return 1;
    }

    const std::wstring skin_dir = L"D:\\Documents\\Repos\\SteamSkin";
    print_line(L"Copying skin files from '" + skin_dir + L"'.");
    const doomnet::SkinFileCopy copied = doomnet::copy_skin_files(skin_dir, steam);
    print_line(copied.detail);
    if (!copied.ok) {
        const doomnet::ToggleResult back = doomnet::millennium_on(steam, true);
        print_line(back.detail);
        return 1;
    }
    const doomnet::SkinPreamble preamble = doomnet::load_skin_preamble(skin_dir + L"\\skin.json", true);
    print_line(preamble.detail);
    if (!preamble.ok) {
        const doomnet::ToggleResult back = doomnet::millennium_on(steam, true);
        print_line(back.detail);
        return 1;
    }
    doomnet::set_boot_preamble(preamble.javascript);

    const wchar_t* asset_names[] = {L"bootstrap.js", L"steam-music-player.css", L"audio-engine.js", L"steam-music-player.js", L"mix-image.js"};
    const std::wstring asset_src = L"D:\\Documents\\Repos\\SteamTunes\\backend\\assets\\frontend";
    const std::wstring asset_dest = steam + L"\\steamui\\steam-music-player";
    const int assets = copy_named_files(asset_src, asset_dest, asset_names, 5, true);
    const std::wstring asset_detail = L"Copied " + std::to_wstring(assets) + L" player file(s) into '" + asset_dest + L"'.";
    doomnet::log_event(L"info", L"payload", asset_detail.c_str());
    print_line(asset_detail);

    wchar_t local_app[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
    const std::wstring install = std::wstring(local_app) + L"\\DoomNet";
    const std::wstring backend = install + L"\\player\\backend";
    ensure_dir(install);
    ensure_dir(install + L"\\player");
    int backend_files = 0;
    std::wstring copy_error;
    print_line(L"Copying the player backend.");
    if (!copy_tree(L"D:\\Documents\\Repos\\SteamTunes\\backend", backend, backend_files, copy_error)) {
        print_line(copy_error);
        const doomnet::ToggleResult back = doomnet::millennium_on(steam, true);
        print_line(back.detail);
        return 1;
    }
    print_line(L"The player backend copy finished.");
    const wchar_t* state_names[] = {L"library_index.json", L"library_snapshot.json", L"settings.json", L"player_state.json", L"music_folders.json", L"loudness.json", L"media_token"};
    const std::wstring state_src = steam + L"\\millennium\\plugins\\SteamMusicPlayer\\backend\\data";
    const int state_files = copy_named_files(state_src, backend + L"\\data", state_names, 8, false);
    std::wstring state_detail = L"Copied " + std::to_wstring(backend_files) + L" backend file(s) to '" + backend + L"' and " + std::to_wstring(state_files) + L" library file(s) from '" + state_src + L"'.";
    int linked_caches = 0;
    print_line(L"Reading the music folder list.");
    const std::wstring music_folder = first_music_folder(backend + L"\\data\\music_folders.json");
    print_line(music_folder.empty() ? std::wstring(L"The music folder list was empty.") : (L"The first music folder is '" + music_folder + L"'."));
    std::wstring cache_root;
    if (!music_folder.empty() && is_real_directory(music_folder)) {
        cache_root = music_folder + L"\\DoomNet Art";
        ensure_dir(cache_root);
        doomnet::log_event(L"info", L"payload", (L"The art cache will live in the first music folder at '" + cache_root + L"'.").c_str());
        print_line(L"The art cache will live in the first music folder at '" + cache_root + L"'.");
    } else {
        doomnet::log_event(L"warn", L"payload", L"No music folder was listed, so the art cache stays linked to the Millennium plugin folder.");
    }
    const wchar_t* cache_names[] = {L"art", L"art_jpeg"};
    for (const wchar_t* cache_name : cache_names) {
        std::wstring link_error;
        const std::wstring link = backend + L"\\data\\" + cache_name;
        const std::wstring millennium_cache = state_src + L"\\" + cache_name;
        std::wstring target = millennium_cache;
        if (!cache_root.empty()) {
            const std::wstring moved = cache_root + L"\\" + cache_name;
            if (is_real_directory(millennium_cache)) {
                print_line(L"Moving '" + millennium_cache + L"' to '" + moved + L"'.");
                if (!move_directory(millennium_cache, moved, link_error)) {
                    doomnet::log_event(L"warn", L"payload", link_error.c_str());
                    print_line(link_error);
                } else {
                    doomnet::log_event(L"info", L"payload", (L"Moved the art cache '" + millennium_cache + L"' to '" + moved + L"'.").c_str());
                }
            }
            if (is_real_directory(moved) || is_reparse_point(moved)) {
                target = moved;
            }
        }
        if (is_reparse_point(link) && !remove_tree(link)) {
            link_error = L"Could not retarget the art link '" + link + L"'. " + doomnet::windows_error_text(GetLastError());
            doomnet::log_event(L"warn", L"payload", link_error.c_str());
            print_line(link_error);
            continue;
        }
        if (junction_directory(link, target, link_error)) {
            ++linked_caches;
            doomnet::log_event(L"info", L"payload", (L"Linked '" + link + L"' to the art cache '" + target + L"'.").c_str());
        } else if (!link_error.empty()) {
            doomnet::log_event(L"warn", L"payload", link_error.c_str());
            print_line(link_error);
        }
    }
    state_detail += L" Linked " + std::to_wstring(linked_caches) + L" Millennium art cache folder(s).";
    doomnet::log_event(L"info", L"payload", state_detail.c_str());
    print_line(state_detail);

    print_line(L"Starting '" + steam + L"\\steam.exe'.");
    doomnet::InjectResult injected = doomnet::launch_and_inject_boot(steam + L"\\steam.exe", L"", true);
    print_line(injected.detail);
    if (!injected.ok) {
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        wait_until_steam_exits();
        const doomnet::ToggleResult back = doomnet::millennium_on(steam, true);
        print_line(back.detail);
        return 1;
    }

    doomnet::HostConfig host;
    host.backend_dir = backend;
    host.steam_path = steam;
    host.install_path = install;
    host.post_load_lua =
        "function doomnet_probe()\n  return 'live-ok'\nend\n"
        "function try_load_compact_snapshot()\nend\n"
        "function get_album_art_b64(key)\n"
        "  local raw = tostring(key or \"\")\n"
        "  local kind, safe = raw:match(\"^(album|track):(.+)$\")\n"
        "  if not kind then kind = \"album\" safe = raw:gsub(\"[^%w_]\", \"\") end\n"
        "  if safe == \"\" then return \"\" end\n"
        "  local fs = require(\"fs\")\n"
        "  local utils = require(\"utils\")\n"
        "  local data = fs.join(utils.get_backend_path(), \"data\")\n"
        "  if kind == \"track\" then\n"
        "    local cache = fs.join(data, \"art\", safe .. \".json\")\n"
        "    if not fs.exists(cache) then return \"\" end\n"
        "    local content = utils.read_file(cache)\n"
        "    if not content then return \"\" end\n"
        "    local b64 = content:match('\"base64\"%s*:%s*\"(.-)\"')\n"
        "    if not b64 or #b64 < 32 then return \"\" end\n"
        "    return (b64:gsub(\"\\\\/\", \"/\"))\n"
        "  end\n"
        "  local bases = {\n"
        "    fs.join(data, \"art_jpeg\", \"albums\", safe),\n"
        "    fs.join(require(\"millennium\").steam_path(), \"steamui\", \"steam-music-player\", \"art\", \"albums\", safe),\n"
        "  }\n"
        "  for _, base in ipairs(bases) do\n"
        "    local path = base .. \".jpg\"\n"
        "    if not fs.exists(path) then path = base .. \".png\" end\n"
        "    if fs.exists(path) then\n"
        "      local content = utils.read_file(path)\n"
        "      if content and #content >= 32 and #content <= 1500000 then\n"
        "        return utils.base64_encode(content)\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "  return \"\"\n"
        "end\n";
    doomnet::log_event(L"info", L"lua", L"A no-op try_load_compact_snapshot was installed so the player's startup can finish. The real function is local and is not in scope at the call.");
    doomnet::log_event(L"info", L"lua", L"get_album_art_b64 reads covers from the Millennium art cache, then from the steamui art folder.");
    const std::string patched_ui = doomnet::patched_player_ui_text();
    if (!patched_ui.empty()) {
        std::wstring write_error;
        const std::wstring ui_path = backend + L"\\assets\\frontend\\steam-music-player.js";
        if (!doomnet::write_new_file(ui_path, patched_ui.data(), patched_ui.size(), write_error)) {
            print_line(write_error);
        }
    }
    doomnet::Bus bus;
    doomnet::BrowserThread browser(bus);
    std::wstring error;
    if (!browser.start(error)) {
        print_line(error);
        doomnet::present_client_windows();
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        return 1;
    }
    doomnet::StateThread state(bus, std::move(host));
    if (!state.start(error) || !state.wait_loaded(120000)) {
        print_line(error.empty() ? std::wstring(L"The Lua host did not finish loading.") : error);
        doomnet::present_client_windows();
        browser.stop();
        state.stop();
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        return 1;
    }

    doomnet::SteamPage page;
    if (!page.start(std::move(injected.pipe), injected.session_id, browser, bus, error)) {
        print_line(error);
        doomnet::present_client_windows();
        browser.stop();
        state.stop();
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        return 1;
    }

    std::string value;
    const char* starter = "window.__doomnetOut='wait'; window.Millennium.callServerMethod('SteamMusicPlayer','doomnet_probe',[]).then(function(v){window.__doomnetOut='ok:'+v;},function(e){window.__doomnetOut='bad:'+(e&&e.message?e.message:e);}); 'started'";
    bool proved = false;
    if (page.evaluate(starter, false, value, error) && value == "started") {
        for (int i = 0; i < 40 && !proved; ++i) {
            std::string out;
            if (page.evaluate("window.__doomnetOut", false, out, error) && out == "ok:live-ok") {
                proved = true;
            } else {
                Sleep(100);
            }
        }
    }
    if (!proved) {
        print_line(error.empty() ? std::wstring(L"The page call did not resolve to live-ok.") : error);
        doomnet::present_client_windows();
        page.stop();
        browser.stop();
        state.stop();
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        return 1;
    }
    print_line(L"The page called doomnet_probe and the host answered live-ok. Steam stays open and this process keeps answering calls.");
    const char* skin_probe = "(function(){function list(v){var o=[],n,s,it;if(!v)return o;if(typeof v.length==='number'){for(n=0;n<v.length&&n<200;n++)o.push(v[n]);return o;}try{it=v[Symbol.iterator]();while(!(s=it.next()).done&&o.length<200)o.push(s.value);}catch(e){}return o;}var raw=null;try{raw=window.g_PopupManager&&g_PopupManager.GetPopups&&g_PopupManager.GetPopups();}catch(e){}var pops=list(raw);var i,hit='';for(i=0;i<pops.length;i++){var popup=pops[i];var win=popup&&(popup.window||popup.m_popup);var doc=null;try{doc=win&&win.document;}catch(e){}if(!doc)continue;var title='';try{title=String(popup.m_strTitle||popup.title||doc.title||'');}catch(e){}var link=null;try{link=doc.querySelector('link[href*=\"doomnet-skin/libraryroot.custom.css\"]');}catch(e){}var color='';try{color=win.getComputedStyle(doc.documentElement).getPropertyValue('--steam-neutral-bg-start');}catch(e){}var ready=false;try{ready=!!win.__steamSkinClientNeutralize;if(doc.querySelector('script[src*=\"game-page.js\"]'))ready=ready&&!!win.__steamSkinPlayBarFix;}catch(e){ready=false;}if((title==='Steam'||link)&&ready&&win.__doomnetLibraryReady){hit+=title+'|'+(link?'link':'nolink')+'|'+(color||'').replace(/^\\s+|\\s+$/g,'')+'|'+(win.__doomnetLibraryReady||'')+'|'+((doc.querySelector&&doc.querySelector('script[src*=\"steam-music-player/bootstrap.js\"]'))?'player':'noplayer')+';';}}if(hit)return hit;return 'none|popups='+pops.length;})()";
    bool skinned = false;
    std::string skin_value;
    const DWORD skin_start = GetTickCount();
    while (GetTickCount() - skin_start < 35000 && !skinned) {
        if (page.evaluate(skin_probe, false, skin_value, error) && skin_value.find("|link|") != std::string::npos && skin_value.find("28, 28, 28") != std::string::npos) {
            skinned = true;
        } else {
            Sleep(250);
        }
    }
    if (!skinned) {
        print_line(skin_value.empty() ? (error.empty() ? std::wstring(L"The Steam page did not pick up the skin stylesheet.") : error) : (L"The skin probe returned '" + std::wstring(skin_value.begin(), skin_value.end()) + L"'."));
        doomnet::present_client_windows();
        page.stop();
        browser.stop();
        state.stop();
        if (injected.process != nullptr) {
            CloseHandle(static_cast<HANDLE>(injected.process));
        }
        return 1;
    }
    print_line(L"The Steam page loaded libraryroot.custom.css and --steam-neutral-bg-start is rgb(28, 28, 28).");
    doomnet::log_event(L"info", L"cdp", L"The library debugger stays attached so player clicks can be answered.");
    if (skin_value.find("|shown") != std::string::npos) {
        doomnet::log_event(L"info", L"inject", L"The library grid was opened while Steam was hidden.");
        print_line(L"The library grid was opened while Steam was hidden.");
    } else {
        doomnet::log_event(L"warn", L"inject", L"The library preload did not find the Library button, so the window was shown without it.");
        print_line(L"The library preload did not find the Library button, so the window was shown without it.");
    }
    if (skin_value.find("|player;") != std::string::npos) {
        doomnet::log_event(L"info", L"inject", L"The music player bootstrap is in the Steam window.");
        print_line(L"The music player bootstrap is in the Steam window.");
    } else {
        doomnet::log_event(L"warn", L"inject", L"The music player bootstrap was not in the Steam window when it was shown.");
        print_line(L"The music player bootstrap was not in the Steam window when it was shown.");
    }
    const DWORD store_start = GetTickCount();
    while (GetTickCount() - store_start < 8000 && doomnet::patched_web_documents() == 0) {
        Sleep(100);
    }
    if (doomnet::patched_web_documents() == 0) {
        print_line(L"The store document was not seen before the window was shown.");
    }
    doomnet::present_client_windows();
    int shown = 0;
    for (int i = 0; i < 20 && shown == 0; ++i) {
        shown = doomnet::show_client_windows(injected.pid);
        if (shown == 0) {
            Sleep(1000);
        }
    }
    if (shown == 0) {
        print_line(L"The Steam window titled Steam did not appear.");
    } else {
        print_line(L"Brought " + std::to_wstring(shown) + L" Steam window(s) titled Steam to the desktop.");
    }
    print_line(L"Stop it by signaling Local\\DoomNetStop or by creating the stop.flag in the DoomNet data folder.");

    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\DoomNetStop");
    if (stop != nullptr) {
        ResetEvent(stop);
    }
    for (;;) {
        if (stop != nullptr && WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) {
            break;
        }
        if (stop == nullptr) {
            Sleep(250);
        }
        wchar_t root[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            const std::wstring flag = std::wstring(root) + L"\\DoomNet\\stop.flag";
            const DWORD attrs = GetFileAttributesW(flag.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                break;
            }
        }
    }
    print_line(L"The host is leaving. Steam keeps running.");
    page.stop();
    browser.stop();
    state.stop();
    if (stop != nullptr) {
        CloseHandle(stop);
    }
    if (injected.process != nullptr) {
        CloseHandle(static_cast<HANDLE>(injected.process));
    }
    return 0;
}
