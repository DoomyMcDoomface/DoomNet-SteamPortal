#include "global_hotkeys.h"

#include "log.h"
#include "wide_io.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace doomnet {

namespace {

Bus* g_bus = nullptr;
std::wstring g_settings_path;
std::atomic<bool> g_armed{false};
std::atomic<int> g_play{0};
std::atomic<int> g_stop{0};
std::atomic<int> g_next{0};
std::atomic<int> g_prev{0};
std::atomic<int> g_mute{0};
std::atomic<int> g_volume{0};
HHOOK g_hook = nullptr;
DWORD g_hook_at = 0;
DWORD g_settings_at = 0;
DWORD g_volume_log_at = 0;
bool g_want = false;
bool g_logged_armed = false;
bool g_logged_hook_failure = false;
unsigned g_down = 0;

// The first signal for a key counts. Anything for that key inside this window is the same press.
constexpr DWORD kTransportQuietMs = 500;
DWORD g_transport_at[4] = {};
bool g_transport_seen[4] = {};

int transport_slot(DWORD vk) {
    switch (vk) {
        case VK_MEDIA_PLAY_PAUSE:
            return 0;
        case VK_MEDIA_STOP:
            return 1;
        case VK_MEDIA_NEXT_TRACK:
            return 2;
        case VK_MEDIA_PREV_TRACK:
            return 3;
        default:
            return -1;
    }
}

bool transport_is_same_press(int slot) {
    const DWORD now = GetTickCount();
    if (g_transport_seen[slot] && now - g_transport_at[slot] < kTransportQuietMs) {
        g_transport_at[slot] = now;
        return true;
    }
    g_transport_seen[slot] = true;
    g_transport_at[slot] = now;
    return false;
}

unsigned key_bit(DWORD vk) {
    switch (vk) {
        case VK_VOLUME_UP:
            return 1u;
        case VK_VOLUME_DOWN:
            return 2u;
        case VK_VOLUME_MUTE:
            return 4u;
        case VK_MEDIA_PLAY_PAUSE:
            return 8u;
        case VK_MEDIA_STOP:
            return 16u;
        case VK_MEDIA_NEXT_TRACK:
            return 32u;
        case VK_MEDIA_PREV_TRACK:
            return 64u;
        default:
            return 0;
    }
}

bool is_volume_key(DWORD vk) {
    return vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE;
}

bool is_media_key(DWORD vk) {
    return is_volume_key(vk) || vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK || vk == VK_MEDIA_STOP || vk == VK_MEDIA_PLAY_PAUSE;
}

void flush_volume(bool log_allowed);

LRESULT CALLBACK keyboard_hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code < 0 || !g_armed.load()) {
        return CallNextHookEx(g_hook, code, wparam, lparam);
    }
    const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (key == nullptr || !is_media_key(key->vkCode)) {
        return CallNextHookEx(g_hook, code, wparam, lparam);
    }
    // Eat the key so Windows does not move the system volume and so another
    // player that registered the same key does not see it.
    const unsigned bit = key_bit(key->vkCode);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    const int slot = transport_slot(key->vkCode);
    if (!down) {
        g_down &= ~bit;
        if (slot >= 0 && g_transport_seen[slot]) {
            g_transport_at[slot] = GetTickCount();
        }
        return 1;
    }
    if (slot >= 0 && transport_is_same_press(slot)) {
        return 1;
    }
    const bool repeat = (g_down & bit) != 0;
    g_down |= bit;
    if (repeat && key->vkCode == VK_VOLUME_MUTE) {
        return 1;
    }
    switch (key->vkCode) {
        case VK_VOLUME_UP:
            g_volume.fetch_add(1);
            flush_volume(false);
            break;
        case VK_VOLUME_DOWN:
            g_volume.fetch_sub(1);
            flush_volume(false);
            break;
        case VK_VOLUME_MUTE:
            g_mute.fetch_add(1);
            break;
        case VK_MEDIA_PLAY_PAUSE:
            g_play.store(1);
            break;
        case VK_MEDIA_STOP:
            g_stop.store(1);
            break;
        case VK_MEDIA_NEXT_TRACK:
            g_next.store(1);
            break;
        case VK_MEDIA_PREV_TRACK:
            g_prev.store(1);
            break;
        default:
            break;
    }
    return 1;
}

