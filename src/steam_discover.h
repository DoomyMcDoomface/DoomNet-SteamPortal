#pragma once

#include <string>

namespace doomnet {

bool directory_has_steam_exe(const std::wstring& directory);
bool discover_steam_path(std::wstring& out);

}
