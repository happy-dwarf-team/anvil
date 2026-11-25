// anvil-model-reload (plugins/anvil-model-reload) without the game: its .gfx
// reader, the mods' files and their changes, the steps of a reload, a run
// against a stand-in of the game's mesh types and console, the window's rows
// and its texts. The game's memory and console (game.cpp) and the window's
// thread are tried in the game (the pull request that added the plugin).
#include "../plugins/anvil-model-reload/src/files.hpp"
#include "../plugins/anvil-model-reload/src/gfx_file.hpp"
#include "../plugins/anvil-model-reload/src/reload.hpp"
#include "../plugins/anvil-model-reload/src/report.hpp"
#include "../plugins/anvil-model-reload/src/texts.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

void run(const char* name, const std::function<void()>& body) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

namespace mr = model_reload;
namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// ---- a stand-in of the game -------------------------------------------------

struct FakeGame : mr::Game {
    std::vector<mr::MeshType> types;
    bool table = true;
    std::vector<std::string> calls;
    std::uintptr_t fault_on_reload = 0;
    std::map<std::string, mr::ConsoleAnswer> answers;  // by the joined line
    bool console_present = true;

    mr::MeshType* find(std::uintptr_t a) {
        for (mr::MeshType& t : types) {
            if (t.address == a) return &t;
        }
        return nullptr;
    }
    bool mesh_types(std::vector<mr::MeshType>& out) override {
        if (!table) return false;
        out = types;
        return true;
    }
    bool set_scale(std::uintptr_t a, float s) override {
        calls.push_back("scale " + find(a)->name + " " + std::to_string(s));
        find(a)->scale = s;
        return true;
    }
    bool set_cull_square(std::uintptr_t a, float v) override {
        calls.push_back("cull " + find(a)->name + " " + std::to_string(static_cast<int>(v)));
        find(a)->cull_square = v;
        return true;
    }
    bool set_file(std::uintptr_t a, std::string_view f) override {
        calls.push_back("file " + find(a)->name + " " + std::string(f));
        find(a)->file = std::string(f);
        return true;
    }
    bool set_lookup_name(std::uintptr_t a, std::size_t i, std::string_view n) override {
        calls.push_back("anim " + find(a)->name + " " + std::to_string(i) + " " + std::string(n));
        find(a)->lookups.at(i).name = std::string(n);
        return true;
    }
    bool reload_type(std::uintptr_t a) override {
        calls.push_back("reload " + find(a)->name);
        return a != fault_on_reload;
    }
    mr::ConsoleAnswer console(const std::vector<std::string>& tokens) override {
        std::string line;
        for (const std::string& t : tokens) line += (line.empty() ? "" : " ") + t;
        calls.push_back("console " + line);
        if (!console_present) return {};
        if (const auto it = answers.find(line); it != answers.end()) return it->second;
        return mr::ConsoleAnswer{true, true, false, tokens[0] == "reloadtexture" ? tokens[1] + " reloaded" : "ok"};
    }
    mr::MapRefresh units{true, false, 7, 4940, 12, 30, 5};
    mr::MapRefresh refresh_map_objects() override {
        calls.push_back("refresh map");
        return units;
    }
};

mr::Lookup lookup(const char* id, const char* name) {
    return mr::Lookup{static_cast<std::uint32_t>(mr::animation_hash(id)), name};
}

FakeGame vanilla() {
    FakeGame g;
    mr::MeshType a;
    a.address = 0x1000;
    a.name = "RUS_INFANTRY_1";
    a.file = "gfx/models/Units/RUS_01.mesh";
    a.scale = 0.5f;
    a.cull_square = mr::cull_square(430);
    a.lookups = {lookup("idle", "rifleman_idle_animation"), lookup("attack", "rifleman_attack_animation")};
    mr::MeshType b = a;
    b.address = 0x2000;
    b.name = "RUS_INFANTRY_2";
    b.file = "gfx/models/Units/RUS_02.mesh";
    mr::MeshType c = a;  // a second type of the first one's file
    c.address = 0x3000;
    c.name = "RUS_INFANTRY_1_shadow";
    g.types = {a, b, c};
    return g;
}

const char* kEastern = R"(# a test file
objectTypes = {
	pdxmesh = {
		name = "RUS_INFANTRY_1"
		file = "gfx\models\Units\RUS_03.mesh"			# a new file
		animation = { id = "idle"	type = "rifleman_attack_animation" }
		animation = { id = "attack" type = "rifleman_attack_animation" }
		scale = 0.5
		scale = 1.5
		meshsettings = { index = 0 texture_diffuse = "x.dds" }
	}
	pdxmesh = {
		name = "RUS_INFANTRY_2"
		file = "gfx/models/Units/RUS_02.mesh"
		animation = { id = "idle" type = "rifleman_idle_animation" }
		animation = { id = "attack" type = "rifleman_attack_animation" }
		scale = 0.5
	}
	PDXMESH = {
		name = NEW_TYPE
		file = "gfx/models/Units/RUS_02.mesh"
	}
}
spriteTypes = { spriteType = { name = "GFX_x" texturefile = "gfx/interface/x.dds" } }
)";

