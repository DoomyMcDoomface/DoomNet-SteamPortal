#pragma once

// Grid thumbnails for the player's album covers.
//
// SteamTunes publishes each album cover at the size it was embedded in the
// music file, often several megabytes and thousands of pixels wide. The
// Artist, Album, and Genre grids draw them at about 200px, so every page
// decoded dozens of full-size images. This worker writes a small JPEG for
// each cover into a sibling "thumbs" folder, and the patched player UI loads
// that first and falls back to the full cover when it is missing.
//
// One background thread at background priority. It never calls Lua and it
// never writes outside the thumbs folder.

#include <memory>
#include <string>

namespace doomnet {

// Longest side of a thumbnail, in pixels.
constexpr unsigned kArtThumbSize = 400;

// Writes a JPEG no larger than max_side on either side. The image is not
// enlarged. The file is written to a temporary name and moved into place.
// Returns false with a sentence in error.
bool write_art_thumbnail(const std::wstring& source, const std::wstring& dest, unsigned max_side, std::wstring& error);

// Pixel size of the first frame of any image Windows can decode.
bool art_image_size(const std::wstring& path, unsigned& width, unsigned& height);

// "<dir>\albums\name.png" -> "<dir>\thumbs\name.jpg". Empty when the name is
// not a .jpg, .jpeg, or .png directly inside a folder named albums.
std::wstring art_thumb_path(const std::wstring& album_cover);

class ArtThumbs {
public:
    ArtThumbs();
    ~ArtThumbs();
    ArtThumbs(const ArtThumbs&) = delete;
    ArtThumbs& operator=(const ArtThumbs&) = delete;

    // albums is "<steam>\steamui\steam-music-player\art\albums". Thumbnails go
    // to "<steam>\steamui\steam-music-player\art\thumbs". The folder is walked
    // again every minute so covers published later, or restored after Steam
    // cleared steamui, get a thumbnail too.
    bool start(const std::wstring& albums, std::wstring& error);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace doomnet
