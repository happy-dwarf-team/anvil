// anvil.exe: the command-line tool for plugin authors (design §16). It links
// anvil_core, so it applies the same manifest, symbols.toml and verification
// rules as the loader in version.dll.
//
// Exit codes: 0 OK, 1 problems found, 2 cannot check (also usage errors).
#include "cli/check.hpp"
#include "cli/game.hpp"
#include "cli/gen.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <windows.h>

namespace {

using namespace anvil::cli;

void print_usage(std::FILE* out) {
    std::fputs("Usage: anvil <command> [options]\n"
               "       anvil --version\n"
               "\n"
               "Tools for Anvil plugin authors.\n"
               "\n"
               "Commands:\n"
               "  gen [--lang c|cpp] <symbols.toml> <out-dir>\n"
               "      write symbols.gen.h (C) and symbols.gen.hpp (C++) from symbols.toml;\n"
               "      both without --lang. A file whose content would not change is not rewritten.\n"
               "  check <plugin-dir> [--exe <eu4.exe>] [--format text|json|github]\n"
               "      check plugin.toml and symbols.toml against their schemas. With --exe, also the\n"
               "      game build, every symbol (bytes, memory, pointers, exports, values) and\n"
               "      instruction boundaries, and write symbols.lock; without it, check that\n"
               "      symbols.lock matches symbols.toml.\n"
               "  check --game <EU4 folder> [--format text|json|github]\n"
               "      check every plugin in <folder>/anvil/plugins against <folder>/eu4.exe (nothing is\n"
               "      written), then resolve them as the game does at start-up: the load order, what is\n"
               "      skipped and why (anvil.toml, API, build, conflicts, dependencies), and warnings\n"
               "      for plugins whose code ranges overlap.\n"
               "\n"
               "Options:\n"
               "  -h, --help    show this help\n"
               "  --version     show the Anvil version\n"
               "\n"
               "Exit codes: 0 OK, 1 problems found, 2 cannot check.\n",
               out);
}

int usage_error(const std::string& message) {
    std::fprintf(stderr, "anvil: %s\n\n", message.c_str());
    print_usage(stderr);
    return kExitCannotCheck;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// "--name value" or "--name=value"; advances `i` past what it used.
std::optional<std::wstring> option_value(const std::vector<std::wstring>& args, size_t& i, std::wstring_view name) {
    const std::wstring& arg = args[i];
    if (arg == name) {
        if (i + 1 >= args.size()) return std::nullopt;
        return args[++i];
    }
    if (arg.size() > name.size() && arg.starts_with(name) && arg[name.size()] == L'=') return arg.substr(name.size() + 1);
    return std::nullopt;
}

bool is_option(const std::wstring& arg, std::wstring_view name) {
    return arg == name || (arg.starts_with(name) && arg.size() > name.size() && arg[name.size()] == L'=');
}

// Writes `content` unless the file already holds it (keeps build systems from
// rebuilding plugins whose symbols did not change).
bool write_if_changed(const std::filesystem::path& path, const std::string& content, bool& written) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream existing;
        existing << in.rdbuf();
        if (existing.str() == content) {
            written = false;
            return true;
        }
    }
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    written = true;
    return static_cast<bool>(out);
}

