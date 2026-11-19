// The files a model modder saves, and what reloads each (docs/model-reload.md).
//
// The plugin looks only in the enabled mods' folders (dlc_load.json in the user
// folder and each mod's descriptor), and there only in gfx/models,
// gfx/entities, gfx/particles and interface. A file counts as changed when it
// is new or its size or time differs from the last look; a deleted file is
// not a change. Nothing here touches the game.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace model_reload {

enum class Kind {
    model_texture,     // .dds/.tga under gfx/models: `reloadtexture <path>`
    mesh,              // .mesh: the plugin reloads every mesh type whose file it is
    animation,         // .anim: `reload anim`
    particle_texture,  // .dds/.tga under gfx/particles: `reload particle`
    asset,             // .asset (entities, particles): `reload assets`
    gfx,               // .gfx under interface: the plugin's reload of pdxmesh entries
};

// `relative` with '/' as written on disk, from the mod's root. Other files
// (flags, sprites, interface textures) are not the plugin's.
std::optional<Kind> classify(std::string_view relative);

struct ModFolder {
    std::string name;  // the descriptor's name, UTF-8
    std::filesystem::path root;
};

struct Mods {
    std::vector<ModFolder> folders;
    std::vector<std::string> problems;  // a mod that cannot be watched, in English for the log
};

// The enabled mods, in the launcher's order. `user_dir` is the game's user
// folder (where dlc_load.json and mod/ are).
Mods enabled_mods(const std::filesystem::path& user_dir);

struct FileKey {
    std::size_t mod = 0;   // index into Mods::folders
    std::string relative;  // as on disk, '/'
    bool operator<(const FileKey& o) const { return mod != o.mod ? mod < o.mod : relative < o.relative; }
};

struct FileStamp {
    std::uintmax_t size = 0;
    std::int64_t time = 0;  // last write, the file system's ticks
    bool operator==(const FileStamp&) const = default;
};

using Snapshot = std::map<FileKey, FileStamp>;

// The plugin's files under the scanned folders of each mod, at most
// `limit` of them (a mod with more is cut, and `cut` is set).
Snapshot scan(const std::vector<ModFolder>& mods, std::size_t limit, bool* cut);

struct Change {
    FileKey key;
    Kind kind = Kind::mesh;
};

// New or changed files of `now` against `before`, by kind, then path.
std::vector<Change> changes(const Snapshot& before, const Snapshot& now);

// One step of a reload, in the order the plugin runs them.
struct Step {
    enum class What {
        reload_texture,  // console `reloadtexture <path>`, one per texture
        reload_mesh,     // the plugin: mesh types whose file is `path`
        reload_anim,     // console `reload anim`, once
        reload_particle, // console `reload particle`, once
        reload_assets,   // console `reload assets`, once
        reload_gfx,      // the plugin: the pdxmesh entries of `path`
    };
    What what = What::reload_mesh;
    std::string path;  // relative, '/' (empty for the once-only steps)
    std::size_t mod = 0;
    std::vector<FileKey> files;  // reload_anim and reload_particle: the changed files they read
};

// Whether the first bytes of a file (at most 32) can be the file its name
// says: a .mesh or .anim starts with the PDX asset mark "@@b@", a .dds with
// "DDS ", a .tga has its 18-byte header. Other files are not looked at.
bool plausible_file(std::string_view relative, std::string_view head);

// Textures, meshes, animations, particles, entities, then .gfx entries: a
// .gfx entry looks its animations up by name, and the console's reloads come
// first. The game's `reload texture` (every texture) and `reload guianim` are
// never part of it. `all` is the snapshot the changes came from: a .gfx that a
// later mod has too is not the one the game reads, and is skipped.
std::vector<Step> plan(const std::vector<Change>& changes, const Snapshot& all);

bool iequals(std::string_view a, std::string_view b);
std::string lower(std::string_view s);

}  // namespace model_reload
