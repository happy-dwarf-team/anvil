#pragma once

#include "zip.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

// The Anvil package the Manager carries (issue #59): version.dll and anvil/
// as `cmake --install --component anvil` lays them out, of the Manager's own
// version. Every file belongs to Anvil except anvil/anvil.toml, which is the
// player's once it exists.
struct Package {
    ZipArchive zip;
    std::string version;
    struct File {
        std::string path;    // as in the zip: "version.dll", "anvil/plugins/anvil-game/plugin.toml"
        std::string sha256;  // upper-case hex
    };
    std::vector<File> files;
    std::vector<std::string> standard_plugins;  // the folders of anvil/plugins/ in the package
};

inline constexpr const char* kPlayerToml = "anvil/anvil.toml";
inline constexpr const char* kManagerExe = "AnvilManager.exe";

// Checks that `zip` is a package (version.dll, anvil/anvil.toml, at least one
// standard plugin) and hashes its files.
std::optional<Package> load_package(ZipArchive zip, std::string version, std::string& error);

// ProductName and ProductVersion of a DLL's version resource; nullopt when
// it has none or cannot be read. Read as data: nothing in it runs.
struct DllProduct {
    std::string name;
    std::string version;
};
std::optional<DllProduct> read_dll_product(const std::filesystem::path& dll);

// What a game folder holds, compared with the package.
struct Installation {
    enum class Dll { Missing, Anvil, Foreign };
    Dll dll = Dll::Missing;
    std::string dll_version;      // Anvil's version.dll: its ProductVersion
    bool anvil_folder = false;    // <EU4>/anvil exists
    bool manager_copy = false;    // <EU4>/AnvilManager.exe exists
    std::vector<std::string> missing;    // package files that are not there (anvil.toml included)
    std::vector<std::string> differing;  // package files with other bytes (not anvil.toml)
    std::vector<std::string> extra;      // files in a standard plugin's folder the package does not have

    bool installed() const { return dll == Dll::Anvil; }
    // Anvil of the package's version with every file as the package has it.
    bool intact() const;
};

Installation inspect_installation(const Package& package, const std::filesystem::path& game_dir);

struct Step {
    enum class Kind {
        Write,          // a package file
        CopyManager,    // the running AnvilManager.exe to <EU4>/AnvilManager.exe
        Remove,         // a file
        RemoveTree,     // a folder with everything in it
        RemoveIfEmpty,  // a folder, when nothing is left in it
        RemoveManager,  // <EU4>/AnvilManager.exe, possibly the running exe
    };
    Kind kind = Kind::Write;
    std::string path;  // relative to the game folder, '/'-separated
};

struct Plan {
    std::vector<Step> steps;
    std::vector<std::string> notes;  // what the player should know before it runs
    std::string refused;             // why the plan cannot run; steps are then empty
};

// Install, update and repair are one plan: every package file that is
// missing or differs is written (anvil.toml only when missing: the player's
// settings stay), files in a standard plugin's folder that the package does
// not have are removed, and the Manager copies itself into the folder when
// `manager_exe` is given and not already that file. version.dll comes last,
// so the game stays vanilla until Anvil is complete. A version.dll that is
// not Anvil's (another mod's proxy) is refused unless `replace_foreign_dll`.
Plan plan_install(const Package& package, const Installation& installation, const std::filesystem::path& game_dir,
                  const std::filesystem::path& manager_exe, bool replace_foreign_dll);

// Uninstall (issue #59): version.dll first (only Anvil's), so the game is
// vanilla at once, then the standard plugins, the package's other files
// except anvil.toml and AnvilManager.exe; the player's plugins, anvil/data,
// anvil/crashes, the logs and anvil.toml stay. With `everything` the whole
// anvil/ folder goes too.
Plan plan_uninstall(const Package& package, const Installation& installation, bool everything);

struct ApplyResult {
    bool ok = true;
    std::string error;           // the step that failed and why; the steps before it are done
    bool access_denied = false;  // Windows refused access: elevation may help
    // RemoveManager found the running exe: the caller removes it after exiting.
    bool manager_left = false;
    size_t done = 0;
};

// Runs the steps in order and stops at the first that fails. Files are
// written through a temporary file each; a folder is removed only below the
// game folder.
ApplyResult apply_plan(const Package& package, const Plan& plan, const std::filesystem::path& game_dir,
                       const std::filesystem::path& manager_exe);

}  // namespace anvil::manager
