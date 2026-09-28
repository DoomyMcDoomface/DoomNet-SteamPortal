#pragma once

namespace doomnet {

// Owns Local\DoomNet until this process exits. A second call, including
// from another process, returns false immediately and does not wait.
bool acquire_instance();

}
