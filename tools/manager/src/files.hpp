#pragma once

// The file helpers moved to the core (src/core/files.hpp), which also uses
// them for approved copies of mod plugins (design §14).
#include "core/files.hpp"

namespace anvil::manager {

using anvil::list_files;
using anvil::remove_tree;
using anvil::write_file_atomic;

}  // namespace anvil::manager
