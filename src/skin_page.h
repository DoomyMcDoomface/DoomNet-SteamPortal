#pragma once

#include <string>

namespace doomnet {

struct SkinFileCopy {
    bool ok = false;
    int copied = 0;
    std::wstring destination;
    std::wstring detail;
};

// Copies *.css and *.js from `skin_dir` into <steam_dir>\steamui\doomnet-skin.
// `steam_dir` must contain steam.exe. A path containing ".." is refused.
// UseDefaultPatches is not consulted here. music-player-inject.js is copied
// with the other files and is not inserted by the page.
SkinFileCopy copy_skin_files(const std::wstring& skin_dir, const std::wstring& steam_dir);

struct SkinPreamble {
    bool ok = false;
    int patches = 0;
    std::string javascript;
    std::wstring detail;
};

// The music player's named colors. Custom uses `custom` when it is #rrggbb.
// The dark color is the player's accentDark, or the custom color darkened the same way.
bool skin_accent_for_theme(const std::string& theme, const std::string& custom, std::string& accent, std::string& accent_dark);

// Replaces SteamSkin's red accent colors with `accent` and `accent_dark`.
// Grey, blue, and white values in the same file stay as they are.
std::string recolor_skin_css(std::string css, const std::string& accent, const std::string& accent_dark);

// Reads skin.json Patches and builds the window.__doomnetConfig statement.
// Skips every TargetJs of music-player-inject.js. Does not add Millennium's
// default patches when UseDefaultPatches is true. `player` inserts the player
// bootstrap; a skin-only load passes false.
SkinPreamble load_skin_preamble(const std::wstring& skin_json, bool player);

// The patches from the last successful load_skin_preamble. The store document
// is not the library popup, so those tags are also inserted into matching
// document responses.
bool skin_tags_ready();

// True after a successful load_skin_preamble whose player flag was true.
bool skin_player_requested();
std::string skin_tags_for_document(const std::string& url);
std::string inject_skin_into_html(const std::string& html, const std::string& tags);

// A small page script that inserts the matching skin CSS as style tags.
// Empty when this URL has no CSS. Used on store documents so the color does
// not depend on holding the network response.
std::string skin_style_script(const std::string& url);

// Inlines the player CSS and scripts for a store or community document.
// Those pages block a steamloopback script address. Empty if the files
// cannot be read.
std::string store_player_script();

// The first folder that contains `file_name`. The SteamTunes and SteamSkin
// repos win on this machine. An installed copy is used when the repo is absent.
std::wstring first_existing_marker(const std::wstring& primary, const std::wstring& fallback, const wchar_t* file_name);

// A source tree prefers `repo`. An installed copy returns `installed` and does not read the repo.
std::wstring launch_payload_dir_for(const std::wstring& exe_path, const std::wstring& data_root, const std::wstring& repo, const std::wstring& installed, const wchar_t* file_name);
std::wstring launch_payload_dir(const std::wstring& repo, const std::wstring& installed, const wchar_t* file_name);

// The player UI with the store cover fallback applied. The SteamTunes
// file on disk is not changed.
std::string patched_player_ui_text();
// bootstrap.js with a new cache name so Steam loads this version instead of 1.15.0.
std::string patched_player_bootstrap_text();
// Rewrites an already-patched player page so Start Listening Party stays on the SteamTunes row.
void repair_party_buttons(std::string& ui);
// Drops earlier copies when the same block was written into the player page more than once.
void keep_one_repeated_block(std::string& text, const std::string& marker);

// The player engine with a second output path that is created only while a
// StreamTunes host session is active. The normal dry/wet -> masterGain path
// is not changed while that output is detached.
std::string patched_player_audio_engine_text();

// The player CSS with the now-playing artist link limited to its text.
// The SteamTunes file on disk is not changed.
std::string patched_player_css_text();

}
