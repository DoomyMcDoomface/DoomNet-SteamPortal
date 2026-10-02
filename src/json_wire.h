#pragma once

// A small JSON reader and string quoter for the browser thread. The Lua json
// module stays on the state thread; this one never touches a lua_State.

#include <string>
#include <utility>
#include <vector>

namespace doomnet {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* find(const char* key) const;
    std::string key_string(const char* key) const;
    bool key_bool(const char* key) const;
};

// False when `text` is not one JSON value. `error` is a short reason.
bool json_parse(const std::string& text, Json& out, std::string& error);

// A JSON string literal, including the quotes.
std::string json_quote(const std::string& text);

}  // namespace doomnet
