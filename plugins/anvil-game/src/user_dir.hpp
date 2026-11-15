// The game's write folder as user_dir() gives it (include/anvil/anvil_game.h).
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace anvil_game {

// PhysFS' write directory as the game set it (UTF-8, which PhysFS uses on
// Windows; text that is not valid UTF-8 is taken in the ANSI code page) as a
// wide path with backslashes and no trailing one ("C:\" keeps its own).
// Empty for an empty name.
inline std::wstring user_dir_path(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = static_cast<int>(utf8.size());
    UINT page = CP_UTF8;
    int n = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (n <= 0) {
        page = CP_ACP;
        n = MultiByteToWideChar(page, 0, utf8.data(), length, nullptr, 0);
        if (n <= 0) return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(page, page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, utf8.data(), length, out.data(), n);
    for (wchar_t& c : out) {
        if (c == L'/') c = L'\\';
    }
    while (out.size() > 1 && out.back() == L'\\' && !(out.size() == 3 && out[1] == L':')) out.pop_back();
    return out;
}

}  // namespace anvil_game
