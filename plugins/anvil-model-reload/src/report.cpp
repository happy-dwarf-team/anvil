#include "report.hpp"

namespace model_reload {

namespace {

Row text(std::string label) {
    Row r;
    r.label = std::move(label);
    return r;
}

Row pair(std::string label, std::string value) {
    Row r;
    r.label = std::move(label);
    r.value = std::move(value);
    r.has_value = true;
    return r;
}

Row separator() {
    Row r;
    r.kind = kSeparator;
    return r;
}

std::string join(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (const std::string& s : items) {
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

const char* step_label(const Step& s) {
    switch (s.what) {
    case Step::What::reload_anim: return "reload anim";
    case Step::What::reload_particle: return "reload particle";
    case Step::What::reload_assets: return "reload assets";
    default: return nullptr;
    }
}

std::string status_text(const StepResult& r, const Texts& t) {
    switch (r.status) {
    case Status::ok:
        if (r.step.what == Step::What::reload_mesh) return t.f("{0} mesh types reloaded", {std::to_string(r.count)});
        if (r.step.what == Step::What::reload_gfx) {
            return t.f("{0} mesh types updated, {1} unchanged", {std::to_string(r.count), std::to_string(r.unchanged)});
        }
        return r.answer.empty() ? std::string(t("Done")) : r.answer;
    case Status::failed: return t.f("The console answered: {0}", {r.answer});
    case Status::no_console: return t("The game has no console now.");
    case Status::no_table: return t("The game's mesh types cannot be read.");
    case Status::nothing: return t("No mesh type of the game uses this file.");
    case Status::unreadable: return t("The file cannot be read, or its braces do not close.");
    case Status::faulted: return t("An error in the game's code: the rest did not run. Save and restart the game.");
    case Status::not_run: return t("Not run");
    }
    return {};
}

const char* type_text(TypeStatus s) {
    switch (s) {
    case TypeStatus::not_found: return N_("A new mesh type: it needs a restart.");
    case TypeStatus::animations: return N_("Its animation ids changed: it needs a restart.");
    case TypeStatus::missing_file: return N_("Its file is in no mod and not in the game: left as it was.");
    case TypeStatus::malformed: return N_("A value cannot be read: left as it was.");
    case TypeStatus::faulted: return N_("An error in the game's code.");
    default: return "";
    }
}

const char* status_word(Status s) {
    switch (s) {
    case Status::ok: return "ok";
    case Status::failed: return "failed";
    case Status::no_console: return "no console";
    case Status::no_table: return "no mesh type table";
    case Status::nothing: return "no mesh type uses it";
    case Status::unreadable: return "unreadable";
    case Status::faulted: return "FAULTED";
    case Status::not_run: return "not run";
    }
    return "?";
}

const char* type_word(TypeStatus s) {
    switch (s) {
    case TypeStatus::updated: return "updated";
    case TypeStatus::unchanged: return "unchanged";
    case TypeStatus::not_found: return "not in the game (restart)";
    case TypeStatus::animations: return "animation ids changed (restart)";
    case TypeStatus::missing_file: return "file not found";
    case TypeStatus::malformed: return "malformed";
    case TypeStatus::faulted: return "FAULTED";
    }
    return "?";
}

}  // namespace

const char* action_of(Kind kind) {
    switch (kind) {
    case Kind::model_texture: return "reloadtexture";
    case Kind::mesh: return "mesh";
    case Kind::animation: return "reload anim";
    case Kind::particle_texture: return "reload particle";
    case Kind::asset: return "reload assets";
    case Kind::gfx: return "pdxmesh";
    }
    return "";
}

std::vector<Row> rows(const PanelState& s, const Texts& t) {
    std::vector<Row> out;
    out.push_back(text(t("For model modders: reloads the meshes, model textures, animations, entities and .gfx mesh "
                         "entries you saved, in the running game.")));
    out.push_back(pair(t("Mods watched"), s.mods.empty() ? std::string(t("none")) : join(s.mods, ", ")));
    if (s.mod_problems > 0) {
        out.push_back(text(t.f("{0} enabled mods cannot be watched; anvil.log names them.", {std::to_string(s.mod_problems)})));
    }
    if (s.cut) out.push_back(text(t.f("More than {0} files: only the first are watched.", {std::to_string(kScanLimit)})));
    out.push_back(separator());
    if (!s.looked) {
        out.push_back(text(t("Looking at the mods' files...")));
    } else if (s.changed.empty()) {
        out.push_back(text(t("No file changed since the last reload.")));
    } else {
        out.push_back(pair(t("Changed since the last reload"), std::to_string(s.changed.size())));
        for (std::size_t i = 0; i < s.changed.size() && i < kFilesShown; ++i) {
            out.push_back(pair(s.changed[i].key.relative, action_of(s.changed[i].kind)));
        }
        if (s.changed.size() > kFilesShown) {
            out.push_back(text(t.f("{0} more", {std::to_string(s.changed.size() - kFilesShown)})));
        }
    }
    switch (s.mode) {
    case Mode::menu: out.push_back(text(t("Load a game to reload."))); break;
    case Mode::ironman:
    case Mode::multiplayer: out.push_back(text(t("Not in ironman or multiplayer games."))); break;
    case Mode::game:
        if (s.running) {
            out.push_back(text(t("Reloading...")));
        } else if (s.looked && !s.changed.empty()) {
            Row b;
            b.kind = kButton;
            b.label = t("Reload the changed files");
            b.id = kReload;
            out.push_back(std::move(b));
        }
        break;
    }
    if (!s.has_last) return out;
    out.push_back(separator());
    out.push_back(pair(t("Last reload"), s.last_time));
    for (const StepResult& r : s.last.steps) {
        const char* label = step_label(r.step);
        out.push_back(pair(label != nullptr ? std::string(label) : r.step.path, status_text(r, t)));
        for (const TypeNote& n : r.types) out.push_back(pair("    " + n.name, t(type_text(n.status))));
        if (!r.ignored_keys.empty()) {
            out.push_back(pair("    pdxmesh", t.f("Not reloaded: {0}", {join(r.ignored_keys, ", ")})));
        }
    }
    if (s.last.map_refreshed) {
        out.push_back(text(t("Units, cities, ports and flags on the map are drawn again with the reloaded entities.")));
    }
    if (s.last.load_a_save) {
        out.push_back(text(t("Objects already on the map keep their old entity: load a save to see the change.")));
    }
    return out;
}

std::vector<std::string> log_lines(const RunResult& result) {
    std::vector<std::string> out;
    for (const StepResult& r : result.steps) {
        std::string line;
        switch (r.step.what) {
        case Step::What::reload_texture: line = "reloadtexture " + r.texture; break;
        case Step::What::reload_mesh: line = "mesh " + r.step.path; break;
        case Step::What::reload_anim: line = "reload anim"; break;
        case Step::What::reload_particle: line = "reload particle"; break;
        case Step::What::reload_assets: line = "reload assets"; break;
        case Step::What::reload_gfx: line = "pdxmesh " + r.step.path; break;
        }
        line += ": ";
        line += status_word(r.status);
        if (r.step.what == Step::What::reload_mesh && r.status == Status::ok) {
            line += ", " + std::to_string(r.count) + " mesh types reloaded";
        }
        if (r.step.what == Step::What::reload_gfx && r.status == Status::ok) {
            line += ", " + std::to_string(r.count) + " updated, " + std::to_string(r.unchanged) + " unchanged";
        }
        for (const TypeNote& n : r.types) line += "; " + n.name + " " + type_word(n.status);
        if (!r.ignored_keys.empty()) line += "; not reloaded: " + join(r.ignored_keys, ", ");
        if (!r.answer.empty()) line += " (console: " + r.answer + ")";
        out.push_back(std::move(line));
    }
    if (result.map_refreshed) {
        out.push_back("entities reloaded: map objects of " + std::to_string(result.provinces) + " provinces deleted (" +
                      std::to_string(result.unit_instances) + " unit instances), the game creates them again; " +
                      std::to_string(result.envoys) + " envoys and " + std::to_string(result.trade_goods) +
                      " trade goods created again, " + std::to_string(result.ambient) +
                      " ambient objects removed for the game to create again");
    }
    if (result.load_a_save) out.push_back("entities reloaded: objects on the map show them after a save load");
    return out;
}

}  // namespace model_reload
