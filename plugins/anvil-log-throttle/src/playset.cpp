#include "playset.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tally.hpp"

namespace script_errors {

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Every key="value" of a key in a .mod file, in order.
std::vector<std::string> values_of(std::string_view text, std::string_view key) {
    std::vector<std::string> out;
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
        if (at != 0 && !std::isspace(static_cast<unsigned char>(text[at - 1]))) continue;
        const std::size_t q = text.find('"', at + key.size());
        const std::size_t eq = text.find('=', at + key.size());
        if (q == std::string_view::npos || eq == std::string_view::npos || eq > q) continue;
        const std::size_t end = text.find('"', q + 1);
        if (end == std::string_view::npos) break;
        out.emplace_back(text.substr(q + 1, end - q - 1));
        at = end;
    }
    return out;
}

// The first key="value" of a key in a .mod file.
std::string value_of(std::string_view text, std::string_view key) {
    std::vector<std::string> values = values_of(text, key);
    return values.empty() ? std::string{} : std::move(values.front());
}

// "events", "common/ideas": forward slashes, no slash at either end.
std::string folder_key(std::string folder) {
    std::replace(folder.begin(), folder.end(), '\\', '/');
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
    return folder;
}

// Whether a later layer replaces `folder` (replace_path), so the game reads
// none of the earlier layers' files under it.
bool replaced_after(const std::vector<Layer>& layers, std::size_t index, const std::string& folder) {
    for (std::size_t j = index + 1; j < layers.size(); ++j) {
        for (const std::string& r : layers[j].replaced) {
            if (folder == r || folder.starts_with(r + "/")) return true;
        }
    }
    return false;
}

// In an event key or an option's name ('ns.1', 'ns.1.a') a dot is part of
// the name, so 'ns.1' is not found inside 'ns.1.a'.
bool word_char(char c, bool dots) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (dots && (c == '.' || c == '-'));
}

bool word_at(std::string_view line, std::size_t at, std::size_t size, bool dots) {
    if (at > 0 && word_char(line[at - 1], dots)) return false;
    return at + size >= line.size() || !word_char(line[at + size], dots);
}

std::string_view trimmed_right(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// '{' less '}'.
int braces(std::string_view code) {
    int n = 0;
    for (const char c : code) n += c == '{' ? 1 : c == '}' ? -1 : 0;
    return n;
}

// Whether the use of a name at `at` in a line's code defines it (FindIn);
// `depth` is the nesting of braces there.
bool defines(std::string_view code, std::size_t at, std::size_t size, int depth, FindIn in) {
    switch (in) {
    case FindIn::script: return code.find("event_target_as") != std::string_view::npos;
    case FindIn::events: {
        // "id = <key>" in a top-level event, "name = <option>" in its option; deeper,
        // "country_event = { id = <key> }" calls the event.
        std::string_view before = trimmed_right(code.substr(0, at));
        if (before.empty() || before.back() != '=') return false;
        before = trimmed_right(before.substr(0, before.size() - 1));
        for (const std::string_view key : {"id", "name"}) {
            if (depth == (key == "id" ? 1 : 2) && before.ends_with(key) &&
                (before.size() == key.size() || !word_char(before[before.size() - key.size() - 1], false))) {
                return true;
            }
        }
        return false;
    }
    case FindIn::missions: {
        // "<mission> = {" at the start of a line, in a series.
        if (depth != 1 || !trimmed_right(code.substr(0, at)).empty()) return false;
        std::size_t i = at + size;
        while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        return i < code.size() && code[i] == '=';
    }
    default: return false;
    }
}

}  // namespace

std::filesystem::path path_of(std::string_view text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w.data(), n);
    return std::filesystem::path(w);
}

std::string utf8_of(const std::filesystem::path& path) {
    const std::wstring& w = path.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path user_dir() {
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    const std::size_t at = line.find(key);
    if (at != std::wstring::npos) {
        std::size_t from = at + key.size();
        const bool quoted = from < line.size() && line[from] == L'"';
        if (quoted) ++from;
        std::size_t end = line.find(quoted ? L'"' : L' ', from);
        if (end == std::wstring::npos) end = line.size();
        return std::filesystem::path(line.substr(from, end - from));
    }
    PWSTR docs = nullptr;
    std::filesystem::path out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        out = std::filesystem::path(docs) / L"Paradox Interactive" / L"Europa Universalis IV";
    }
    CoTaskMemFree(docs);
    return out;
}

