#include "symbol_check.hpp"

namespace anvil {

std::string check_plugin_symbols(const std::filesystem::path& symbols_file, const char* build_id,
                                 const PatchRegistry& patches, uintptr_t image_base, SymbolVerification* verified) {
    const SymbolFileResult parsed = load_symbols(symbols_file);
    if (!parsed.file) return parsed.error;
    if (build_id == nullptr) return "game build unknown (EU4 updated?)";
    if (parsed.file->build != build_id) {
        return "symbols.toml is for build " + parsed.file->build + ", the game is " + build_id;
    }
    const OriginalImageReader reader(patches, image_base);
    std::string error;
    const auto image = PeImage::parse(reader, error);
    if (!image) return "cannot read the game image: " + error;
    SymbolVerification result = verify_symbols(*parsed.file, *image);
    if (!result.ok()) {
        std::string reason = result.problems.front() + " (EU4 updated or eu4.exe modified?)";
        if (result.problems.size() > 1) reason += " and " + std::to_string(result.problems.size() - 1) + " more";
        return reason;
    }
    if (verified != nullptr) *verified = std::move(result);
    return {};
}

}  // namespace anvil
