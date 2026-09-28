// Spec: Lua host. No Lua headers in this scaffold.
// Host module "json":
//   encode   tables the backend builds, including arrays
//   decode   objects and arrays back to tables
// Track ids stay strings. A numeric hash in scientific notation is how
// library.get_track misses a row.

#include "anchors.h"

namespace doomnet {
void host_json_anchor() {}
}
