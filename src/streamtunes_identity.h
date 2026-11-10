#pragma once

#include <string>

namespace doomnet {

// Reads the persona selected for automatic or most-recent Steam login.
// AccountName is never returned or exposed to the page.
bool streamtunes_persona(const std::wstring& steam_path, std::string& persona, std::wstring& error);

// Empty when the relay is disabled or the persona cannot be established.
std::string streamtunes_config_script(const std::wstring& steam_path, const std::wstring& relay_url, std::wstring& error);

}  // namespace doomnet
