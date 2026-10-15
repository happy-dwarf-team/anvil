#include "expectations.hpp"

#include <cstdio>

namespace anvil {

std::string check_expectations(const Manifest& m, const char* build, const PatchRegistry& patches,
                               uintptr_t image_base, size_t& checked) {
    checked = 0;
    for (const auto& e : m.expectations) {
        if (e.build != "*" && (build == nullptr || e.build != build)) continue;
        std::string detail;
        const AnvilResult r =
            patches.expect_bytes(m.id, image_base + e.rva, e.bytes.data(), e.mask.data(), e.bytes.size(), &detail);
        if (r == ANVIL_OK) {
            ++checked;
            continue;
        }
        char where[48];
        std::snprintf(where, sizeof(where), "rva 0x%X, %zu B", e.rva, e.bytes.size());
        const std::string label = e.name.empty() ? std::string(where) : e.name + " (" + where + ")";
        if (r == ANVIL_ERROR_BYTES_MISMATCH) {
            return "game code differs from what the plugin expects at " + label + ", " + detail +
                   " (EU4 updated or eu4.exe modified?)";
        }
        return "expected bytes at " + label + " lie outside the game image or are not readable";
    }
    return {};
}

}  // namespace anvil
