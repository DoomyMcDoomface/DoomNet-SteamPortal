#pragma once

#include "update_gate.h"

#include <string>

namespace doomnet {

struct ManifestPiece {
    std::wstring version;
    std::wstring url;
    std::string sha256;
};

struct ReleaseManifest {
    ManifestPiece doomnet;
    ManifestPiece steamtunes;
    ManifestPiece steamskin;
};

struct UpdatePlan {
    bool doomnet = false;
    bool steamtunes = false;
    bool steamskin = false;

    bool any() const { return doomnet || steamtunes || steamskin; }
};

// The production manifest is three pieces. A missing piece, a bad hash, or a
// URL that is not https fails the parse and leaves `out` unchanged.
bool parse_release_manifest(const std::string& json, ReleaseManifest& out, std::wstring& error);

// A piece is offered only when it is already installed and the manifest version
// differs. An empty SteamSkin version means that app was skipped, so a newer
// SteamSkin on GitHub does not install it.
UpdatePlan updates_needed(const ReleaseManifest& manifest, const InstalledVersions& installed);

std::string sha256_hex(const void* data, size_t size);
bool sha256_file(const std::wstring& path, std::string& hex, std::wstring& error);

// Logs `installer` with the path and returns false when the hash differs.
// The caller must not replace an install after false.
bool payload_hash_matches(const std::wstring& path, const std::string& expected_sha256, std::wstring& error);

struct UpdateCheckResult {
    // True when a payload was downloaded. The running program stays the old one until Steam starts again.
    bool restart_steam = false;
    std::wstring message;
};

// One sentence. Source trees and updates:false do not read a manifest.
// `steam_folder` is where version.dll may be replaced once Steam has quit.
UpdateCheckResult check_production_updates(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled, const std::wstring& steam_folder = {});

void note_production_update_gate(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled, const std::wstring& steam_folder = {});

// The Store button calls this from the state thread. The download runs on another thread.
// False means a check is already running.
bool begin_store_update_check(const std::wstring& data_root, const std::wstring& steam_path);

// "wait" until the check finishes, then "ready" and `message`. A ready result that downloaded
// a payload restarts Steam after this returns, so the page can show the message first.
std::string take_store_update_check(std::wstring& message);

// Replaces files staged by an earlier check. The player library and settings.json stay.
// A missing pending list is not an error.
bool apply_pending_updates(const std::wstring& data_root, const std::wstring& steam_folder);

// https only. A mismatch or a non-https URL does not leave a payload in place.
bool download_https(const std::wstring& url, const std::wstring& destination, size_t max_bytes, std::wstring& error);

// Copies `source` onto `destination` only after the hash matches. A mismatch
// leaves `destination` untouched.
bool replace_file_if_hash_matches(const std::wstring& source, const std::wstring& destination, const std::string& expected_sha256, std::wstring& error);

// True when that exact steam.exe, or steamwebhelper.exe in its folder, is running.
bool steam_client_running(const std::wstring& steam_exe);

// CreateProcess of `steam.exe -shutdown` in a visible window. launch_hidden is not used.
bool ask_steam_to_quit(const std::wstring& steam_exe, std::wstring& error);

// Starts `steam_exe` in a visible window. The file name must be steam.exe.
// A running copy of that same file is left alone. launch_hidden is not called.
bool start_steam_visible(const std::wstring& steam_exe, std::wstring& error);

}  // namespace doomnet
