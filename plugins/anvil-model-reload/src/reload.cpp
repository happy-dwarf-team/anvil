#include "reload.hpp"

#include <algorithm>

namespace model_reload {

namespace {

std::string_view folder_of(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash + 1);
}

StepResult console_step(Game& game, const Step& step, const std::vector<std::string>& tokens) {
    StepResult r;
    r.step = step;
    const ConsoleAnswer a = game.console(tokens);
    r.answer = a.text;
    if (a.faulted) {
        r.status = Status::faulted;
    } else if (!a.ran) {
        r.status = Status::no_console;
    } else {
        r.status = a.success ? Status::ok : Status::failed;
    }
    return r;
}

StepResult mesh_step(Game& game, const Step& step) {
    StepResult r;
    r.step = step;
    std::vector<MeshType> types;
    if (!game.mesh_types(types)) {
        r.status = Status::no_table;
        return r;
    }
    for (const MeshType& t : types) {
        if (!iequals(t.file, step.path)) continue;
        if (!game.reload_type(t.address)) {
            r.status = Status::faulted;
            return r;
        }
        ++r.count;
    }
    r.status = r.count > 0 ? Status::ok : Status::nothing;
    return r;
}

// One pdxmesh entry against the type the game looks up by its name.
TypeStatus apply_entry(Game& game, const MeshEntry& e, const MeshType& t, const RunInput& in) {
    if (e.malformed) return TypeStatus::malformed;
    if (e.animations.size() != t.lookups.size()) return TypeStatus::animations;
    for (std::size_t i = 0; i < e.animations.size(); ++i) {
        if (static_cast<std::uint32_t>(animation_hash(e.animations[i].id)) != t.lookups[i].hash) {
            return TypeStatus::animations;
        }
    }
    const bool file = e.has_file && e.file != t.file;
    if (file && (!in.file_exists || !in.file_exists(e.file))) return TypeStatus::missing_file;
    const bool scale = e.scale != t.scale;
    const float cull = cull_square(e.cull_distance);
    const bool cull_changed = cull != t.cull_square;
    bool names = false;
    for (std::size_t i = 0; i < e.animations.size(); ++i) names |= e.animations[i].type != t.lookups[i].name;
    if (!file && !scale && !cull_changed && !names) return TypeStatus::unchanged;
    if (scale && !game.set_scale(t.address, e.scale)) return TypeStatus::faulted;
    if (cull_changed && !game.set_cull_square(t.address, cull)) return TypeStatus::faulted;
    if (file && !game.set_file(t.address, e.file)) return TypeStatus::faulted;
    for (std::size_t i = 0; i < e.animations.size(); ++i) {
        if (e.animations[i].type != t.lookups[i].name && !game.set_lookup_name(t.address, i, e.animations[i].type)) {
            return TypeStatus::faulted;
        }
    }
    return game.reload_type(t.address) ? TypeStatus::updated : TypeStatus::faulted;
}

StepResult gfx_step(Game& game, const Step& step, const RunInput& in) {
    StepResult r;
    r.step = step;
    std::string text;
    if (!in.read_file || !in.read_file(step, text)) {
        r.status = Status::unreadable;
        return r;
    }
    const GfxFile file = parse_gfx(text);
    if (file.unbalanced) {
        r.status = Status::unreadable;
        return r;
    }
    std::vector<MeshType> types;
    if (!file.meshes.empty() && !game.mesh_types(types)) {
        r.status = Status::no_table;
        return r;
    }
    for (std::size_t i = 0; i < file.meshes.size(); ++i) {
        const MeshEntry& e = file.meshes[i];
        for (const std::string& k : e.other_keys) {
            if (std::find(r.ignored_keys.begin(), r.ignored_keys.end(), k) == r.ignored_keys.end()) {
                r.ignored_keys.push_back(k);
            }
        }
        // Two blocks of one name in a file: the game looks up the later one.
        bool later = false;
        for (std::size_t j = i + 1; j < file.meshes.size(); ++j) later |= file.meshes[j].name == e.name;
        if (later) continue;
        TypeNote note{e.name, TypeStatus::not_found};
        if (e.name.empty()) {
            note.status = TypeStatus::malformed;
        } else {
            const auto t = std::find_if(types.begin(), types.end(), [&](const MeshType& m) { return m.name == e.name; });
            if (t != types.end()) note.status = apply_entry(game, e, *t, in);
        }
        if (note.status == TypeStatus::updated) {
            ++r.count;
        } else if (note.status == TypeStatus::unchanged) {
            ++r.unchanged;
        } else {
            r.types.push_back(note);
        }
        if (note.status == TypeStatus::faulted) {
            r.status = Status::faulted;
            return r;
        }
    }
    r.status = Status::ok;
    return r;
}

// The first file of the step that is empty or not in the game's format, or "".
std::string bad_file_of(const Step& step, const RunInput& in) {
    if (!in.read_head) return {};
    const auto bad = [&](std::size_t mod, const std::string& relative) {
        std::string head;
        return !in.read_head(mod, relative, head) || !plausible_file(relative, head);
    };
    switch (step.what) {
    case Step::What::reload_texture:
    case Step::What::reload_mesh: return bad(step.mod, step.path) ? step.path : std::string();
    case Step::What::reload_anim:
    case Step::What::reload_particle:
        for (const FileKey& f : step.files) {
            if (bad(f.mod, f.relative)) return f.relative;
        }
        return {};
    default: return {};
    }
}

}  // namespace

