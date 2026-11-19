// A reload run: the steps of plan() against the game, behind an interface that
// the tests replace (docs/model-reload.md). The real one (game.hpp) reads and
// writes the game's memory and calls its console on the main thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "files.hpp"
#include "gfx_file.hpp"

namespace model_reload {

struct Lookup {
    std::uint32_t hash = 0;  // the id's hash, low 32 bits of _nHash
    std::string name;        // the animation's name (_Name)
};

// One CPdxMeshType of the game's gfx type table, as read.
struct MeshType {
    std::uintptr_t address = 0;
    std::string name;  // _Key
    std::string file;  // _Filename
    float scale = 1.0f;
    float cull_square = 0.0f;
    std::vector<Lookup> lookups;
};

struct ConsoleAnswer {
    bool ran = false;      // false: the game has no console manager now
    bool success = false;  // the dispatcher's own flag
    bool faulted = false;  // an exception in the game's code: stop
    std::string text;
};

struct MapRefresh {
    bool available = false;  // false: no map now (the menu) or the province graphics cannot be read
    bool faulted = false;    // an exception in the game's code: stop
    int instances = 0;       // unit instances among the objects deleted
    int provinces = 0;       // provinces whose map objects were deleted
    int envoys = 0;          // envoys on the map created again
    int trade_goods = 0;     // trade goods on the trade routes created again
    int ambient = 0;         // ambient objects (eagles, windmills, whales) the game removes and creates again
};

class Game {
public:
    virtual ~Game() = default;
    // Every mesh type, in the table's order (for one name, the one the game
    // looks up comes first). False: the table cannot be read.
    virtual bool mesh_types(std::vector<MeshType>& out) = 0;
    // The writes and the reload of one type; false: an exception in the game's
    // code (the run stops).
    virtual bool set_scale(std::uintptr_t type, float scale) = 0;
    virtual bool set_cull_square(std::uintptr_t type, float value) = 0;
    virtual bool set_file(std::uintptr_t type, std::string_view file) = 0;
    virtual bool set_lookup_name(std::uintptr_t type, std::size_t index, std::string_view name) = 0;
    virtual bool reload_type(std::uintptr_t type) = 0;
    virtual ConsoleAnswer console(const std::vector<std::string>& tokens) = 0;
    // Deletes the 3D instances on the map (units, cities, ports, flags, city
    // sprawl) and the map icons of every province, as the game does for a
    // province it clears; the game creates them again from the reloaded
    // entities when it next draws each province. Then creates the envoys and
    // the trade goods on the trade routes again, and marks the ambient objects
    // for the game to remove and create again.
    virtual MapRefresh refresh_map_objects() = 0;
};

enum class Status {
    ok,
    failed,       // the console said no (its answer is kept)
    no_console,   // the game has no console manager now
    no_table,     // the gfx type table cannot be read
    nothing,      // a mesh no type uses
    unreadable,   // a .gfx file that cannot be read or does not parse
    faulted,      // an exception in the game's code: the steps after it did not run
    not_run,      // after a fault
};

enum class TypeStatus {
    updated,      // fields written and the type reloaded
    unchanged,    // the same values as in the game
    not_found,    // a type the game does not have: a restart adds it
    animations,   // a changed count or order of animation ids: a restart
    missing_file, // `file` names a file no mod and the game have: left as it was
    malformed,    // a value the plugin cannot read: left as it was
    faulted,
};

struct TypeNote {
    std::string name;
    TypeStatus status = TypeStatus::updated;
};

struct StepResult {
    Step step;
    Status status = Status::ok;
    int count = 0;           // mesh types reloaded (mesh and .gfx steps)
    std::string answer;      // the console's text
    std::string texture;     // reload_texture: the name passed to the console
    std::vector<TypeNote> types;               // .gfx: every type that was not simply updated or unchanged
    int unchanged = 0;                         // .gfx
    std::vector<std::string> ignored_keys;     // .gfx: keys of its pdxmesh blocks not reloaded
};

struct RunResult {
    std::vector<StepResult> steps;
    bool faulted = false;
    // After a `reload assets` that succeeded: the objects on the map (units,
    // cities, ports, flags) are created again (map_refreshed, with the number of
    // provinces and of the unit instances among them), or that was not possible
    // and they keep the old entity until a save load (load_a_save).
    bool map_refreshed = false;
    int provinces = 0;
    int unit_instances = 0;
    int envoys = 0;
    int trade_goods = 0;
    int ambient = 0;
    bool load_a_save = false;
};

struct RunInput {
    std::vector<Step> steps;
    std::vector<ModFolder> mods;
    // A .gfx file's text by its step (mod and path); false when it cannot be read.
    std::function<bool(const Step&, std::string&)> read_file;
    // Whether a game path (as `file =` writes it) exists in a mod or in the game.
    std::function<bool(std::string_view)> file_exists;
};

RunResult run(Game& game, const RunInput& input);

// The texture name `reloadtexture` gets for a file of a mod: the folder as the
// game's mesh types spell it when one of them is in that folder (the game
// builds a model texture's name from its mesh's file), else as on disk.
std::string texture_name(std::string_view relative, const std::vector<MeshType>& types);

}  // namespace model_reload
