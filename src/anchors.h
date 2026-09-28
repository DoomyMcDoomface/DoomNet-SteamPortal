#pragma once

// Each translation unit defines one anchor. main.cpp calls all of them.
// That is the skeleton's link check: drop a file from CMakeLists.txt and
// the build fails. The functions allocate nothing and do no I/O.
// Replace the body when filling that file. Do not add work to the anchors
// themselves.

namespace doomnet {

void log_anchor();
void config_anchor();
void steam_discover_anchor();
void state_thread_anchor();
void browser_thread_anchor();
void steam_attach_anchor();
void stream_anchor();
void host_fs_anchor();
void host_json_anchor();
void host_logger_anchor();
void host_utils_anchor();
void host_millennium_anchor();
void toggle_anchor();
void installer_anchor();

}
