#pragma once

#include <string>
#include <vector>

namespace doomnet {

struct PlaylistSong {
    std::string label;
    std::string path;
};

// UTF-8 extended M3U. Other players open this file. Paths stay on their own line.
std::string playlist_m3u(const std::vector<PlaylistSong>& songs);

// "Cool Mix" becomes "Cool Mix.m3u". Characters a file name cannot use are spaces.
std::wstring playlist_file_name(const std::wstring& playlist_name);

// A song path from a playlist file, joined to that file's folder when it is relative.
std::string resolve_playlist_entry(const std::string& playlist_path, const std::string& entry);

// The open-dialog filter. The same extensions the playlist reader understands.
const wchar_t* playlist_open_filter();

// Writes the M3U, replacing a file that is already there. A path containing ".." is refused.
bool write_playlist_file(const std::wstring& path, const std::string& body, std::wstring& error);

// The page calls export_playlist and import_playlist. The Windows file window runs on
// its own thread, and those functions park between polls.
const char* playlist_io_script();

// Opens the Windows file window on its own thread. A second window is refused while
// the first is still open.
bool begin_playlist_save(const std::wstring& playlist_name);
bool begin_playlist_open();

// "wait" while the window is open, "path" with the chosen file, "cancel" when it
// was closed without a file, "failed" with a sentence, or "idle" when nothing is open.
std::string playlist_dialog_take(std::wstring& path, std::wstring& error);

}
