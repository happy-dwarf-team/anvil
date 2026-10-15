#pragma once

#include "manifest.hpp"
#include "patching.hpp"

#include <cstdint>
#include <string>

namespace anvil {

// Compares the [[expect]] entries of `m` that apply to `build` (entries for
// "*" always apply; with an unknown build, nullptr, only those). Returns an
// empty string when everything matches, otherwise why the plugin must not be
// loaded. `checked` receives the number of entries compared.
std::string check_expectations(const Manifest& m, const char* build, const PatchRegistry& patches,
                               uintptr_t image_base, size_t& checked);

}  // namespace anvil