// ---- texts ------------------------------------------------------------------

std::set<std::string> used_texts(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    std::set<std::string> out;
    for (const std::string call : {"t(\"", "t.f(\"", "N_(\"", "texts()(\""}) {
        for (std::size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) continue;
            // a literal that goes on over lines: "a " "b"
            std::string text;
            std::size_t from = at + call.size();
            for (;;) {
                std::size_t end = from;
                while (end < s.size() && !(s[end] == '"' && s[end - 1] != '\\')) ++end;
                text += s.substr(from, end - from);
                std::size_t next = end + 1;
                while (next < s.size() && std::isspace(static_cast<unsigned char>(s[next]))) ++next;
                if (next < s.size() && s[next] == '"') {
                    from = next + 1;
                    continue;
                }
                break;
            }
            out.insert(text);
        }
    }
    return out;
}

std::vector<std::string> placeholders(const char* text) {
    std::vector<std::string> out;
    for (const char* p = text; *p != 0; ++p) {
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') out.push_back(std::string(p, 3));
    }
    std::sort(out.begin(), out.end());
    return out;
}

const mr::Row* find_row(const std::vector<mr::Row>& rows, const std::string& label) {
    for (const mr::Row& r : rows) {
        if (r.label == label) return &r;
    }
    return nullptr;
}

}  // namespace

