#pragma once

#include <string>

namespace doomnet {

// Opens the Windows folder chooser on its own thread. The state thread must
// poll folder_pick_take and park between polls. A second chooser is refused
// while the first is still open.
bool begin_folder_pick();

// "wait" while the chooser is open, "path" with the chosen folder, "cancel"
// when it was closed without a folder, "failed" with a sentence, or "idle"
// when nothing is open. A finished chooser is reported once.
std::string folder_pick_take(std::wstring& path, std::wstring& error);

}
