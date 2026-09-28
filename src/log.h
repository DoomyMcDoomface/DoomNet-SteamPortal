#pragma once

#include <string>

namespace doomnet {

// One physical line, prefixed with this process id:
//   timestamp level pid=1234 component: sentence
// Newlines in the text are spaces, so a reader can split the file on lines.
// The write is serialized with Local\DoomNetLog and waits at most one second.
// Returns false if the line was not written. Never opens a console.
bool log_event(const wchar_t* level, const wchar_t* component, const wchar_t* detail);

// Same file, without a component prefix. Tests use this for the tear check.
bool log_line(const wchar_t* level, const wchar_t* message);

// "Windows error 2 (The system cannot find the file specified.)"
// The number is always present. The words are the system message when Windows has one.
std::wstring windows_error_text(unsigned long code);

}
