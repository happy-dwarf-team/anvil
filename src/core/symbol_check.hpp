#pragma once

#include "changes.hpp"
#include "i18n/message.hpp"
#include "pe_image.hpp"
#include "symbols.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace anvil {

// The running eu4.exe as the game shipped it: bytes that plugins have already
// changed read in their original form, so two plugins can depend on the same
// function (design §7).
class OriginalImageReader final : public ImageReader {
public:
    OriginalImageReader(const ChangeRegistry& patches, uintptr_t image_base) : patches_(patches), base_(image_base) {}
    bool read(uint32_t rva, void* out, size_t size) const override {
        return patches_.read_original(base_ + rva, out, size);
    }
    uint64_t base() const override { return base_; }

private:
    const ChangeRegistry& patches_;
    uintptr_t base_;
};


}  // namespace anvil
