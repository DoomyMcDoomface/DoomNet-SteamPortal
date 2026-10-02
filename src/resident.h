#pragma once

// The resident paths. The no-argument launch finishes its checks and returns.
// `--run` and `--watch` stay in the process until they are asked to stop.
// They do not start steam.exe. `--attach` is started by the bootstrap when
// the user opens Steam, and it injects into that running client.

#include "config.h"

#include <string>

namespace doomnet {

enum class StartupMode {
    Once,    // no arguments: the path Check-Skeleton.ps1 measures
    Run,     // --run
    Watch,   // --watch
    Attach,  // --attach
};

// `command_line` is wWinMain's lpCmdLine, which does not include the program name.
// False when the text is not empty, `--run`, `--watch`, or `--attach`. `rejected` is that text.
bool parse_startup_mode(const wchar_t* command_line, StartupMode& mode, std::wstring& rejected);

// Blocks until Local\DoomNetStop is signaled or <root>\stop.flag exists.
// Starts the Lua host only when the player is on and <root>\player\backend\main.lua
// is present. `--run` and `--watch` do not start Steam. `--attach` injects into
// a Steam the user already started and stays up for the next launch.
void run_resident(StartupMode mode, const std::wstring& root, const std::wstring& steam, const Config& config);

}  // namespace doomnet
