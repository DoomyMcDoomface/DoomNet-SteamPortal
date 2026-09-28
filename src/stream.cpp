// Spec: Playback path, Contracts to match.
// URL the page fetches, built by stage_track_for_playback as an opaque
// string: https://steamloopback.host/steam-music-player/stream/<token>/<id>
// <id> is tostring(track.id), not staged_name_for.
// Token is the hex in player data media_token (at least 16 hex digits).
// Lookup id in library_index.json (id, path, size). library_snapshot.json
// has no filesystem path.
// Missing id, "..", a drive letter, or a path segment is 404.
// Never join the URL onto a directory.
// No Range header: 200, entire file, Content-Length equal to size.
// release_staged_track on this path deletes nothing and returns ok true.
// get_host_escape_probe's "%2e%2e" URL is 404.
// Wide CreateFileW for the open. Do not copy the file into steamui when
// this handler is up.

#include "anchors.h"

namespace doomnet {
void stream_anchor() {}
}
