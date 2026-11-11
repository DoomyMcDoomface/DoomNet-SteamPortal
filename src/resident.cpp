#include "resident.h"

#include "art_thumbs.h"
#include "browser_thread.h"
#include "catalog.h"
#include "global_hotkeys.h"
#include "host_config.h"
#include "log.h"
#include "json_wire.h"
#include "page_bridge.h"
#include "paths.h"
#include "payload.h"
#include "pipe_session.h"
#include "skin_page.h"
#include "state_thread.h"
#include "steam_attach.h"
#include "streamtunes_identity.h"
#include "text.h"
#include "update_gate.h"
#include "wide_io.h"

#include <windows.h>

#include <cstring>
#include <memory>
#include <string>

namespace doomnet {

namespace {

std::wstring trim(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const wchar_t* begin = text;
    while (*begin == L' ' || *begin == L'\t') {
        ++begin;
    }
    const wchar_t* end = begin + wcslen(begin);
    while (end > begin && (end[-1] == L' ' || end[-1] == L'\t')) {
        --end;
    }
    return std::wstring(begin, end);
}

bool stop_flag_present(const std::wstring& root) {
    const std::wstring path = root + L"\\stop.flag";
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool main_lua_present(const std::wstring& backend) {
    const DWORD attrs = GetFileAttributesW((backend + L"\\main.lua").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool file_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int copy_named(const std::wstring& from_dir, const std::wstring& to_dir, const wchar_t* const* names, int count) {
    CreateDirectoryW(to_dir.c_str(), nullptr);
    int copied = 0;
    for (int i = 0; i < count; ++i) {
        const std::wstring src = from_dir + L"\\" + names[i];
        const std::wstring dst = to_dir + L"\\" + names[i];
        if (!file_exists(src)) {
            continue;
        }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            ++copied;
        }
    }
    return copied;
}

const char* kAttachPostLoad =
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

bool stop_requested(HANDLE stop, const std::wstring& root) {
    if (stop != nullptr && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
        return true;
    }
    return stop_flag_present(root);
}

std::string library_ready_state(SteamPage& page) {
    const char* probe =
        "(function(){function list(v){var o=[],i,s,it;if(!v)return o;if(typeof v.length==='number'){for(i=0;i<v.length&&i<40;i++)o.push(v[i]);return o;}"
        "try{it=v[Symbol.iterator]();while(!(s=it.next()).done&&o.length<40)o.push(s.value);}catch(e){}return o;}"
        "var raw=null;try{raw=window.g_PopupManager&&g_PopupManager.GetPopups&&g_PopupManager.GetPopups();}catch(e){}"
        "var pops=list(raw),i;for(i=0;i<pops.length;i++){var popup=pops[i];var win=popup&&(popup.window||popup.m_popup);"
        "var title='';try{title=String(popup.m_strTitle||popup.title||(win&&win.document&&win.document.title)||'');}catch(e){}"
        "if(title!=='Steam'&&title!=='Steam Games List')continue;var state='';try{state=String(win&&win.__doomnetLibraryReady||'');}catch(e){}"
        "return state||'wait';}return 'nopopup';})()";
    std::string value;
    std::wstring error;
    if (!page.evaluate(probe, false, value, error)) {
        return "wait";
    }
    return value;
}

void wait_for_library_grid(SteamPage& page, HANDLE stop, const std::wstring& root, unsigned long pid) {
    const DWORD started = GetTickCount();
    std::string state = "wait";
    while (!stop_requested(stop, root) && GetTickCount() - started < 70000) {
        state = library_ready_state(page);
        if (state == "shown" || state == "skipped") {
            break;
        }
        global_keys_wait(250);
    }
    present_client_windows();
    std::wstring wide_state(state.begin(), state.end());
    if (state == "shown") {
        log_event(L"info", L"inject", (L"The library grid finished its first pass while Steam pid " + std::to_wstring(pid) + L" was hidden.").c_str());
    } else {
        log_event(L"warn", L"inject", (L"The library preload ended as '" + wide_state + L"', so Steam pid " + std::to_wstring(pid) + L" was shown without a finished grid.").c_str());
    }
}

void prepare_installed_files(const std::wstring& steam, const Config& config) {
    std::string page_preamble;
    std::wstring install_root;
    std::wstring root_error;
    data_root(install_root, root_error);
    const std::wstring skin_dir = launch_payload_dir(L"D:\\Documents\\Repos\\SteamSkin", install_root.empty() ? L"" : install_root + L"\\skin", L"skin.json");
    const std::wstring skin_json = skin_dir + L"\\skin.json";
    if (file_exists(skin_json)) {
        const SkinFileCopy copied = copy_skin_files(skin_dir, steam);
        log_event(L"info", L"payload", copied.detail.c_str());
        const SkinPreamble preamble = load_skin_preamble(skin_json, true);
        log_event(preamble.ok ? L"info" : L"warn", L"inject", preamble.detail.c_str());
        if (preamble.ok) {
            page_preamble = preamble.javascript;
        }
    } else {
        log_event(L"warn", L"inject", (L"'" + skin_json + L"' is not there, so this launch kept the skin files already in Steam.").c_str());
    }
    std::wstring party_error;
    const std::string party_config = streamtunes_config_script(steam, config.streamtunes_relay, party_error);
    if (!party_config.empty()) {
        if (!page_preamble.empty()) {
            page_preamble += "\n";
        }
        page_preamble += party_config;
        log_event(L"info", L"party", (L"Enabled the StreamTunes relay '" + config.streamtunes_relay + L"' for the logged-in Steam persona.").c_str());
    } else if (!party_error.empty()) {
        log_event(L"warn", L"party", party_error.c_str());
    } else {
        log_event(L"info", L"party", L"StreamTunes has no relay URL in config.json, so its controls stay hidden.");
    }
    std::wstring version_root;
    std::wstring version_error;
    InstalledVersions versions;
    if (data_root(version_root, version_error)) {
        std::wstring ignored;
        load_installed_versions(version_root + L"\\versions.json", versions, ignored);
    }
    const std::string skin_version = config.skin ? wide_to_utf8(versions.steamskin) : std::string();
    const std::string store_script = "window.__doomnetStore={apps:[{id:\"steamskin\",name:\"SteamSkin\",installed:" +
                                      std::string(config.skin ? "true" : "false") + ",version:" + json_quote(skin_version) + "}]};";
    if (!page_preamble.empty()) {
        page_preamble += "\n";
    }
    page_preamble += store_script;
    log_event(L"info", L"inject", config.skin
        ? (L"Listed SteamSkin in the Store as installed" + (versions.steamskin.empty() ? std::wstring(L".") : L" version '" + versions.steamskin + L"'.")).c_str()
        : L"Listed SteamSkin in the Store as not installed.");
    set_boot_preamble(page_preamble);
    const wchar_t* assets[] = {L"bootstrap.js", L"steam-music-player.css", L"audio-engine.js", L"steam-music-player.js", L"mix-image.js"};
    const std::wstring asset_src = launch_payload_dir(L"D:\\Documents\\Repos\\SteamTunes\\backend\\assets\\frontend", install_root.empty() ? L"" : install_root + L"\\player\\backend\\assets\\frontend", L"steam-music-player.js");
    const int copied_assets = copy_named(asset_src, steam + L"\\steamui\\steam-music-player", assets, 5);
    if (copied_assets > 0) {
        log_event(L"info", L"payload", (L"Copied " + std::to_wstring(copied_assets) + L" player file(s) into '" + steam + L"\\steamui\\steam-music-player'.").c_str());
    }
    const std::string patched_bootstrap = patched_player_bootstrap_text();
    if (!patched_bootstrap.empty()) {
        std::wstring write_error;
        const std::wstring published_bootstrap = steam + L"\\steamui\\steam-music-player\\bootstrap.js";
        if (!write_new_file(published_bootstrap, patched_bootstrap.data(), patched_bootstrap.size(), write_error) && !write_error.empty()) {
            log_event(L"warn", L"payload", write_error.c_str());
        }
    }
    const std::string patched = patched_player_ui_text();
    if (!patched.empty()) {
        std::wstring write_error;
        const std::wstring published = steam + L"\\steamui\\steam-music-player\\steam-music-player.js";
        if (!write_new_file(published, patched.data(), patched.size(), write_error) && !write_error.empty()) {
            log_event(L"warn", L"payload", write_error.c_str());
        }
    }
    const std::string patched_engine = patched_player_audio_engine_text();
    if (!patched_engine.empty()) {
        std::wstring write_error;
        const std::wstring published_engine = steam + L"\\steamui\\steam-music-player\\audio-engine.js";
        if (!write_new_file(published_engine, patched_engine.data(), patched_engine.size(), write_error) && !write_error.empty()) {
            log_event(L"warn", L"payload", write_error.c_str());
        }
    }
    const std::string patched_css = patched_player_css_text();
    if (!patched_css.empty()) {
        std::wstring write_error;
        const std::wstring published_css = steam + L"\\steamui\\steam-music-player\\steam-music-player.css";
        if (!write_new_file(published_css, patched_css.data(), patched_css.size(), write_error) && !write_error.empty()) {
            log_event(L"warn", L"payload", write_error.c_str());
        }
    }
}

void run_attach(HANDLE stop, const std::wstring& root, const std::wstring& steam, const Config& config) {
    const std::wstring flag = root + L"\\stop.flag";
    const std::wstring backend = root + L"\\player\\backend";
    wchar_t running_exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, running_exe, MAX_PATH);
    if (installed_host_leaves_when_steam_exits(running_exe)) {
        log_event(L"info", L"startup", L"Attaching to Steam the user started. This installed copy reads the player and skin in its data folder and checks GitHub for updates. It leaves when Steam exits so the next launch starts the program on disk.");
    } else {
        log_event(L"info", L"startup", (L"Attaching to Steam the user started. This host stays up so the next launch loads the player and the skin. Stop it by signaling Local\\DoomNetStop or by creating '" + flag + L"'.").c_str());
    }
    if (steam.empty()) {
        log_event(L"error", L"steam", L"Steam was not found, so this launch cannot inject.");
    } else if (!main_lua_present(backend)) {
        log_event(L"error", L"lua", (L"'" + backend + L"\\main.lua' is not there, so the player was not started.").c_str());
    } else {
        prepare_installed_files(steam, config);
    }

    ArtThumbs thumbs;
    if (!steam.empty()) {
        const std::wstring albums = steam + L"\\steamui\\steam-music-player\\art\\albums";
        std::wstring thumbs_error;
        if (thumbs.start(albums, thumbs_error)) {
            log_event(L"info", L"art", (L"Making grid thumbnails for '" + albums + L"' in the background.").c_str());
        } else {
            log_event(L"warn", L"art", thumbs_error.c_str());
        }
    }

    Bus bus;
    auto browser = std::make_unique<BrowserThread>(bus);
    std::unique_ptr<StateThread> state;
    SteamPage page;
    bool host_up = false;
    bool page_up = false;
    unsigned long attached_pid = 0;
    unsigned long presented_pid = 0;
    unsigned long holding_pid = 0;
    std::wstring browser_error;
    if (main_lua_present(backend) && bus.valid() && browser->start(browser_error)) {
        HostConfig host;
        host.backend_dir = backend;
        host.install_path = root;
        host.steam_path = steam;
        host.post_load_lua = kAttachPostLoad;
        std::wstring error;
        state = std::make_unique<StateThread>(bus, std::move(host));
        if (!state->start(error)) {
            log_event(L"error", L"lua", error.c_str());
            browser->stop();
            state.reset();
        } else {
            host_up = true;
            log_event(L"info", L"lua", L"A no-op try_load_compact_snapshot was installed so the player's startup can finish. The real function is local and is not in scope at the call.");
            log_event(L"info", L"lua", L"get_album_art_b64 reads covers from the art cache in the first music folder, then from the steamui art folder.");
            if (!state->wait_loaded(120000)) {
                log_event(L"warn", L"lua", L"The Lua host did not finish loading within 120 seconds. The player may answer late.");
            }
            if (!steam.empty()) {
                prepare_installed_files(steam, config);
                log_event(L"info", L"payload", L"Wrote version 1.16.0 into steamui after the player started. That page has the scan and the listening-party button. It is not a copy of 1.15.0.");
            }
        }
    } else if (!browser_error.empty()) {
        log_event(L"error", L"browser", browser_error.c_str());
    }
    if (host_up) {
        global_keys_bind(bus, root + L"\\player\\backend\\data\\settings.json");
    }

    ULONGLONG update_checked = GetTickCount64();
    bool saw_steam = false;
    while (!stop_requested(stop, root)) {
        if (GetTickCount64() - update_checked >= 60ull * 60ull * 1000ull) {
            update_checked = GetTickCount64();
            const std::wstring running_steam = !config.steam_path.empty() ? config.steam_path : steam;
            note_production_update_gate(running_exe, root, config.updates, running_steam);
        }
        if (steam.empty() || !main_lua_present(backend)) {
            global_keys_wait(250);
            continue;
        }
        const unsigned long pid = find_steam_pid(steam + L"\\steam.exe");
        if (pid == 0) {
            if (page_up) {
                page.stop();
                page_up = false;
                attached_pid = 0;
            }
            if (holding_pid != 0) {
                drop_client_windows();
                holding_pid = 0;
            }
            presented_pid = 0;
            if (saw_steam && installed_host_leaves_when_steam_exits(running_exe)) {
                log_event(L"info", L"steam", L"Steam exited. This installed copy is leaving so the next Steam launch starts the program on disk.");
                break;
            }
            if (saw_steam) {
                log_event(L"info", L"steam", L"Steam exited. The host stays up and will inject the next launch.");
                saw_steam = false;
            }
            global_keys_wait(250);
            continue;
        }
        saw_steam = true;
        if (presented_pid != pid && holding_pid != pid) {
            hold_client_windows(pid);
            holding_pid = pid;
        }
        if (page_up && pid == attached_pid) {
            global_keys_wait(250);
            continue;
        }
        if (page_up) {
            page.stop();
            page_up = false;
        }
        InjectResult injected = inject_running_client(steam + L"\\steam.exe");
        if (!injected.ok) {
            log_event(L"warn", L"inject", injected.detail.c_str());
            global_keys_wait(1000);
            continue;
        }
        std::wstring error;
        if (!page.start(std::move(injected.pipe), injected.session_id, *browser, bus, error)) {
            log_event(L"error", L"cdp", error.c_str());
            global_keys_wait(1000);
            continue;
        }
        page_up = true;
        attached_pid = injected.pid;
        log_event(L"info", L"inject", (L"The player and the skin are attached to Steam pid " + std::to_wstring(attached_pid) + L".").c_str());
        wait_for_library_grid(page, stop, root, attached_pid);
        holding_pid = 0;
        presented_pid = attached_pid;
    }

    if (stop != nullptr && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
        log_event(L"info", L"startup", L"Local\\DoomNetStop was signaled. The host is leaving.");
    } else if (stop_flag_present(root)) {
        log_event(L"info", L"startup", (L"'" + flag + L"' is present. The host is leaving.").c_str());
    }
    if (page_up) {
        page.stop();
    }
    global_keys_stop();
    if (host_up) {
        browser->stop();
        state->stop();
    }
    thumbs.stop();
}

}  // namespace

bool parse_startup_mode(const wchar_t* command_line, StartupMode& mode, std::wstring& rejected) {
    rejected.clear();
    const std::wstring text = trim(command_line);
    if (text.empty()) {
        mode = StartupMode::Once;
        return true;
    }
    const size_t split = text.find_first_of(L" \t");
    const std::wstring token = split == std::wstring::npos ? text : text.substr(0, split);
    const std::wstring rest = split == std::wstring::npos ? std::wstring() : trim(text.c_str() + split);
    if (!rest.empty() || (token != L"--run" && token != L"--watch" && token != L"--attach")) {
        mode = StartupMode::Once;
        rejected = text;
        return false;
    }
    if (token == L"--run") {
        mode = StartupMode::Run;
    } else if (token == L"--watch") {
        mode = StartupMode::Watch;
    } else {
        mode = StartupMode::Attach;
    }
    return true;
}

void run_resident(StartupMode mode, const std::wstring& root, const std::wstring& steam, const Config& config) {
    const std::wstring flag = root + L"\\stop.flag";
    if (stop_flag_present(root)) {
        const std::wstring detail = L"'" + flag + L"' is already present, so this process is not staying running and the Lua host was not started.";
        log_event(L"info", L"startup", detail.c_str());
        return;
    }

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::wstring steam_folder = !config.steam_path.empty() ? config.steam_path : steam;
    if (updates_should_run(exe_path, root, config.updates)) {
        apply_pending_updates(root, steam_folder);
        note_production_update_gate(exe_path, root, config.updates, steam_folder);
    }

    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\DoomNetStop");
    if (stop == nullptr) {
        const std::wstring detail = L"Could not create Local\\DoomNetStop, so the only way to stop this process is '" + flag + L"'. " + windows_error_text(GetLastError());
        log_event(L"error", L"startup", detail.c_str());
    } else {
        // A previous run may have left the event signaled. This run waits for a new request.
        ResetEvent(stop);
    }

    if (mode == StartupMode::Attach) {
        run_attach(stop, root, steam, config);
        if (stop != nullptr) {
            CloseHandle(stop);
        }
        log_event(L"info", L"startup", L"The host has stopped.");
        return;
    }

    Bus bus;
    auto browser = std::make_unique<BrowserThread>(bus);
    std::unique_ptr<StateThread> state;
    bool host_up = false;

    const std::wstring backend = root + L"\\player\\backend";
    if (!config.player) {
        log_event(L"info", L"lua", L"The player is off in config.json, so the Lua host was not started.");
    } else if (!main_lua_present(backend)) {
        const std::wstring detail = L"The player is on, but '" + backend + L"\\main.lua' is not there, so the Lua host was not started.";
        log_event(L"error", L"lua", detail.c_str());
    } else if (!bus.valid()) {
        log_event(L"error", L"lua", L"The Lua host was not started because the message bus could not create its event.");
    } else {
        HostConfig host;
        host.backend_dir = backend;
        host.install_path = root;
        host.steam_path = !config.steam_path.empty() ? config.steam_path : steam;
        std::wstring error;
        if (!browser->start(error)) {
            log_event(L"error", L"browser", error.c_str());
        } else {
            state = std::make_unique<StateThread>(bus, std::move(host));
            if (!state->start(error)) {
                log_event(L"error", L"lua", error.c_str());
                browser->stop();
                state.reset();
            } else {
                host_up = true;
            }
        }
    }

    if (mode == StartupMode::Watch) {
        const std::wstring detail = L"Watching for a Steam launch. This build does not start steam.exe and does not inject. Stop it by signaling Local\\DoomNetStop or by creating '" + flag + L"'.";
        log_event(L"info", L"startup", detail.c_str());
    } else {
        const std::wstring detail = L"Running the host. Steam was not started and nothing was injected. Stop it by signaling Local\\DoomNetStop or by creating '" + flag + L"'.";
        log_event(L"info", L"startup", detail.c_str());
    }

    if (host_up) {
        global_keys_bind(bus, root + L"\\player\\backend\\data\\settings.json");
    }

    ULONGLONG update_checked = GetTickCount64();
    bool restart_pending = false;
    bool quit_sent = false;

    for (;;) {
        if (host_up) {
            global_keys_poll();
        }
        // The keyboard hook is delivered on this thread. A plain wait would
        // miss the key and Windows would pass it to the focused program.
        const DWORD slice = global_keys_armed() ? 30u : 250u;
        if (stop != nullptr) {
            const DWORD wake = MsgWaitForMultipleObjects(1, &stop, FALSE, slice, QS_ALLINPUT);
            if (wake == WAIT_OBJECT_0) {
                log_event(L"info", L"startup", L"Local\\DoomNetStop was signaled. The host is leaving.");
                break;
            }
        } else {
            Sleep(slice);
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (stop_flag_present(root)) {
            const std::wstring detail = L"'" + flag + L"' is present. The host is leaving.";
            log_event(L"info", L"startup", detail.c_str());
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - update_checked >= 60ull * 60ull * 1000ull) {
            update_checked = now;
            note_production_update_gate(exe_path, root, config.updates, steam_folder);
        }
        StoreInstallRequest store;
        if (take_store_install(store)) {
            if (store.root.empty()) {
                store.root = root;
            }
            if (store.steam_path.empty()) {
                store.steam_path = !config.steam_path.empty() ? config.steam_path : steam;
            }
            const InstallResult installed = install_store_skin(store);
            if (installed.ok) {
                restart_pending = true;
                quit_sent = false;
            }
        }
        if (restart_pending) {
            std::wstring folder = !config.steam_path.empty() ? config.steam_path : steam;
            if (folder.empty()) {
                log_event(L"error", L"installer", L"Steam was not restarted because no Steam folder is configured.");
                restart_pending = false;
                continue;
            }
            std::wstring steam_exe = folder;
            if (steam_exe.size() < 9 || _wcsicmp(steam_exe.c_str() + (steam_exe.size() - 9), L"steam.exe") != 0) {
                if (!steam_exe.empty() && steam_exe.back() != L'\\' && steam_exe.back() != L'/') {
                    steam_exe += L'\\';
                }
                steam_exe += L"steam.exe";
            }
            std::wstring restart_error;
            if (steam_client_running(steam_exe)) {
                if (!quit_sent) {
                    ask_steam_to_quit(steam_exe, restart_error);
                    quit_sent = true;
                }
            } else if (start_steam_visible(steam_exe, restart_error)) {
                restart_pending = false;
            } else {
                restart_pending = false;
            }
        }
    }

    global_keys_stop();
    if (host_up) {
        browser->stop();
        state->stop();
    }
    if (stop != nullptr) {
        CloseHandle(stop);
    }
    log_event(L"info", L"startup", L"The host has stopped.");
}

}  // namespace doomnet
