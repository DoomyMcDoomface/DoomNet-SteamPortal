// Spec: Lua host, Threads. No Lua headers in this scaffold.
// Host module "logger": info, warn, error.
// Timestamped lines in logs\doomnet.log. No console.
// Called from the state thread only. The browser thread uses the log
// mutex directly and does not enter Lua to format a line.

#include "anchors.h"

namespace doomnet {
void host_logger_anchor() {}
}
