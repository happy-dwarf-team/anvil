#include "game.hpp"

#include <windows.h>

#include <cstring>

namespace model_reload {

namespace {

// CPdx3DType and CPdxMeshType (PDB layouts, symbols.toml's header).
constexpr std::uintptr_t kKey = 0x10;
constexpr std::uintptr_t kScale = 0x30;
constexpr std::uintptr_t kCull = 0x34;
constexpr std::uintptr_t kFilename = 0xA0;
constexpr std::uintptr_t kLookup = 0xC0;  // begin, end, capacity
constexpr std::uintptr_t kLookupSize = 0x40;
constexpr std::uintptr_t kLookupHash = 0x10;
constexpr std::uintptr_t kLookupName = 0x20;

constexpr std::uint32_t kMaxBuckets = 1u << 22;
constexpr std::size_t kMaxTypes = 1u << 20;
constexpr std::size_t kMaxLookups = 4096;
constexpr std::uint64_t kMaxText = 4096;
constexpr std::uint64_t kMaxAnswer = 1 << 20;

// No C++ objects with destructors in the __try functions.
bool read_raw(std::uintptr_t at, void* out, std::size_t n) {
    if (at == 0) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_raw(std::uintptr_t at, const void* in, std::size_t n) {
    if (at == 0) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(at), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> bool read(std::uintptr_t at, T& out) { return read_raw(at, &out, sizeof(T)); }

// The game's MSVC std::string: a 16-byte buffer or a pointer, the size, the capacity.
struct RawString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;
};
static_assert(sizeof(RawString) == 0x20);

bool read_string(std::uintptr_t at, std::string& out) {
    RawString s;
    if (!read(at, s) || s.size > s.capacity || s.size > kMaxText) return false;
    out.resize(static_cast<std::size_t>(s.size));
    if (s.size == 0) return true;
    const auto data = s.capacity >= 16 ? reinterpret_cast<std::uintptr_t>(s.pointer) : at;
    return read_raw(data, out.data(), out.size());
}

using ReloadFn = void (*)(void* type);
using AssignFn = void* (*)(void* text, const char* chars, std::uint64_t count);

bool call_reload(std::uintptr_t fn, std::uintptr_t type) {
    __try {
        reinterpret_cast<ReloadFn>(fn)(reinterpret_cast<void*>(type));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_assign(std::uintptr_t fn, std::uintptr_t text, const char* chars, std::uint64_t count) {
    __try {
        reinterpret_cast<AssignFn>(fn)(reinterpret_cast<void*>(text), chars, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The console's types, as in Tongs: a vector of strings and the result.
struct RawVector {
    RawString* begin;
    RawString* end;
    RawString* capacity_end;
};
struct RawResult {
    std::uint8_t success;
    std::uint8_t padding[7];
    RawString text;
};
static_assert(sizeof(RawResult) == 0x28);
using DispatchFn = RawResult* (*)(void* manager, RawResult* result, RawVector* tokens);
using StringFreeFn = void (*)(RawString* text);

bool call_dispatch(std::uintptr_t fn, std::uintptr_t manager, RawResult* result, RawVector* tokens) {
    __try {
        reinterpret_cast<DispatchFn>(fn)(reinterpret_cast<void*>(manager), result, tokens);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_free(std::uintptr_t fn, RawString* text) {
    __try {
        reinterpret_cast<StringFreeFn>(fn)(text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool GameMemory::mesh_types(std::vector<MeshType>& out) {
    out.clear();
    std::uintptr_t graphics = 0, buckets = 0;
    std::uint32_t count = 0;
    if (!read(a_.graphics, graphics) || graphics == 0 || !read(graphics + a_.bucket_count, count) ||
        !read(graphics + a_.buckets, buckets) || count > kMaxBuckets || (count != 0 && buckets == 0)) {
        return false;
    }
    std::size_t nodes = 0;
    for (std::uint32_t b = 0; b < count; ++b) {
        std::uintptr_t node = 0;
        if (!read(buckets + 8ull * b, node)) return false;
        while (node != 0) {
            if (++nodes > kMaxTypes) return false;
            std::uintptr_t type = 0, next = 0, vtable = 0;
            std::uint32_t id = 0;
            if (!read(node, type) || !read(node + 8, next)) return false;
            if (type != 0 && read(type, vtable) && read(type + 8, id) && vtable == a_.mesh_vtable &&
                id == a_.mesh_type_id) {
                MeshType t;
                t.address = type;
                std::uintptr_t begin = 0, end = 0;
                if (!read_string(type + kKey, t.name) || !read_string(type + kFilename, t.file) ||
                    !read(type + kScale, t.scale) || !read(type + kCull, t.cull_square) ||
                    !read(type + kLookup, begin) || !read(type + kLookup + 8, end) || end < begin ||
                    (end - begin) % kLookupSize != 0 || (end - begin) / kLookupSize > kMaxLookups) {
                    return false;
                }
                for (std::uintptr_t e = begin; e < end; e += kLookupSize) {
                    Lookup l;
                    std::uint64_t hash = 0;
                    if (!read(e + kLookupHash, hash) || !read_string(e + kLookupName, l.name)) return false;
                    l.hash = static_cast<std::uint32_t>(hash);
                    t.lookups.push_back(std::move(l));
                }
                out.push_back(std::move(t));
            }
            node = next;
        }
    }
    return true;
}

bool GameMemory::set_scale(std::uintptr_t type, float scale) { return write_raw(type + kScale, &scale, sizeof scale); }

bool GameMemory::set_cull_square(std::uintptr_t type, float value) {
    return write_raw(type + kCull, &value, sizeof value);
}

bool GameMemory::set_file(std::uintptr_t type, std::string_view file) {
    const std::string copy(file);  // the game copies it (memmove), also over its own buffer
    return call_assign(a_.string_assign, type + kFilename, copy.c_str(), copy.size());
}

bool GameMemory::set_lookup_name(std::uintptr_t type, std::size_t index, std::string_view name) {
    std::uintptr_t begin = 0, end = 0;
    if (!read(type + kLookup, begin) || !read(type + kLookup + 8, end) || end < begin ||
        index >= (end - begin) / kLookupSize) {
        return false;
    }
    const std::string copy(name);
    return call_assign(a_.string_assign, begin + index * kLookupSize + kLookupName, copy.c_str(), copy.size());
}

bool GameMemory::reload_type(std::uintptr_t type) {
    std::uintptr_t vtable = 0;
    if (!read(type, vtable) || vtable != a_.mesh_vtable) return false;
    return call_reload(a_.reload_type, type);
}

ConsoleAnswer GameMemory::console(const std::vector<std::string>& tokens) {
    ConsoleAnswer out;
    std::uintptr_t manager = 0;
    if (tokens.empty() || !read(a_.console_manager, manager) || manager == 0) return out;
    out.ran = true;
    // Inline copies of short tokens, pointers into `tokens` for long ones: the
    // dispatcher only reads them and copies the arguments it passes on.
    std::vector<RawString> strings(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        RawString& s = strings[i];
        std::memset(&s, 0, sizeof s);
        s.size = tokens[i].size();
        if (s.size < 16) {
            std::memcpy(s.buffer, tokens[i].c_str(), tokens[i].size() + 1);
            s.capacity = 15;
        } else {
            s.pointer = tokens[i].c_str();
            s.capacity = s.size;
        }
    }
    RawVector vector{strings.data(), strings.data() + strings.size(), strings.data() + strings.size()};
    RawResult result;
    std::memset(&result, 0, sizeof result);
    if (!call_dispatch(a_.console_dispatch, manager, &result, &vector)) {
        out.faulted = true;
        return out;  // the result may be half built; it is not freed
    }
    out.success = result.success != 0;
    const RawString& text = result.text;
    if (text.size <= text.capacity && text.size <= kMaxAnswer) {
        const char* data = text.capacity >= 16 ? text.pointer : text.buffer;
        out.text.resize(static_cast<std::size_t>(text.size));
        if (text.size > 0 && !read_raw(reinterpret_cast<std::uintptr_t>(data), out.text.data(), out.text.size())) {
            out.text.clear();
        }
    }
    if (!call_free(a_.string_free, &result.text)) out.faulted = true;
    return out;
}

}  // namespace model_reload
