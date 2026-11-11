#pragma once

#include <string>

namespace doomnet {

// True when path is under this machine's DoomNet, SteamTunes, or SteamSkin
// source trees. Tester installs never live there, so production updates stay off
// for the lead-dev copy without needing a config flag.
bool path_is_dev_source_tree(const std::wstring& path);

// Production updates run only when config enables them and neither the running
// executable nor the data root sits in a source tree.
bool updates_should_run(const std::wstring& exe_path, const std::wstring& data_root, bool config_updates_enabled);

struct InstalledVersions {
    std::wstring doomnet;
    std::wstring steamtunes;
    std::wstring steamskin;
};

// versions.json under the data root. Missing keys stay empty. Unknown keys and
// malformed JSON fail with a full sentence and leave `out` unchanged.
bool load_installed_versions(const std::wstring& path, InstalledVersions& out, std::wstring& error);
bool save_installed_versions(const std::wstring& path, const InstalledVersions& versions, std::wstring& error);

}  // namespace doomnet
