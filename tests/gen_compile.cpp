// Compiles the C++ header `anvil gen` writes for tests/fixtures/gen/symbols.toml
// (the design §5.2 example) and checks what it declares at compile time.
// gen_compile.c does the same for the C header.
struct Root;  // types named in symbols.toml are declared before the header

#include "symbols.gen.hpp"

#include <cstdio>
#include <string_view>
#include <type_traits>

static_assert(sym::speed_object.id == 0 && sym::physfs_get_write_dir.id == 4);
static_assert(std::is_same_v<decltype(sym::speed_object)::Signature, void*(void*)>);
static_assert(std::is_same_v<decltype(sym::physfs_get_write_dir)::Pointer, const char* (*)()>);
static_assert(std::is_same_v<decltype(sym::person_name_block), const anvil::Site>);
static_assert(std::is_same_v<decltype(sym::frontend_tick_slot), const anvil::Slot>);
static_assert(std::is_base_of_v<anvil::Site, sym::Types::root_and_date> && sym::root_and_date.id == 3);
static_assert(sym::root_and_date.root == 0x233FE78 && sym::root_and_date.date_offset == 0x1DD0);
static_assert(std::is_same_v<decltype(sym::root)::Type, Root> && sym::root.id == 3);
static_assert(std::string_view(sym::root.value) == "root");

extern "C" int c_last_symbol();  // gen_compile.c

int main() {
    if (c_last_symbol() != 4) {
        std::printf("the C header disagrees with the C++ header\n");
        return 1;
    }
    std::printf("generated headers compile as C and C++\n");
    return 0;
}
