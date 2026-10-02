#pragma once

#include <string>

namespace doomnet {

// UTF-8 <-> UTF-16 helpers for the Lua host.
//
// Every string that crosses between Lua and Windows is UTF-8 inside Lua.
// Millennium's host used the ANSI code page, which is why the player has so
// much code to work around non-ASCII music paths. Here a Lua path is decoded
// as UTF-8 and handed to the wide Windows APIs.

// Strict decode. False when the bytes are not valid UTF-8.
bool utf8_to_wide_strict(const char* bytes, size_t length, std::wstring& out);

// Lua path -> wide path. UTF-8 first. If the bytes are not valid UTF-8 they are
// read as the system ANSI code page, which is what a path produced by
// Millennium-era code would contain. False if neither decode works or the
// path is empty or contains a NUL.
bool lua_path_to_wide(const char* bytes, size_t length, std::wstring& out);

// Lossy decode: invalid sequences become U+FFFD. Never fails.
std::wstring utf8_to_wide_lossy(const std::string& text);

std::string wide_to_utf8(const std::wstring& text);

// Returns text with every invalid UTF-8 sequence replaced by U+FFFD (EF BF BD).
// Used before bytes from Lua are placed into JSON that a page will parse.
std::string sanitize_utf8(const std::string& text);

}  // namespace doomnet