int run_gen(const std::vector<std::wstring>& args) {
    bool c = true, cpp = true;
    std::vector<std::wstring> positional;
    for (size_t i = 2; i < args.size(); ++i) {
        if (is_option(args[i], L"--lang")) {
            const auto lang = option_value(args, i, L"--lang");
            if (lang == L"c") cpp = false;
            else if (lang == L"cpp") c = false;
            else return usage_error("gen: --lang must be c or cpp");
        } else if (args[i].starts_with(L"--")) {
            return usage_error("gen: unknown option " + utf8(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) return usage_error("gen needs <symbols.toml> and <out-dir>");
    const std::filesystem::path source(positional[0]), out_dir(positional[1]);

    const anvil::SymbolFileResult symbols = anvil::load_symbols(source);
    if (!symbols.file) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) {
            std::fprintf(stderr, "anvil gen: cannot read %s\n", shown(source).c_str());
            return kExitCannotCheck;
        }
        std::string message = symbols.error;
        if (message.starts_with("symbols.toml: ")) message.erase(0, 14);
        std::printf("%s%s: error: %s\n", shown(source).c_str(),
                    symbols.line != 0 ? (":" + std::to_string(symbols.line)).c_str() : "", message.c_str());
        return kExitProblems;
    }
    const auto name = source.filename().u8string();
    const GeneratedHeaders headers = generate_headers(*symbols.file, std::string(name.begin(), name.end()), c, cpp);
    if (!headers.error.empty()) {
        std::string message = headers.error;
        if (message.starts_with("symbols.toml: ")) message.erase(0, 14);
        std::printf("%s%s: error: %s\n", shown(source).c_str(),
                    headers.line != 0 ? (":" + std::to_string(headers.line)).c_str() : "", message.c_str());
        return kExitProblems;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    for (const auto& [file, content, wanted] : {std::tuple{"symbols.gen.h", &headers.c, c},
                                                std::tuple{"symbols.gen.hpp", &headers.cpp, cpp}}) {
        if (!wanted) continue;
        const std::filesystem::path path = out_dir / file;
        bool written = false;
        if (!write_if_changed(path, *content, written)) {
            std::fprintf(stderr, "anvil gen: cannot write %s\n", shown(path).c_str());
            return kExitCannotCheck;
        }
        std::printf("%s: %s\n", shown(path).c_str(), written ? "written" : "unchanged");
    }
    return kExitOk;
}

int run_check(const std::vector<std::wstring>& args) {
    CheckOptions options;
    Format format = Format::Text;
    std::optional<std::wstring> game;
    std::vector<std::wstring> positional;
    for (size_t i = 2; i < args.size(); ++i) {
        if (is_option(args[i], L"--game")) {
            game = option_value(args, i, L"--game");
            if (!game || game->empty()) return usage_error("check: --game needs the EU4 folder");
        } else if (is_option(args[i], L"--exe")) {
            const auto exe = option_value(args, i, L"--exe");
            if (!exe || exe->empty()) return usage_error("check: --exe needs the path of eu4.exe");
            options.exe = *exe;
        } else if (is_option(args[i], L"--format")) {
            const auto text = option_value(args, i, L"--format");
            const auto parsed = text ? parse_format(utf8(*text)) : std::nullopt;
            if (!parsed) return usage_error("check: --format must be text, json or github");
            format = *parsed;
        } else if (args[i].starts_with(L"--")) {
            return usage_error("check: unknown option " + utf8(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (game) {
        if (!positional.empty() || !options.exe.empty()) {
            return usage_error("check: --game takes the EU4 folder only (not a <plugin-dir> or --exe)");
        }
        GameOptions game_options;
        game_options.game_dir = *game;
        const GameReport report = check_game(game_options);
        const std::string text = format_game_report(report, format);
        std::fputs(text.c_str(), format == Format::Text && report.exit_code == kExitCannotCheck && report.plugins.empty()
                                     ? stderr
                                     : stdout);
        return report.exit_code;
    }
    if (positional.size() != 1) return usage_error("check needs one <plugin-dir>");
    options.plugin_dir = positional[0];

    const CheckReport report = check_plugin(options);
    const std::string text = format_report(report, format);
    // Text: "cannot check" goes to stderr like usage errors; json and github stay on stdout for tools.
    std::fputs(text.c_str(), format == Format::Text && report.exit_code == kExitCannotCheck ? stderr : stdout);
    return report.exit_code;
}

// UTF-8 output, so paths and messages survive the console and pipes. The code
// page belongs to the console, which outlives this process: restore it.
class Utf8Console {
public:
    Utf8Console() : previous_(GetConsoleOutputCP()) {
        if (previous_ != 0) SetConsoleOutputCP(CP_UTF8);
    }
    ~Utf8Console() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (previous_ != 0) SetConsoleOutputCP(previous_);
    }
    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;

private:
    UINT previous_;
};

int run(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        print_usage(stderr);
        return kExitCannotCheck;
    }
    const std::wstring& command = args[1];
    if (command == L"-h" || command == L"--help") {
        print_usage(stdout);
        return kExitOk;
    }
    if (command == L"--version") {
        std::printf("anvil %s\n", ANVIL_VERSION);
        return kExitOk;
    }
    if (command == L"gen") return run_gen(args);
    if (command == L"check") return run_check(args);
    std::fprintf(stderr, "anvil: unknown command '%s'\n\n", utf8(command).c_str());
    print_usage(stderr);
    return kExitCannotCheck;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const Utf8Console console;
    return run(std::vector<std::wstring>(argv, argv + argc));
}
