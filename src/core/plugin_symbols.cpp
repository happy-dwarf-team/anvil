#include "plugin_symbols.hpp"

namespace anvil {

PluginSymbols::PluginSymbols(const SymbolFile& file, const SymbolVerification& verified, uintptr_t image_base) {
    symbols_.reserve(file.symbols.size());
    for (size_t i = 0; i < file.symbols.size() && i < verified.symbols.size(); ++i) {
        PluginSymbol s;
        s.declaration = file.symbols[i];
        s.rva = verified.symbols[i].rva;
        s.address = image_base + s.rva;
        s.values = verified.symbols[i].values;
        symbols_.push_back(std::move(s));
    }
}

void* PluginSymbols::address_of(uint32_t id) const {
    const PluginSymbol* s = get(id);
    return s != nullptr ? reinterpret_cast<void*>(s->address) : nullptr;
}

std::optional<uint64_t> PluginSymbols::value(uint32_t id, std::string_view name) const {
    const PluginSymbol* s = get(id);
    if (s == nullptr) return std::nullopt;
    for (const auto& [n, v] : s->values) {
        if (n == name) return v;
    }
    return std::nullopt;
}

ChangeTarget target_of(const PluginSymbol& symbol) {
    ChangeTarget t;
    t.address = symbol.address;
    t.inside_image = true;
    t.expected = symbol.declaration.bytes.data();
    t.mask = symbol.declaration.mask.empty() ? nullptr : symbol.declaration.mask.data();
    t.expected_size = symbol.declaration.bytes.size();
    return t;
}

ChangeStatus hook_function(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t fn,
                           void* detour, void** next, ChangeId* out) {
    const PluginSymbol* s = symbols.get(fn);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind != SymbolKind::Function) return ChangeStatus::WrongKind;
    return registry.hook_function(owner, target_of(*s), detour, next, out);
}

ChangeStatus redirect_call(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t call_site,
                           void* target, void** next, ChangeId* out) {
    const PluginSymbol* s = symbols.get(call_site);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind != SymbolKind::Site) return ChangeStatus::WrongKind;
    return registry.redirect_call(owner, target_of(*s), 0, target, next, out);
}

ChangeStatus patch_bytes(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t site,
                         const void* bytes, size_t size, ChangeId* out) {
    const PluginSymbol* s = symbols.get(site);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind == SymbolKind::Slot) return ChangeStatus::WrongKind;  // swap_pointer (#44)
    if (size == 0 || size > s->declaration.bytes.size()) return ChangeStatus::InvalidArgument;
    return registry.patch_bytes(owner, target_of(*s), bytes, size, out);
}

}  // namespace anvil
