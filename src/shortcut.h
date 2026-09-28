#pragma once

#include <string>

namespace doomnet {

// Writes a .lnk in a caller-chosen folder. Does not look at the desktop.
bool create_shortcut(const std::wstring& lnk_path, const std::wstring& target, const std::wstring& icon_path, std::wstring& error);

// Changes the shortcut target and leaves the icon path as icon_path.
// Refuses a target whose file name is steam.exe.
bool retarget_shortcut(const std::wstring& lnk_path, const std::wstring& new_target, const std::wstring& icon_path, std::wstring& error);

bool read_shortcut(const std::wstring& lnk_path, std::wstring& target, std::wstring& icon_path, std::wstring& error);

}
