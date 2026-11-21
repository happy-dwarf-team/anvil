#include "crash_hints.hpp"

namespace anvil_game::crash_hints {

namespace {

constexpr std::int64_t kMaxProvinces = 1'000'000;  // a longer vector is taken as garbage

class Text {
public:
    Text(char* out, std::size_t capacity) : out_(out), capacity_(capacity) {}
    Text& put(const char* text) {
        while (*text != '\0' && size_ < capacity_) out_[size_++] = *text++;
        return *this;
    }
    Text& dec(std::int64_t value) {
        char digits[24];
        std::size_t n = 0;
        const bool negative = value < 0;
        std::uint64_t v = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        if (negative) put("-");
        while (n > 0 && size_ < capacity_) out_[size_++] = digits[--n];
        return *this;
    }
    Text& hex(std::uint64_t value) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        char digits[16];
        std::size_t n = 0;
        do {
            digits[n++] = kDigits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        put("0x");
        while (n > 0 && size_ < capacity_) out_[size_++] = digits[--n];
        return *this;
    }
    std::size_t size() const { return size_; }

private:
    char* out_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

template <typename T>
bool read_value(Reader read, std::uintptr_t address, T& out) {
    return read(address, &out, sizeof(T));
}

// The province array and its entry count, or false when the game has none yet.
bool read_provinces(const ProvinceLayout& l, Reader read, std::uintptr_t& array, std::int64_t& count) {
    if (l.root == 0 || l.province_size == 0) return false;
    std::uintptr_t root = 0, begin = 0, end = 0;
    if (!read_value(read, l.root, root) || root == 0) return false;
    if (!read_value(read, root + l.provinces, begin) || !read_value(read, root + l.provinces_end, end) ||
        end < begin) {
        return false;
    }
    count = static_cast<std::int64_t>((end - begin) >> l.pointer_shift);
    if (count < 2 || count > kMaxProvinces) return false;
    return read_value(read, root + l.province_array, array) && array != 0;
}

// Whether `value` is among the 8-byte words of the stack from rsp up.
bool on_stack(Reader read, std::uintptr_t rsp, std::uintptr_t high, std::uintptr_t value) {
    const std::uintptr_t base = rsp & ~std::uintptr_t{7};
    if (rsp == 0 || high <= base) return false;
    const std::uintptr_t end = high - base > kStackBytes ? base + kStackBytes : high;
    std::uintptr_t words[512];
    for (std::uintptr_t at = base; at < end;) {
        std::uintptr_t next = (at & ~std::uintptr_t{0xFFF}) + 0x1000;
        if (next > end) next = end;
        const std::size_t n = static_cast<std::size_t>((next - at) / 8);
        if (n > 0 && read(at, words, n * 8)) {
            for (std::size_t i = 0; i < n; ++i) {
                if (words[i] == value) return true;
            }
        }
        at = next;
    }
    return false;
}

}  // namespace

ProvinceEntry province_entry(std::uintptr_t array, std::int64_t count, std::uint32_t size, std::uintptr_t address) {
    ProvinceEntry e;
    e.count = count;
    if (array == 0 || size == 0 || count < 2) return e;
    std::int64_t id = 0;
    if (address >= array) {
        const std::uint64_t entry = (address - array) / size;
        if (entry < static_cast<std::uint64_t>(count) ||
            entry >= static_cast<std::uint64_t>(count) + static_cast<std::uint64_t>(kReach)) {
            return e;
        }
        id = static_cast<std::int64_t>(entry);
    } else {
        const std::uint64_t before = (array - address + size - 1) / size;  // entries before the array, rounded up
        if (before > static_cast<std::uint64_t>(kReach)) return e;
        id = -static_cast<std::int64_t>(before);
    }
    e.found = true;
    e.id = id;
    e.offset = static_cast<std::uint64_t>(address - (array + static_cast<std::uintptr_t>(id) * size));
    return e;
}

std::size_t write_note(const ProvinceLayout& layout, std::uintptr_t image_base, std::uintptr_t idea_return,
                       const Crash& crash, Reader read, char* out, std::size_t capacity) {
    Text text(out, capacity);
    std::uintptr_t array = 0;
    std::int64_t count = 0;
    if (crash.access_violation && read_provinces(layout, read, array, count)) {
        const ProvinceEntry e = province_entry(array, count, layout.province_size, crash.address);
        if (e.found) {
            text.put("The game ")
                .put(crash.write ? "wrote to " : "read ")
                .hex(crash.address)
                .put(", where the entry of province ")
                .dec(e.id)
                .put(" would be (")
                .hex(e.offset)
                .put(" into it), but this map has provinces 1 to ")
                .dec(e.count - 1)
                .put(".\n");
            text.put("The game does not check the province IDs that script or a save gives it: a trigger or effect "
                     "with province ")
                .dec(e.id)
                .put(" (such as owns = ")
                .dec(e.id)
                .put(") most likely caused this crash, and the owners of script, if listed, say where it ran "
                     "(anvil#426).\n");
        }
    }
    // A missing idea is a null pointer, so the merge reads a field just past 0. The return address alone
    // could be a stale copy left on the stack by an earlier call.
    if (idea_return != 0 && crash.access_violation && !crash.write && crash.address < kNullReach &&
        on_stack(read, crash.rsp, crash.stack_high, idea_return)) {
        text.put("The stack holds a return address of the loop that adds up a country's idea modifiers (eu4.exe+")
            .hex(idea_return - image_base)
            .put(", CalcTechAndIdeaModifiers).\n");
        text.put("A save with more ideas taken in an idea group than the group has makes that loop read ideas "
                 "past the group's end, and the game does not check that count when it loads a save "
                 "(active_idea_groups, anvil#426).\n");
    }
    return text.size();
}

}  // namespace anvil_game::crash_hints
