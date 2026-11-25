#include "game.hpp"

#include <windows.h>

#include <cstring>

namespace model_reload {

namespace {

// The 3D type's and the mesh type's fields (symbols.toml's header).
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

using ClearFn = void (*)(void* province_graphics);
using SpritesFn = void (*)(void* province_graphics, void* province, void* map, bool remove_only);

// The province graphics' clear (province_graphics_clear), then the city and port
// sprites' creation (create_province_sprites) with remove_only: with
// it, the function does not read the province (null here) and returns after
// deleting the city and port and zeroing the key that makes Update create them.
bool call_clear(std::uintptr_t clear, std::uintptr_t sprites, std::uintptr_t graphics, std::uintptr_t map) {
    __try {
        reinterpret_cast<ClearFn>(clear)(reinterpret_cast<void*>(graphics));
        reinterpret_cast<SpritesFn>(sprites)(reinterpret_cast<void*>(graphics), nullptr,
                                             reinterpret_cast<void*>(map), true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The size of a province's graphics (the game's code multiplies by 5 << 10).
constexpr std::uintptr_t kProvinceGraphicsSize = 0x1400;
constexpr std::int32_t kMaxProvinces = 1 << 16;
constexpr std::uint32_t kMaxUnitSlots = 64;

// The objects the clear leaves (symbols.toml's last section).
constexpr std::uintptr_t kTempEnvoySize = 0x18;     // a province's envoy entry
constexpr std::uintptr_t kTempEnvoyGraphics = 0x8;  // its envoy graphics, a pointer
constexpr std::uintptr_t kEnvoySize = 0xC8;         // an envoy object
constexpr std::uintptr_t kEnvoyProvince = 0x48;
constexpr std::uintptr_t kEnvoyInstance = 0x50;
constexpr std::uintptr_t kEnvoyOwnerValid = 0x65;  // the owner tag's last byte
constexpr std::uintptr_t kGoodsSize = 0x90;        // a moving trade good
constexpr std::uintptr_t kGoodsType = 0x10;        // 0 naval, 1 land
constexpr std::uintptr_t kGoodsInstance = 0x78;
constexpr std::uintptr_t kAmbientInstance = 0x8;  // a visible ambient object
constexpr std::uintptr_t kAmbientRemove = 0x10;
constexpr std::size_t kMaxObjects = 1u << 16;  // per array: more is not a real one

// A CArray (begin, end, capacity) of `size`-byte elements; false when it
// cannot be read or does not look like one.
bool read_array(std::uintptr_t at, std::uintptr_t size, std::uintptr_t& begin, std::size_t& count) {
    std::uintptr_t end = 0;
    if (!read(at, begin) || !read(at + 8, end) || end < begin || (end - begin) % size != 0) return false;
    count = static_cast<std::size_t>((end - begin) / size);
    return count <= kMaxObjects && (count == 0 || begin != 0);
}

using RecreateFn = void (*)(void* owner, std::uint32_t index);

bool call_recreate(std::uintptr_t fn, std::uintptr_t owner, std::uint32_t index) {
    __try {
        reinterpret_cast<RecreateFn>(fn)(reinterpret_cast<void*>(owner), index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// The envoys of one province: every envoy object with an instance gets it
// again from the envoy's recreate (envoy_recreate), which reads the entity anew. One whose
// owner tag is not valid is skipped: the function would delete its instance
// and create none, which the game itself never asks for.
bool GameMemory::recreate_envoys(std::uintptr_t graphics, int& count) {
    std::uintptr_t temps = 0;
    std::size_t n = 0;
    if (!read_array(graphics + a_.envoy_graphics, kTempEnvoySize, temps, n)) return false;
    for (std::size_t t = 0; t < n; ++t) {
        std::uintptr_t envoy_graphics = 0, objects = 0;
        std::size_t m = 0;
        if (!read(temps + t * kTempEnvoySize + kTempEnvoyGraphics, envoy_graphics)) return false;
        if (envoy_graphics == 0) continue;
        if (!read_array(envoy_graphics + a_.envoys, kEnvoySize, objects, m)) return false;
        for (std::size_t i = 0; i < m; ++i) {
            const std::uintptr_t e = objects + i * kEnvoySize;
            std::uintptr_t instance = 0, province = 0;
            std::uint8_t owner_valid = 0;
            if (!read(e + kEnvoyInstance, instance) || !read(e + kEnvoyProvince, province) ||
                !read(e + kEnvoyOwnerValid, owner_valid)) {
                return false;
            }
            if (instance == 0 || (province != 0 && owner_valid == 0)) continue;
            if (!call_recreate(a_.envoy_recreate, envoy_graphics, static_cast<std::uint32_t>(i))) return false;
            ++count;
        }
    }
    return true;
}

// The trade goods on the map's trade routes: each with an instance and a type
// the function knows (naval or land) is created again.
bool GameMemory::recreate_trade_goods(std::uintptr_t map, int& count) {
    std::uintptr_t routes = 0;
    std::size_t n = 0;
    if (!read_array(map + a_.trade_routes, 8, routes, n)) return false;
    for (std::size_t r = 0; r < n; ++r) {
        std::uintptr_t route = 0, goods = 0;
        std::size_t m = 0;
        if (!read(routes + r * 8, route)) return false;
        if (route == 0) continue;
        if (!read_array(route + a_.trade_goods, kGoodsSize, goods, m)) return false;
        for (std::size_t i = 0; i < m; ++i) {
            const std::uintptr_t g = goods + i * kGoodsSize;
            std::uintptr_t instance = 0;
            std::uint32_t type = 0;
            if (!read(g + kGoodsInstance, instance) || !read(g + kGoodsType, type)) return false;
            if (instance == 0 || type > 1) continue;
            if (!call_recreate(a_.trade_goods_recreate, route, static_cast<std::uint32_t>(i))) return false;
            ++count;
        }
    }
    return true;
}

// The visible ambient objects: each is marked as when its time runs out, so
// the game's next map update destroys its instance and creates the object's
// instance again while its cell is drawn.
bool GameMemory::remove_ambient(std::uintptr_t map, int& count) {
    std::uintptr_t objects = 0;
    std::size_t n = 0;
    if (!read_array(map + a_.ambient_objects, 8, objects, n)) return false;
    const std::uint8_t yes = 1;
    for (std::size_t i = 0; i < n; ++i) {
        std::uintptr_t object = 0, instance = 0;
        if (!read(objects + i * 8, object)) return false;
        if (object == 0) continue;
        if (!read(object + kAmbientInstance, instance)) return false;
        if (instance == 0) continue;
        if (!write_raw(object + kAmbientRemove, &yes, sizeof yes)) return false;
        ++count;
    }
    return true;
}

MapRefresh GameMemory::refresh_map_objects() {
    MapRefresh out;
    std::uintptr_t state = 0, owner = 0, app = 0, map = 0, data = 0;
    std::int32_t count = 0;
    if (!read(a_.game_state, state) || state == 0 || !read(state + a_.graphics_owner, owner) || owner == 0 ||
        !read(owner + a_.application, app) || app == 0 || !read(app + a_.map, map) || map == 0 ||
        !read(map + a_.provinces, data) || data == 0 || !read(map + a_.province_count, count) || count < 0 ||
        count > kMaxProvinces || a_.unit_slots == 0 || a_.unit_slots > kMaxUnitSlots || a_.province_clear == 0 ||
        a_.province_sprites == 0) {
        return out;
    }
    out.available = true;
    // From 1, as the graphical map's province graphics update: province 0 is never drawn.
    for (std::int32_t p = 1; p < count; ++p) {
        const std::uintptr_t graphics = data + kProvinceGraphicsSize * static_cast<std::uintptr_t>(p);
        for (std::uint32_t s = 0; s < a_.unit_slots; ++s) {
            std::uintptr_t instance = 0;
            if (!read(graphics + a_.units + 8ull * s, instance)) {
                out.faulted = true;
                return out;
            }
            if (instance != 0) ++out.instances;
        }
        if (!call_clear(a_.province_clear, a_.province_sprites, graphics, map)) {
            out.faulted = true;
            return out;
        }
        ++out.provinces;
    }
    // Clear leaves these; a build without their symbols skips them.
    if (a_.envoy_recreate != 0 && a_.envoy_graphics != 0 && a_.envoys != 0) {
        for (std::int32_t p = 1; p < count; ++p) {
            if (!recreate_envoys(data + kProvinceGraphicsSize * static_cast<std::uintptr_t>(p), out.envoys)) {
                out.faulted = true;
                return out;
            }
        }
    }
    if (a_.trade_goods_recreate != 0 && a_.trade_routes != 0 && a_.trade_goods != 0 &&
        !recreate_trade_goods(map, out.trade_goods)) {
        out.faulted = true;
        return out;
    }
    if (a_.ambient_objects != 0 && !remove_ambient(map, out.ambient)) out.faulted = true;
    return out;
}

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
