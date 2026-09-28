// Spec: Threads, Playback path.
// Owns the page-list socket, timers, and byte reads a coroutine handed off.
// Outbound work: Resolve, Sleep, Frontend, ReadRange, Stop.
// Never calls lua_*. Never base64-encodes on this thread.
// A page timer (audio chunk, 8 seconds) rejects the Promise. It does not
// cancel the coroutine. If no page has the frontend method yet, Resume with
// nil immediately.
// A ReadRange with no Range header is a 200 of the whole file. A 206 is
// only for a request that sent Range. The current page compares
// byteLength to size and falls back to base64 chunks if they differ.
// Do not fetch http://127.0.0.1 from a Steam page.

#include "anchors.h"

namespace doomnet {
void browser_thread_anchor() {}
}