std::filesystem::path game_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::vector<Layer> playset_layers(const std::filesystem::path& dlc_load, const std::filesystem::path& user,
                                  const std::filesystem::path& game) {
    std::vector<Layer> layers;
    if (!game.empty()) layers.push_back({"game", game});
    const std::string json = read_file(dlc_load);
    const std::size_t list = json.find("\"enabled_mods\"");
    if (list == std::string::npos) return layers;
    const std::size_t open = json.find('[', list);
    const std::size_t close = open == std::string::npos ? std::string::npos : json.find(']', open);
    if (close == std::string::npos) return layers;
    const std::string_view items(json.data() + open + 1, close - open - 1);
    for (std::size_t q = items.find('"'); q != std::string_view::npos; q = items.find('"', q + 1)) {
        const std::size_t end = items.find('"', q + 1);
        if (end == std::string_view::npos) break;
        const std::string mod_file(items.substr(q + 1, end - q - 1));
        q = end;
        const std::string mod = read_file(user / path_of(mod_file));
        const std::string name = value_of(mod, "name");
        const std::string path = value_of(mod, "path");
        if (path.empty()) continue;
        std::filesystem::path p = path_of(path);
        if (p.is_relative()) p = user / p;
        Layer& added = layers.emplace_back();
        added.name = name.empty() ? mod_file : log_throttle::utf8_text(name);
        added.path = p;
        for (std::string& r : values_of(mod, "replace_path")) {
            if (std::string key = folder_key(std::move(r)); !key.empty()) added.replaced.push_back(std::move(key));
        }
    }
    return layers;
}

bool resolve(const std::vector<Layer>& layers, std::string_view file, std::string& path, std::string& layer) {
    if (file.empty()) return false;
    const std::filesystem::path rel = path_of(file);
    if (rel.is_absolute()) return false;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(it->path / rel, ec)) {
            path = utf8_of(it->path / rel);
            layer = it->name;
            return true;
        }
    }
    return false;
}

Search search(const std::vector<Layer>& layers, std::string_view token, FindIn in) {
    Search out;
    if (token.empty() || in == FindIn::none) return out;
    static const std::vector<const wchar_t*> kScript = {L"events", L"common", L"decisions", L"missions",
                                                         L"history/countries"};
    static const std::vector<const wchar_t*> kEvents = {L"events"};
    static const std::vector<const wchar_t*> kMissions = {L"missions"};
    const std::vector<const wchar_t*>& folders =
        in == FindIn::events ? kEvents : in == FindIn::missions ? kMissions : kScript;
    const bool dots = in != FindIn::script;
    std::vector<Hit> uses;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const Layer& layer = layers[index];
        for (const wchar_t* folder : folders) {
            if (replaced_after(layers, index, folder_key(utf8_of(folder)))) continue;
            std::error_code ec;
            const std::filesystem::path dir = layer.path / folder;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != L".txt") continue;
                const std::string text = read_file(it->path());
                if (text.find(token) == std::string::npos) continue;
                bool counted_file = false;
                int number = 0;
                int depth = 0;  // of braces, at the start of the line
                for (std::size_t start = 0; start < text.size();) {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    ++number;
                    const std::string_view line(text.data() + start, end - start);
                    start = end + 1;
                    const std::string_view code = line.substr(0, line.find('#'));
                    for (std::size_t at = code.find(token); at != std::string_view::npos;
                         at = code.find(token, at + 1)) {
                        if (!word_at(code, at, token.size(), dots)) continue;
                        ++out.total;
                        if (!counted_file) {
                            ++out.files;
                            counted_file = true;
                        }
                        Hit h;
                        h.abs = utf8_of(it->path());
                        // Lexical: a game folder of junctions stays under the layer.
                        std::string rel = utf8_of(it->path().lexically_relative(layer.path));
                        std::replace(rel.begin(), rel.end(), '\\', '/');
                        h.rel = rel;
                        h.line = number;
                        h.defines = defines(code, at, token.size(), depth + braces(code.substr(0, at)), in);
                        if (h.defines) {
                            out.hits.push_back(std::move(h));
                        } else if (uses.size() < 8) {
                            uses.push_back(std::move(h));
                        }
                        break;
                    }
                    depth = std::max(0, depth + braces(code));
                }
            }
        }
    }
    if (out.hits.size() > 6) out.hits.resize(6);
    for (Hit& h : uses) {
        if (out.hits.size() >= 10) break;
        out.hits.push_back(std::move(h));
    }
    return out;
}

std::string vscode_uri(std::string_view utf8_path, int line) {
    std::string out = "vscode://file/";
    for (const char ch : utf8_path) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\\') {
            out += '/';
        } else if (std::isalnum(c) || c == '/' || c == ':' || c == '.' || c == '_' || c == '-') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    if (line > 0) out += ":" + std::to_string(line);
    return out;
}

}  // namespace script_errors
