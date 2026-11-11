#pragma once

#include <string>

namespace doomnet {

// Reads the persona selected for automatic or most-recent Steam login.
// AccountName is never returned or exposed to the page.
bool streamtunes_persona(const std::wstring& steam_path, std::string& persona, std::wstring& error);

// Empty when the relay is disabled or the persona cannot be established.
std::string streamtunes_config_script(const std::wstring& steam_path, const std::wstring& relay_url, std::wstring& error);

// A saved address wins. A source tree with an empty address stays off.
// A tester install with an empty address uses the party already running for this test.
std::wstring streamtunes_relay_for_launch(const std::wstring& exe_path, const std::wstring& configured);

}  // namespace doomnet