int main() {
    run("gfx: pdxmesh blocks at any depth, the last field wins, defaults, other keys", [] {
        const mr::GfxFile f = mr::parse_gfx(kEastern);
        CHECK(!f.unbalanced && f.meshes.size() == 3);
        if (f.meshes.size() != 3) return;
        const mr::MeshEntry& a = f.meshes[0];
        CHECK(a.name == "RUS_INFANTRY_1" && a.has_file && a.file == "gfx/models/Units/RUS_03.mesh" && a.scale == 1.5f &&
              a.cull_distance == 430.0f && a.animations.size() == 2 && a.animations[0].id == "idle" &&
              a.animations[0].type == "rifleman_attack_animation" && a.line == 3 && !a.malformed);
        CHECK(a.other_keys == std::vector<std::string>{"meshsettings"});
        CHECK(f.meshes[1].scale == 0.5f && f.meshes[1].other_keys.empty());
        CHECK(f.meshes[2].name == "NEW_TYPE" && f.meshes[2].scale == 1.0f && f.meshes[2].animations.empty());
    });

    run("gfx: comments, BOM, cull_distance, a bad number, unbalanced braces", [] {
        mr::GfxFile f = mr::parse_gfx("\xEF\xBB\xBFobjectTypes = { pdxmesh = { name = a # x = {\n cull_distance = 200 } }");
        CHECK(!f.unbalanced && f.meshes.size() == 1 && f.meshes[0].cull_distance == 200.0f &&
              mr::cull_square(f.meshes[0].cull_distance) == 40000.0f);
        f = mr::parse_gfx("objectTypes = { pdxmesh = { name = a scale = big } }");
        CHECK(f.meshes.size() == 1 && f.meshes[0].malformed);
        f = mr::parse_gfx("objectTypes = { pdxmesh = { name = a scale = 2 }");
        CHECK(f.unbalanced && f.meshes.empty());
        f = mr::parse_gfx("objectTypes = { } }");
        CHECK(f.unbalanced);
        f = mr::parse_gfx("");
        CHECK(!f.unbalanced && f.meshes.empty());
    });

    run("gfx: the animation id hash is the game's", [] {
        CHECK(mr::animation_hash("idle") == 0x6FB760FFu && mr::animation_hash("attack") == 0x0AC4AEEDu &&
              mr::animation_hash("walk") == 0x7DB16DBEu && mr::animation_hash("") == 0x811C9DC5u);
    });

    run("files: what each path is, case-insensitive", [] {
        CHECK(mr::classify("interface/assets/eastern.gfx") == mr::Kind::gfx);
        CHECK(mr::classify("Interface/Assets/Eastern.GFX") == mr::Kind::gfx);
        CHECK(!mr::classify("interface/eastern.dds"));
        CHECK(mr::classify("gfx/models/Units/RUS_01.mesh") == mr::Kind::mesh);
        CHECK(mr::classify("gfx/models/Units/animation/rifleman_idle.anim") == mr::Kind::animation);
        CHECK(mr::classify("gfx/entities/eastern_units.asset") == mr::Kind::asset);
        CHECK(mr::classify("gfx/particles/fire.asset") == mr::Kind::asset);
        CHECK(mr::classify("gfx/models/Units/RUS_01_diffuse.dds") == mr::Kind::model_texture);
        CHECK(mr::classify("gfx/particles/fire.dds") == mr::Kind::particle_texture);
        CHECK(!mr::classify("gfx/flags/RUS.tga") && !mr::classify("gfx/interface/x.dds") && !mr::classify("common/x.txt"));
    });

    const fs::path temp = fs::temp_directory_path() / ("anvil-model-reload-tests-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(temp);
    const fs::path user = temp / "user";
    const fs::path outside = temp / "outside mod ąć";

    run("files: the enabled mods from dlc_load.json and their descriptors", [&] {
        write(user / "dlc_load.json",
              R"({"enabled_mods":["mod/local.mod","mod/abs.mod","mod/zip.mod","mod/gone.mod","mod/none.mod"],"disabled_dlcs":[]})");
        write(user / "mod/local.mod", "name=\"Local\"\npath=\"mod/local\"\ntags={\n\t\"Graphics\"\n}\n");
        fs::create_directories(user / "mod/local");
        write(user / "mod/abs.mod", "version=\"1\"\nname=\"Outside\"\npath=\"" +
                                        std::string(reinterpret_cast<const char*>(outside.generic_u8string().c_str())) +
                                        "\"\n");
        fs::create_directories(outside);
        write(user / "mod/zip.mod", "name=\"Zipped\"\narchive=\"mod/zipped.zip\"\n");
        write(user / "mod/gone.mod", "name=\"Gone\"\npath=\"mod/gone\"\n");
        const mr::Mods m = mr::enabled_mods(user);
        CHECK(m.folders.size() == 2 && m.problems.size() == 3);
        if (m.folders.size() != 2) return;
        CHECK(m.folders[0].name == "Local" && fs::equivalent(m.folders[0].root, user / "mod/local"));
        CHECK(m.folders[1].name == "Outside" && fs::equivalent(m.folders[1].root, outside));
        CHECK(mr::enabled_mods(temp / "nowhere").problems.size() == 1);
    });

    run("files: a scan, then a saved, a new and an untouched file", [&] {
        const fs::path local = user / "mod/local";
        write(local / "gfx/models/Units/RUS_01.mesh", "mesh");
        write(local / "gfx/models/Units/RUS_01_diffuse.dds", "dds");
        write(local / "gfx/flags/RUS.tga", "flag");
        write(local / "interface/assets/eastern.gfx", kEastern);
        write(local / "interface/eastern.dds", "sprite");
        write(outside / "interface/assets/eastern.gfx", kEastern);
        const mr::Mods m = mr::enabled_mods(user);
        bool cut = true;
        const mr::Snapshot before = mr::scan(m.folders, 1000, &cut);
        CHECK(!cut && before.size() == 4);
        write(local / "gfx/models/Units/RUS_01.mesh", "mesh, longer");
        write(local / "gfx/entities/eastern_units.asset", "asset");
        const mr::Snapshot now = mr::scan(m.folders, 1000, &cut);
        const std::vector<mr::Change> c = mr::changes(before, now);
        CHECK(c.size() == 2);
        if (c.size() == 2) {
            CHECK(c[0].kind == mr::Kind::mesh && c[0].key.relative == "gfx/models/Units/RUS_01.mesh" && c[0].key.mod == 0);
            CHECK(c[1].kind == mr::Kind::asset && c[1].key.relative == "gfx/entities/eastern_units.asset");
        }
        CHECK(mr::changes(now, now).empty());
        mr::scan(m.folders, 2, &cut);
        CHECK(cut);

        // A deleted file is not a change; saved again, it is one.
        fs::remove(local / "gfx/models/Units/RUS_01.mesh");
        const mr::Snapshot gone = mr::scan(m.folders, 1000, &cut);
        CHECK(gone.size() == now.size() - 1 && mr::changes(now, gone).empty());
        write(local / "gfx/models/Units/RUS_01.mesh", "mesh, again");
        const std::vector<mr::Change> back = mr::changes(gone, mr::scan(m.folders, 1000, &cut));
        CHECK(back.size() == 1 && back[0].kind == mr::Kind::mesh && back[0].key.relative == "gfx/models/Units/RUS_01.mesh");
    });

    run("files: the steps in order, once each; a .gfx a later mod has is skipped", [] {
        auto change = [](std::size_t mod, const char* path) {
            return mr::Change{mr::FileKey{mod, path}, *mr::classify(path)};
        };
        std::vector<mr::Change> c = {change(0, "interface/assets/eastern.gfx"), change(1, "interface/assets/eastern.gfx"),
                                     change(0, "interface/assets/western.gfx"), change(0, "gfx/entities/a.asset"),
                                     change(0, "gfx/entities/b.asset"), change(0, "gfx/models/x.anim"),
                                     change(0, "gfx/models/Units/a.mesh"), change(1, "gfx/models/units/A.mesh"),
                                     change(0, "gfx/models/Units/a_diffuse.dds"), change(0, "gfx/particles/fire.dds")};
        mr::Snapshot all;
        for (const mr::Change& x : c) all[x.key] = {};
        all[mr::FileKey{1, "Interface/Assets/Western.gfx"}] = {};  // unchanged in the later mod
        mr::Snapshot before;
        before[mr::FileKey{1, "Interface/Assets/Western.gfx"}] = {};
        const std::vector<mr::Step> s = mr::plan(mr::changes(before, all), all);
        using W = mr::Step::What;
        std::vector<std::string> got;
        for (const mr::Step& x : s) got.push_back(std::to_string(static_cast<int>(x.what)) + ":" + std::to_string(x.mod) + ":" + x.path);
        const std::vector<std::string> want = {
            std::to_string(static_cast<int>(W::reload_texture)) + ":0:gfx/models/Units/a_diffuse.dds",
            std::to_string(static_cast<int>(W::reload_mesh)) + ":0:gfx/models/Units/a.mesh",
            std::to_string(static_cast<int>(W::reload_anim)) + ":0:",
            std::to_string(static_cast<int>(W::reload_particle)) + ":0:",
            std::to_string(static_cast<int>(W::reload_assets)) + ":0:",
            std::to_string(static_cast<int>(W::reload_gfx)) + ":1:interface/assets/eastern.gfx",
        };
        CHECK(got == want);
        for (const std::string& g : got) std::printf("    %s\n", g.c_str());
        for (const mr::Step& x : s) CHECK(x.what != W::reload_gfx || x.path != "interface/assets/western.gfx");
    });

    run("run: a .mesh reloads every type of its file; one no type uses", [] {
        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_mesh, "gfx/models/units/rus_01.MESH", 0},
                    {mr::Step::What::reload_mesh, "gfx/models/Units/none.mesh", 0}};
        const mr::RunResult r = mr::run(g, in);
        CHECK(r.steps.size() == 2 && r.steps[0].status == mr::Status::ok && r.steps[0].count == 2);
        CHECK(r.steps[1].status == mr::Status::nothing && !r.faulted && !r.load_a_save);
        CHECK((g.calls == std::vector<std::string>{"reload RUS_INFANTRY_1", "reload RUS_INFANTRY_1_shadow"}));
    });

    run("run: a .gfx writes only what changed, then reloads; new types and missing files stay", [] {
        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_gfx, "interface/assets/eastern.gfx", 0}};
        in.read_file = [](const mr::Step&, std::string& out) {
            out = kEastern;
            return true;
        };
        std::vector<std::string> asked;
        in.file_exists = [&](std::string_view p) {
            asked.emplace_back(p);
            return true;
        };
        mr::RunResult r = mr::run(g, in);
        CHECK(r.steps.size() == 1 && r.steps[0].status == mr::Status::ok && r.steps[0].count == 1 &&
              r.steps[0].unchanged == 1);
        CHECK(r.steps[0].types.size() == 1 && r.steps[0].types[0].name == "NEW_TYPE" &&
              r.steps[0].types[0].status == mr::TypeStatus::not_found);
        CHECK(r.steps[0].ignored_keys == std::vector<std::string>{"meshsettings"});
        CHECK((g.calls == std::vector<std::string>{"scale RUS_INFANTRY_1 1.500000", "file RUS_INFANTRY_1 gfx/models/Units/RUS_03.mesh",
                                                    "anim RUS_INFANTRY_1 0 rifleman_attack_animation", "reload RUS_INFANTRY_1"}));
        CHECK(asked == std::vector<std::string>{"gfx/models/Units/RUS_03.mesh"});
        // The same file again: nothing to write.
        g.calls.clear();
        r = mr::run(g, in);
        CHECK(r.steps[0].count == 0 && r.steps[0].unchanged == 2 && g.calls.empty());
        // A file that is nowhere.
        g = vanilla();
        in.file_exists = [](std::string_view) { return false; };
        r = mr::run(g, in);
        CHECK(r.steps[0].count == 0 && r.steps[0].types.size() == 2 &&
              r.steps[0].types[0].status == mr::TypeStatus::missing_file && g.calls.empty());
    });

    run("run: changed animation ids, a bad value, a cull distance, the later of two blocks", [] {
        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_gfx, "interface/assets/a.gfx", 0}};
        std::string text =
            "objectTypes = {\n"
            " pdxmesh = { name = RUS_INFANTRY_1 animation = { id = walk type = x } animation = { id = attack type = y } }\n"
            " pdxmesh = { name = RUS_INFANTRY_2 scale = wide }\n"
            " pdxmesh = { name = RUS_INFANTRY_1_shadow scale = 9 }\n"
            " pdxmesh = { name = RUS_INFANTRY_1_shadow scale = 0.5 cull_distance = 100\n"
            "   animation = { id = idle type = rifleman_idle_animation } animation = { id = attack type = rifleman_attack_animation } }\n"
            "}\n";
        in.read_file = [&](const mr::Step&, std::string& out) {
            out = text;
            return true;
        };
        const mr::RunResult r = mr::run(g, in);
        CHECK(r.steps[0].status == mr::Status::ok && r.steps[0].count == 1 && r.steps[0].types.size() == 2);
        if (r.steps[0].types.size() == 2) {
            CHECK(r.steps[0].types[0].status == mr::TypeStatus::animations);
            CHECK(r.steps[0].types[1].status == mr::TypeStatus::malformed);
        }
        CHECK((g.calls == std::vector<std::string>{"cull RUS_INFANTRY_1_shadow 10000", "reload RUS_INFANTRY_1_shadow"}));
        text = "objectTypes = { pdxmesh = { name = a ";
        CHECK(mr::run(g, in).steps[0].status == mr::Status::unreadable);
        in.read_file = nullptr;
        CHECK(mr::run(g, in).steps[0].status == mr::Status::unreadable);
    });

    run("run: the console steps, a texture's folder as the game spells it, assets recreate the units", [] {
        FakeGame g = vanilla();
        g.answers["reload particle"] = mr::ConsoleAnswer{true, false, false, "no"};
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_texture, "gfx/models/units/RUS_01_diffuse.dds", 0},
                    {mr::Step::What::reload_texture, "gfx/models/Other/x.dds", 0},
                    {mr::Step::What::reload_anim, "", 0},
                    {mr::Step::What::reload_particle, "", 0},
                    {mr::Step::What::reload_assets, "", 0}};
        const mr::RunResult r = mr::run(g, in);
        CHECK((g.calls == std::vector<std::string>{"console reloadtexture gfx/models/Units/RUS_01_diffuse.dds",
                                                    "console reloadtexture gfx/models/Other/x.dds", "console reload anim",
                                                    "console reload particle", "console reload assets",
                                                    "refresh map"}));
        CHECK(r.steps[0].status == mr::Status::ok && r.steps[0].texture == "gfx/models/Units/RUS_01_diffuse.dds" &&
              r.steps[0].answer == "gfx/models/Units/RUS_01_diffuse.dds reloaded");
        CHECK(r.steps[3].status == mr::Status::failed && r.steps[3].answer == "no");
        CHECK(r.map_refreshed && r.unit_instances == 7 && r.provinces == 4940 && !r.load_a_save && !r.faulted);
        CHECK(r.envoys == 12 && r.trade_goods == 30 && r.ambient == 5);
        g.units = mr::MapRefresh{false, false, 0};  // no map: a save load shows them
        const mr::RunResult no_map = mr::run(g, in);
        CHECK(!no_map.map_refreshed && no_map.load_a_save && !no_map.faulted);
        g.units = mr::MapRefresh{true, true, 3, 120};
        const mr::RunResult fault = mr::run(g, in);
        CHECK(fault.faulted && !fault.map_refreshed && fault.load_a_save);
        CHECK(fault.steps[4].status == mr::Status::faulted && fault.steps[0].status == mr::Status::ok);
        g.units = mr::MapRefresh{true, false, 7, 4940, 12, 30, 5};
        g.console_present = false;
        g.calls.clear();
        const mr::RunResult none = mr::run(g, in);
        CHECK(none.steps[2].status == mr::Status::no_console && !none.load_a_save && !none.map_refreshed);
        CHECK(std::find(g.calls.begin(), g.calls.end(), "refresh map") == g.calls.end());
    });

    run("run: an exception in the game's code stops the rest", [] {
        FakeGame g = vanilla();
        g.fault_on_reload = 0x1000;
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_mesh, "gfx/models/Units/RUS_01.mesh", 0}, {mr::Step::What::reload_assets, "", 0}};
        const mr::RunResult r = mr::run(g, in);
        CHECK(r.faulted && r.steps[0].status == mr::Status::faulted && r.steps[1].status == mr::Status::not_run);
        CHECK(g.calls == std::vector<std::string>{"reload RUS_INFANTRY_1"});
        g.table = false;
        g.fault_on_reload = 0;
        CHECK(mr::run(g, in).steps[0].status == mr::Status::no_table);
    });

    run("run: an empty or damaged mesh, animation or texture is not handed to the game", [] {
        CHECK(mr::plausible_file("gfx/models/a.MESH", std::string("@@b@!\x08pdxasset", 14)));
        CHECK(!mr::plausible_file("gfx/models/a.mesh", ""));
        CHECK(!mr::plausible_file("gfx/models/a.anim", "@@b"));
        CHECK(!mr::plausible_file("gfx/models/a.anim", "PK\x03\x04xxxx"));
        CHECK(mr::plausible_file("gfx/models/a.dds", "DDS |...") && !mr::plausible_file("gfx/models/a.dds", "@@b@"));
        CHECK(mr::plausible_file("gfx/models/a.tga", std::string(18, '\0')) && !mr::plausible_file("gfx/models/a.tga", "x"));
        CHECK(mr::plausible_file("gfx/entities/a.asset", ""));  // text: not looked at

        // The plan names the files of the once-only steps.
        const std::vector<mr::Change> c = {
            {mr::FileKey{0, "gfx/models/a.anim"}, mr::Kind::animation},
            {mr::FileKey{1, "gfx/models/b.anim"}, mr::Kind::animation},
            {mr::FileKey{0, "gfx/particles/fire.dds"}, mr::Kind::particle_texture}};
        const std::vector<mr::Step> s = mr::plan(c, {});
        CHECK(s.size() == 2 && s[0].files.size() == 2 && s[0].files[1].mod == 1 && s[1].files.size() == 1);

        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_mesh, "gfx/models/Units/RUS_01.mesh", 0},
                    {mr::Step::What::reload_texture, "gfx/models/Units/RUS_01_diffuse.dds", 0}, s[0],
                    {mr::Step::What::reload_assets, "", 0}};
        const std::map<std::string, std::string> heads = {{"gfx/models/Units/RUS_01.mesh", ""},
                                                          {"gfx/models/Units/RUS_01_diffuse.dds", "DDS |"},
                                                          {"gfx/models/a.anim", "@@b@"}};
        in.read_head = [&](std::size_t, std::string_view rel, std::string& out) {
            const auto it = heads.find(std::string(rel));
            if (it == heads.end()) return false;  // b.anim cannot be read
            out = it->second;
            return true;
        };
        const mr::RunResult r = mr::run(g, in);
        CHECK(r.steps.size() == 4 && r.steps[0].status == mr::Status::bad_file &&
              r.steps[0].bad_file == "gfx/models/Units/RUS_01.mesh");
        CHECK(r.steps[1].status == mr::Status::ok);
        CHECK(r.steps[2].status == mr::Status::bad_file && r.steps[2].bad_file == "gfx/models/b.anim");
        CHECK(r.steps[3].status == mr::Status::ok && !r.faulted && r.map_refreshed);
        CHECK((g.calls == std::vector<std::string>{"console reloadtexture gfx/models/Units/RUS_01_diffuse.dds",
                                                    "console reload assets", "refresh map"}));
        const std::vector<std::string> log = mr::log_lines(r);
        CHECK(log.size() >= 3 && log[0] == "mesh gfx/models/Units/RUS_01.mesh: not reloaded, a file is empty or not in the "
                                           "game's format (gfx/models/Units/RUS_01.mesh)");
        CHECK(log.size() >= 3 && log[2] == "reload anim: not reloaded, a file is empty or not in the game's format "
                                           "(gfx/models/b.anim)");
        mr::PanelState ps;
        ps.looked = true;
        ps.mode = mr::Mode::game;
        ps.has_last = true;
        ps.last = r;
        const mr::Texts t{mr::Lang::en};
        bool shown = false;
        for (const mr::Row& row : mr::rows(ps, t)) {
            shown |= row.value == "gfx/models/b.anim is empty or not in the game's format: not reloaded.";
        }
        CHECK(shown);
    });

    run("run: a mesh or texture deleted before the reload is left out; a mesh cut after its mark goes to the game", [] {
        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_mesh, "gfx/models/Units/RUS_01.mesh", 0},
                    {mr::Step::What::reload_texture, "gfx/models/Units/RUS_01_diffuse.dds", 0}};
        std::map<std::string, std::string> disk;  // nothing: both deleted since the look
        in.read_head = [&](std::size_t, std::string_view rel, std::string& out) {
            const auto it = disk.find(std::string(rel));
            if (it == disk.end()) return false;
            out = it->second;
            return true;
        };
        mr::RunResult r = mr::run(g, in);
        CHECK(r.steps.size() == 2 && r.steps[0].status == mr::Status::bad_file &&
              r.steps[0].bad_file == "gfx/models/Units/RUS_01.mesh");
        CHECK(r.steps.size() == 2 && r.steps[1].status == mr::Status::bad_file &&
              r.steps[1].bad_file == "gfx/models/Units/RUS_01_diffuse.dds");
        CHECK(!r.faulted && !r.load_a_save && g.calls.empty());
        // Saved again, only the mark left of the mesh (an export cut short): the
        // check lets it through, and the game logs that it will not render.
        disk = {{"gfx/models/Units/RUS_01.mesh", "@@b@"}, {"gfx/models/Units/RUS_01_diffuse.dds", "DDS "}};
        r = mr::run(g, in);
        CHECK(r.steps.size() == 2 && r.steps[0].status == mr::Status::ok && r.steps[0].count == 2 &&
              r.steps[1].status == mr::Status::ok);
        CHECK((g.calls == std::vector<std::string>{"reload RUS_INFANTRY_1", "reload RUS_INFANTRY_1_shadow",
                                                    "console reloadtexture gfx/models/Units/RUS_01_diffuse.dds"}));
    });

    run("run: ten reloads in a row give the same calls and results; a .gfx saved back restores the type", [] {
        FakeGame g = vanilla();
        const mr::MeshType original = g.types[1];
        const auto entry = [](const char* scale) {
            return std::string("objectTypes = { pdxmesh = { name = RUS_INFANTRY_2 file = \"gfx/models/Units/RUS_02.mesh\" "
                               "scale = ") +
                   scale +
                   " animation = { id = idle type = rifleman_idle_animation }"
                   " animation = { id = attack type = rifleman_attack_animation } } }\n";
        };
        mr::RunInput gfx;
        gfx.steps = {{mr::Step::What::reload_gfx, "interface/assets/eastern.gfx", 0}};
        gfx.file_exists = [](std::string_view) { return true; };
        std::string text;
        gfx.read_file = [&](const mr::Step&, std::string& out) {
            out = text;
            return true;
        };
        for (int i = 0; i < 10; ++i) {
            const bool bigger = i % 2 == 0;
            text = entry(bigger ? "1.5" : "0.5");
            g.calls.clear();
            const mr::RunResult r = mr::run(g, gfx);
            CHECK(r.steps.size() == 1 && r.steps[0].status == mr::Status::ok && r.steps[0].count == 1 &&
                  r.steps[0].types.empty() && !r.faulted);
            CHECK((g.calls == std::vector<std::string>{std::string("scale RUS_INFANTRY_2 ") + (bigger ? "1.500000" : "0.500000"),
                                                        "reload RUS_INFANTRY_2"}));
        }
        const mr::MeshType& now = g.types[1];
        CHECK(now.scale == original.scale && now.file == original.file && now.cull_square == original.cull_square &&
              now.lookups.size() == original.lookups.size() && now.lookups[0].name == original.lookups[0].name &&
              now.lookups[1].name == original.lookups[1].name);

        // A mesh and the entities, ten times: the same calls and counts each
        // time, and a refresh that faulted once does not hold back the next.
        mr::RunInput mesh;
        mesh.steps = {{mr::Step::What::reload_mesh, "gfx/models/Units/RUS_01.mesh", 0}, {mr::Step::What::reload_assets, "", 0}};
        const std::vector<std::string> want = {"reload RUS_INFANTRY_1", "reload RUS_INFANTRY_1_shadow", "console reload assets",
                                               "refresh map"};
        for (int i = 0; i < 10; ++i) {
            g.units = i == 4 ? mr::MapRefresh{true, true, 3, 120} : mr::MapRefresh{true, false, 7, 4940, 12, 30, 5};
            g.calls.clear();
            const mr::RunResult r = mr::run(g, mesh);
            CHECK(g.calls == want);
            if (i == 4) {
                CHECK(r.faulted && r.load_a_save && r.steps[1].status == mr::Status::faulted);
            } else {
                CHECK(!r.faulted && r.map_refreshed && r.steps[0].count == 2 && r.unit_instances == 7 &&
                      r.provinces == 4940 && r.envoys == 12 && r.trade_goods == 30 && r.ambient == 5);
            }
        }
    });

    run("rows: the menu, changed files and the button, the last run", [] {
        const mr::Texts t{mr::Lang::en};
        mr::PanelState s;
        s.looked = true;
        s.mods = {"Local", "Outside"};
        std::vector<mr::Row> rows = mr::rows(s, t);
        CHECK(find_row(rows, "Mods watched") != nullptr && find_row(rows, "Mods watched")->value == "Local, Outside");
        CHECK(find_row(rows, "No file changed since the last reload.") != nullptr);
        CHECK(find_row(rows, "Load a game to reload.") != nullptr && find_row(rows, "Reload the changed files") == nullptr);
        s.mode = mr::Mode::game;
        s.changed = {mr::Change{mr::FileKey{0, "gfx/models/Units/RUS_01.mesh"}, mr::Kind::mesh}};
        rows = mr::rows(s, t);
        const mr::Row* b = find_row(rows, "Reload the changed files");
        CHECK(b != nullptr && b->kind == mr::kButton && b->id == mr::kReload);
        CHECK(find_row(rows, "gfx/models/Units/RUS_01.mesh") != nullptr &&
              find_row(rows, "gfx/models/Units/RUS_01.mesh")->value == "mesh");
        s.mode = mr::Mode::ironman;
        rows = mr::rows(s, t);
        CHECK(find_row(rows, "Not in ironman or multiplayer games.") != nullptr && find_row(rows, "Reload the changed files") == nullptr);
        s.mode = mr::Mode::game;
        for (int i = 0; i < 50; ++i) s.changed.push_back(s.changed[0]);
        rows = mr::rows(s, t);
        CHECK(find_row(rows, "11 more") != nullptr);
        s.has_last = true;
        s.last_time = "12:04:31";
        FakeGame g = vanilla();
        mr::RunInput in;
        in.steps = {{mr::Step::What::reload_mesh, "gfx/models/Units/RUS_01.mesh", 0}, {mr::Step::What::reload_assets, "", 0},
                    {mr::Step::What::reload_gfx, "interface/assets/eastern.gfx", 0}};
        in.read_file = [](const mr::Step&, std::string& out) {
            out = kEastern;
            return true;
        };
        in.file_exists = [](std::string_view) { return true; };
        s.last = mr::run(g, in);
        rows = mr::rows(s, t);
        CHECK(find_row(rows, "Last reload") != nullptr && find_row(rows, "Last reload")->value == "12:04:31");
        CHECK(find_row(rows, "reload assets") != nullptr && find_row(rows, "reload assets")->value == "ok");
        CHECK(find_row(rows, "    NEW_TYPE") != nullptr && find_row(rows, "    NEW_TYPE")->value == "A new mesh type: it needs a restart.");
        CHECK(find_row(rows, "    pdxmesh") != nullptr && find_row(rows, "    pdxmesh")->value == "Not reloaded: meshsettings");
        CHECK(find_row(rows, "Units, cities, ports and flags on the map are drawn again with the reloaded entities.") != nullptr);
        CHECK(find_row(rows, "Objects already on the map keep their old entity: load a save to see the change.") == nullptr);
        const std::vector<std::string> log = mr::log_lines(s.last);
        CHECK(log.size() == 4 && log[3] == "entities reloaded: map objects of 4940 provinces deleted (7 unit instances), the game creates them "
                                           "again; 12 envoys and 30 trade goods created again, 5 ambient objects "
                                           "removed for the game to create again");
        CHECK(log.size() == 4 && log[0] == "mesh gfx/models/Units/RUS_01.mesh: ok, 2 mesh types reloaded");
        CHECK(log.size() == 4 && log[2] == "pdxmesh interface/assets/eastern.gfx: ok, 1 updated, 1 unchanged; NEW_TYPE not in "
                                           "the game (restart); not reloaded: meshsettings");
        for (const std::string& l : log) std::printf("    %s\n", l.c_str());
        for (const mr::Row& r : rows) CHECK(r.label.size() < 4096 && r.value.size() < 4096);
        const std::vector<mr::Row> pl = mr::rows(s, mr::Texts{mr::Lang::pl});
        CHECK(find_row(pl, "Ostatnie przeładowanie") != nullptr);
        s.last.map_refreshed = false;
        s.last.load_a_save = true;
        rows = mr::rows(s, t);
        CHECK(find_row(rows, "Objects already on the map keep their old entity: load a save to see the change.") != nullptr);
        CHECK(mr::log_lines(s.last).back() == "entities reloaded: objects on the map show them after a save load");
    });

    run("texts: every text the window uses has every language, with its placeholders", [] {
        std::size_t n = 0;
        const mr::TextEntry* e = mr::text_entries(n);
        std::set<std::string> known;
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(known.insert(e[i].english).second);
            for (std::size_t k = 0; k < mr::kTranslations; ++k) {
                CHECK(e[i].translated[k] != nullptr && std::strlen(e[i].translated[k]) > 0);
                if (e[i].translated[k] != nullptr && placeholders(e[i].translated[k]) != placeholders(e[i].english)) {
                    std::printf("  placeholders differ: %s [%zu]\n", e[i].english, k);
                    CHECK(false);
                }
            }
        }
        const fs::path src = fs::path(ANVIL_SOURCE_DIR) / "plugins" / "anvil-model-reload" / "src";
        std::set<std::string> used;
        for (const char* file : {"report.cpp", "plugin.cpp"}) {
            for (const std::string& text : used_texts(src / file)) used.insert(text);
        }
        CHECK(used.size() > 25);
        for (const std::string& text : used) {
            if (!known.count(text)) std::printf("  not in texts.cpp: %s\n", text.c_str());
            CHECK(known.count(text) == 1);
        }
        for (const std::string& text : known) {
            if (!used.count(text)) std::printf("  not used: %s\n", text.c_str());
            CHECK(used.count(text) == 1);
        }
        CHECK(mr::Texts{mr::Lang::de}.f("{0} more", {"3"}) == "3 weitere" && mr::lang_of("zh-Hant") == mr::Lang::zh_hant &&
              mr::lang_of("xx") == mr::Lang::en);
    });

    std::error_code ignored;
    fs::remove_all(temp, ignored);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
