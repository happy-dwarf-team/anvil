#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// Writes `bytes` to `file` through a temporary file next to it, replacing it
// in one step; creates the folder. False with `error` (and `access_denied`
// set when Windows refused access).
bool write_file_atomic(const std::filesystem::path& file, std::string_view bytes, std::string& error,
                       bool* access_denied = nullptr);

// The files below `folder`, and the links (junctions, symbolic links) as
// entries of their own: nothing behind a link is listed.
void list_files(const std::filesystem::path& folder, std::vector<std::filesystem::path>& out);

// Removes `path` and what is in it without following links: a junction or
// symbolic link is removed itself, never what it points to. Read-only files
// go too. 0, or the Windows error of the first thing that could not go; a
// missing path is 0.
DWORD remove_tree(const std::filesystem::path& path);

}  // namespace anvil