bool json_flag(const std::string& text, const char* key, bool fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) {
        return fallback;
    }
    at += needle.size();
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) {
        ++at;
    }
    if (at >= text.size() || text[at] != ':') {
        return fallback;
    }
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) {
        ++at;
    }
    if (text.compare(at, 4, "true") == 0) {
        return true;
    }
    if (text.compare(at, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

unsigned long long g_command_number = 0;

void post_command(const std::string& json) {
    if (g_bus == nullptr || !g_bus->valid() || json.empty() || json.back() != '}') {
        return;
    }
    // The player applies a command with an id once, however many windows or
    // relays hand it over. Without one, the same press was applied twice.
    ++g_command_number;
    std::string with_id = json.substr(0, json.size() - 1);
    with_id += ",\"id\":\"doomnet-key-" + std::to_string(GetTickCount64()) + "-" + std::to_string(g_command_number) + "\"}";
    CallMsg message;
    message.call_id = 0;
    message.method = "transport_command";
    WireValue arg;
    arg.kind = WireValue::Kind::String;
    arg.text = with_id;
    message.args.push_back(std::move(arg));
    g_bus->to_state(std::move(message));
}

void install_hook() {
    if (g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0);
    g_hook_at = GetTickCount();
    if (g_hook == nullptr) {
        if (!g_logged_hook_failure) {
            g_logged_hook_failure = true;
            const std::wstring detail = L"The keyboard hook could not be installed, so Global Control is not taking media keys. " + windows_error_text(GetLastError());
            log_event(L"error", L"hook", detail.c_str());
        }
    } else {
        g_logged_hook_failure = false;
    }
}

void remove_hook() {
    if (g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
}

void refresh_settings() {
    const DWORD now = GetTickCount();
    if (g_settings_at != 0 && now - g_settings_at < 300) {
        return;
    }
    g_settings_at = now;
    std::string text;
    std::wstring error;
    const bool want = read_entire_file(g_settings_path, text, error) && json_flag(text, "mediaKeysEnabled", true) && json_flag(text, "globalControl", false);
    if (want == g_want) {
        return;
    }
    g_want = want;
    g_armed.store(want);
    if (want) {
        install_hook();
        if (!g_logged_armed && g_hook != nullptr) {
            g_logged_armed = true;
            const std::wstring detail = L"Global Control is on in '" + g_settings_path + L"', so play, pause, stop, next, previous, and the volume keys go to the player even when Steam is not in front. Windows and other programs do not receive those keys.";
            log_event(L"info", L"hook", detail.c_str());
        }
    } else {
        remove_hook();
        if (g_logged_armed) {
            g_logged_armed = false;
            log_event(L"info", L"hook", L"Global Control is off, so media and volume keys stay with the focused program.");
        }
    }
}

void flush_volume(bool log_allowed);

void drain() {
    if (!g_armed.load() || g_bus == nullptr) {
        g_play.store(0);
        g_stop.store(0);
        g_next.store(0);
        g_prev.store(0);
        g_mute.store(0);
        g_volume.store(0);
        return;
    }
    if (g_play.exchange(0) != 0) {
        post_command("{\"action\":\"toggle\"}");
        log_event(L"info", L"hook", L"Play/Pause went to the player.");
    }
    if (g_stop.exchange(0) != 0) {
        post_command("{\"action\":\"pause\"}");
        log_event(L"info", L"hook", L"Stop went to the player as pause.");
    }
    if (g_prev.exchange(0) != 0) {
        post_command("{\"action\":\"prev\"}");
        log_event(L"info", L"hook", L"Previous went to the player.");
    }
    if (g_next.exchange(0) != 0) {
        post_command("{\"action\":\"next\"}");
        log_event(L"info", L"hook", L"Next went to the player.");
    }
    if (g_mute.exchange(0) != 0) {
        post_command("{\"action\":\"volumeBump\",\"value\":0}");
        log_event(L"info", L"hook", L"Mute went to the player volume, not the Windows volume.");
    }
    flush_volume(true);
}

// The hook calls this with log_allowed false. Writing the log there holds the
// key, and Windows will not deliver the next notch until the hook returns.
void flush_volume(bool log_allowed) {
    if (!g_armed.load() || g_bus == nullptr) {
        g_volume.store(0);
        return;
    }
    const int steps = g_volume.exchange(0);
    if (steps == 0) {
        return;
    }
    const double delta = steps * 0.03;
    if (!g_bus->merge_queued_volume(delta)) {
        char json[80];
        std::snprintf(json, sizeof(json), "{\"action\":\"volumeBump\",\"value\":%.2f}", delta);
        post_command(json);
    }
    if (!log_allowed) {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_volume_log_at == 0 || now - g_volume_log_at > 1000) {
        g_volume_log_at = now;
        log_event(L"info", L"hook", L"A volume key changed the player volume and did not change the Windows volume.");
    }
}

}  // namespace

void global_keys_bind(Bus& bus, const std::wstring& settings_path) {
    g_bus = &bus;
    g_settings_path = settings_path;
    g_settings_at = 0;
    log_event(L"info", L"hook", (L"Watching '" + settings_path + L"' for Global Control.").c_str());
}

void global_keys_poll() {
    if (g_bus == nullptr) {
        return;
    }
    drain();
    refresh_settings();
    if (g_want && g_hook != nullptr && GetTickCount() - g_hook_at > 5000) {
        // A player that starts later installs its own hook in front of ours.
        // Installing again puts this one first, which is what Global Control means.
        install_hook();
    } else if (g_want && g_hook == nullptr) {
        install_hook();
    }
}

bool global_keys_armed() {
    return g_armed.load();
}

void global_keys_wait(unsigned long milliseconds) {
    if (g_bus == nullptr) {
        Sleep(milliseconds);
        return;
    }
    const DWORD start = GetTickCount();
    for (;;) {
        global_keys_poll();
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= milliseconds) {
            break;
        }
        DWORD slice = milliseconds - elapsed;
        if (slice > 30) {
            slice = 30;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void global_keys_stop() {
    g_armed.store(false);
    g_want = false;
    remove_hook();
    g_bus = nullptr;
    g_play.store(0);
    g_stop.store(0);
    g_next.store(0);
    g_prev.store(0);
    g_mute.store(0);
    g_volume.store(0);
    g_down = 0;
    for (int i = 0; i < 4; ++i) {
        g_transport_seen[i] = false;
        g_transport_at[i] = 0;
    }
}

}  // namespace doomnet
