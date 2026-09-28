#pragma once

#include <string>

namespace doomnet {

// Data root. DOOMNET_ROOT overrides %LOCALAPPDATA%\DoomNet when it is an
// absolute path with no ".." component. An unusable override is an error
// and does not fall through to the real AppData directory.
bool data_root(std::wstring& out, std::wstring& error);

bool ensure_directory(const std::wstring& path);

}
