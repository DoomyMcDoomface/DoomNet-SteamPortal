#pragma once

#include <windows.h>

namespace doomnet {

// True only when both the player and the skin were selected.
bool color_question_needed(bool player, bool skin);

// Theme written into a new settings file. Null when nothing should be written:
// skin-only, neither piece, or settings.json already exists.
// match_skin is the installer's answer and is ignored unless the question is asked.
const wchar_t* theme_for_new_settings(bool player, bool skin, bool match_skin, bool settings_exist);

struct InstallChoices {
    bool player = false;
    bool skin = false;
    bool match_skin = false;
    bool accepted = false;
};

constexpr int kInstallIdPlayer = 101;
constexpr int kInstallIdSkin = 102;
constexpr int kInstallIdSteamBlue = 103;
constexpr int kInstallIdMatch = 104;
constexpr int kInstallIdInstall = 105;
constexpr int kInstallIdCancel = 106;

// Hidden when created. Startup does not call this. Tests drive it with SendMessage.
HWND create_install_window(InstallChoices* choices);

}
