#include "streamtunes_identity.h"

#include "json_wire.h"
#include "text.h"
#include "update_gate.h"
#include "wide_io.h"

#include <regex>
#include <vector>

namespace doomnet {
namespace {

std::string value_in(const std::string& block, const char* key) {
    const std::regex pattern(std::string("\"") + key + "\"\\s*\"((?:\\\\.|[^\"])*)\"", std::regex::ECMAScript);
    std::smatch match;
    if (!std::regex_search(block, match, pattern)) {
        return {};
    }
    const std::string raw = match[1].str();
    std::string value;
    value.reserve(raw.size());
    bool escaped = false;
    for (char ch : raw) {
        if (escaped) {
            value.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else {
            value.push_back(ch);
        }
    }
    if (escaped) {
        value.push_back('\\');
    }
    return sanitize_utf8(value);
}

struct Candidate {
    std::string persona;
    bool preferred = false;
};

}  // namespace

bool streamtunes_persona(const std::wstring& steam_path, std::string& persona, std::wstring& error) {
    persona.clear();
    error.clear();
    if (steam_path.empty() || steam_path.find(L"..") != std::wstring::npos) {
        error = L"Refusing to read a Steam persona because the Steam path is empty or contains '..'.";
        return false;
    }
    const std::wstring path = steam_path + L"\\config\\loginusers.vdf";
    std::string text;
    if (!read_entire_file(path, text, error)) {
        error = L"Could not read the logged-in Steam persona from '" + path + L"'. " + error;
        return false;
    }

    const std::regex user_block("\"[0-9]{10,20}\"\\s*\\{([^}]*)\\}", std::regex::ECMAScript);
    std::vector<Candidate> candidates;
    for (std::sregex_iterator it(text.begin(), text.end(), user_block), end; it != end; ++it) {
        const std::string block = (*it)[1].str();
        const std::string name = value_in(block, "PersonaName");
        if (name.empty() || name.size() > 64) {
            continue;
        }
        const bool preferred = value_in(block, "AutoLogin") == "1" || value_in(block, "MostRecent") == "1";
        candidates.push_back({name, preferred});
    }
    for (const Candidate& candidate : candidates) {
        if (candidate.preferred) {
            persona = candidate.persona;
            return true;
        }
    }
    if (candidates.size() == 1) {
        persona = candidates.front().persona;
        return true;
    }
    error = L"'" + path + L"' did not identify one automatic or most-recent Steam persona.";
    return false;
}

std::wstring streamtunes_relay_for_launch(const std::wstring& exe_path, const std::wstring& configured) {
    if (!configured.empty()) {
        return configured;
    }
    if (path_is_dev_source_tree(exe_path)) {
        return {};
    }
    return L"wss://leu-stadium-bosnia-logic.trycloudflare.com/streamtunes";
}

std::string streamtunes_config_script(const std::wstring& steam_path, const std::wstring& relay_url, std::wstring& error) {
    error.clear();
    if (relay_url.empty()) {
        return {};
    }
    std::string persona;
    if (!streamtunes_persona(steam_path, persona, error)) {
        return {};
    }
    return "window.__doomnetPartyConfig={url:" + json_quote(wide_to_utf8(relay_url)) +
           ",name:" + json_quote(persona) + "};";
}

}  // namespace doomnet
