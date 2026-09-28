#pragma once

#include <string>

namespace doomnet {

// CreateFileW helpers. Paths containing ".." are rejected.
// read_entire_file refuses files over 1 MiB so a test cannot pull a library into memory.
bool wide_file_exists(const std::wstring& path);
bool write_new_file(const std::wstring& path, const void* data, size_t size, std::wstring& error);
bool read_entire_file(const std::wstring& path, std::string& out, std::wstring& error);

}