std::string texture_name(std::string_view relative, const std::vector<MeshType>& types) {
    const std::string_view folder = folder_of(relative);
    for (const MeshType& t : types) {
        const std::string_view f = folder_of(t.file);
        if (!f.empty() && iequals(f, folder)) return std::string(f) + std::string(relative.substr(folder.size()));
    }
    return std::string(relative);
}

RunResult run(Game& game, const RunInput& in) {
    RunResult out;
    bool assets = false;
    for (const Step& step : in.steps) {
        if (out.faulted) {
            StepResult skipped;
            skipped.step = step;
            skipped.status = Status::not_run;
            out.steps.push_back(std::move(skipped));
            continue;
        }
        // A file the game would read that is empty or not in its format (a
        // save caught half written, a wrong export): the step is left out, so
        // the game's loaders never see it.
        if (const std::string bad = bad_file_of(step, in); !bad.empty()) {
            StepResult skipped;
            skipped.step = step;
            skipped.status = Status::bad_file;
            skipped.bad_file = bad;
            out.steps.push_back(std::move(skipped));
            continue;
        }
        StepResult r;
        switch (step.what) {
        case Step::What::reload_texture: {
            std::vector<MeshType> types;
            game.mesh_types(types);  // only for the folder's spelling
            const std::string name = texture_name(step.path, types);
            r = console_step(game, step, {"reloadtexture", name});
            r.texture = name;
            break;
        }
        case Step::What::reload_mesh: r = mesh_step(game, step); break;
        case Step::What::reload_anim: r = console_step(game, step, {"reload", "anim"}); break;
        case Step::What::reload_particle: r = console_step(game, step, {"reload", "particle"}); break;
        case Step::What::reload_assets:
            r = console_step(game, step, {"reload", "assets"});
            assets |= r.status == Status::ok;
            break;
        case Step::What::reload_gfx: r = gfx_step(game, step, in); break;
        }
        out.faulted |= r.status == Status::faulted;
        out.steps.push_back(std::move(r));
    }
    // Once, after every step: the map objects are created from the entities as
    // the run left them.
    if (assets && !out.faulted) {
        const MapRefresh u = game.refresh_map_objects();
        out.faulted = u.faulted;
        if (u.faulted) {
            // Shown on the assets step, whose follow-up it is: save and restart.
            for (StepResult& r : out.steps) {
                if (r.step.what == Step::What::reload_assets) r.status = Status::faulted;
            }
        }
        out.map_refreshed = u.available && !u.faulted;
        out.unit_instances = u.instances;
        out.provinces = u.provinces;
        out.envoys = u.envoys;
        out.trade_goods = u.trade_goods;
        out.ambient = u.ambient;
        out.load_a_save = !out.map_refreshed;
    } else if (assets) {
        out.load_a_save = true;
    }
    return out;
}

}  // namespace model_reload
