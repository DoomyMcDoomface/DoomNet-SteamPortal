#pragma once

#include <string>

namespace doomnet {

// One answer for a steamloopback stream URL. `body` is the file bytes, not
// base64. `content_range` is set only for a 206.
struct StreamReply {
    int status = 404;
    std::string content_type;
    std::string body;
    std::string content_range;
    std::wstring detail;
};

// True after the main Steam page can intercept the stream URL. Until then
// the player keeps its copy-into-steamui fallback.
bool stream_hook_up();
void set_stream_hook(bool up);

// Read the original file for
// https://steamloopback.host/steam-music-player/stream/<token>/<id>
// `range_header` is the request's Range value, or empty.
// Never writes a copy under steamui.
StreamReply serve_stream_url(const std::string& url, const std::string& range_header = {});

}  // namespace doomnet
