#pragma once

// The exe side of injection. The hook stays in wsock32.dll. This file copies
// that DLL beside steam.exe only when Millennium's copy is not there, and it
// can start a steam.exe that is not the discovered install.

#include "cdp_pipe.h"

#include <string>

namespace doomnet {

struct PlaceBootstrapResult {
    bool ok = false;
    bool replaced = false;
    std::wstring detail;
};

// Copies `dll_source` to <steam_path>\wsock32.dll.
// Refuses when Millennium is active, when the file already there is Millennium's
// bootstrap, when the file already there is some other unrecognized DLL, when
// the path is empty or contains "..", when steam.exe is absent, or when the
// path is inside the Windows directory.
// `require_steam_closed` refuses while steam.exe or steamwebhelper.exe is running.
PlaceBootstrapResult place_doomnet_bootstrap(const std::wstring& steam_path, const std::wstring& dll_source, bool require_steam_closed);

struct InjectResult {
    bool ok = false;
    unsigned long pid = 0;
    // A process handle when a client was started. The caller closes it.
    // Null when this call did not leave a process running.
    void* process = nullptr;
    std::wstring detail;
    // Open when `ok` is true. The caller answers page calls on this pipe.
    CdpPipe pipe;
    std::string session_id;
};

// Starts `steam_exe` (the file, not a searched install) and injects the boot
// script over the debugging pipe once steamui.dll is loaded in that process.
// Refuses the discovered Steam install unless `allow_real_client` is true, so
// tests cannot open the user's Steam by accident. Does not use launch_hidden.
InjectResult launch_and_inject_boot(const std::wstring& steam_exe, const std::wstring& arguments, bool allow_real_client);

// Connects to a Steam the user already started. Does not start steam.exe and
// does not stop it if the pipe is not ready yet.
InjectResult inject_running_client(const std::wstring& steam_exe);

// The pid of steam.exe at `steam_exe`, or 0 when that client is not running.
unsigned long find_steam_pid(const std::wstring& steam_exe);

// Shows top-level windows owned by `pid` and its child processes. Returns how many were shown.
int show_client_windows(unsigned long pid);

// Keeps the client windows hidden until present_client_windows. Used so the
// store cannot paint Steam blue before the skin is in the document.
void hold_client_windows(unsigned long pid);
void present_client_windows();
void drop_client_windows();

void steam_attach_anchor();

}  // namespace doomnet
