// The pdxmesh entries of an interface .gfx file, read by the plugin itself
// (docs/model-reload.md). The game's own .gfx reader is not used:
// `reload guianim` runs it with temporary tables and crashes the game.
//
// The fields are the ones the game's mesh type and 3D type readers
// read (EU4 1.37.5): name, file, scale, cull_distance and animation = { id
// type }. A field left out has the value a new mesh type starts with (scale
// 1.0, cull distance 430), as when the game reads the file at start. Other
// keys of a pdxmesh block (meshsettings, the rest) are kept by name only: the
// plugin does not reload them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace model_reload {

inline constexpr float kDefaultScale = 1.0f;
inline constexpr float kDefaultCullDistance = 430.0f;  // the type starts with 430² = 184900

struct AnimationEntry {
    std::string id;    // "idle", "attack": the game keeps its hash
    std::string type;  // the animation's name, from an .asset file
};

struct MeshEntry {
    std::string name;
    std::string file;  // with '\\' made '/', as the game does
    bool has_file = false;
    float scale = kDefaultScale;
    float cull_distance = kDefaultCullDistance;
    std::vector<AnimationEntry> animations;
    std::vector<std::string> other_keys;  // keys this plugin does not reload, in file order, once each
    int line = 0;                         // of `pdxmesh`, 1-based
    bool malformed = false;               // a value that is not a number, a block where a value belongs
};

struct GfxFile {
    std::vector<MeshEntry> meshes;  // in file order
    bool unbalanced = false;        // braces that do not close: nothing of the file is used
};

GfxFile parse_gfx(std::string_view text);

// The game's animation id hash: FNV-1 over the id's bytes (signed chars),
// 32 bits, kept in a 64-bit field of the lookup entry (+0x10).
std::uint64_t animation_hash(std::string_view id);

// The cull distance as the type keeps it, squared (+0x34).
inline float cull_square(float distance) { return distance * distance; }

}  // namespace model_reload
