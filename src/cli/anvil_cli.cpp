// anvil.exe: the command-line tool for plugin authors (design §16). It links
// anvil_core, so it applies the same manifest, resolver and build rules as the
// loader in version.dll. `gen` and `check` are not implemented yet.
//
// Exit codes: 0 OK, 1 problems found, 2 cannot check (also usage errors).
#include <cstdio>
#include <cstring>

namespace {

constexpr int kOk = 0;
constexpr int kCannotCheck = 2;

void print_usage(std::FILE* out) {
    std::fputs("Usage: anvil <command> [options]\n"
               "       anvil --version\n"
               "\n"
               "Tools for Anvil plugin authors.\n"
               "\n"
               "Commands:\n"
               "  gen      generate the symbol header from symbols.toml (not implemented yet)\n"
               "  check    check a plugin against its schemas and the game (not implemented yet)\n"
               "\n"
               "Options:\n"
               "  -h, --help    show this help\n"
               "  --version     show the Anvil version\n",
               out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return kCannotCheck;
    }
    const char* command = argv[1];
    if (std::strcmp(command, "-h") == 0 || std::strcmp(command, "--help") == 0) {
        print_usage(stdout);
        return kOk;
    }
    if (std::strcmp(command, "--version") == 0) {
        std::printf("anvil %s\n", ANVIL_VERSION);
        return kOk;
    }
    if (std::strcmp(command, "gen") == 0 || std::strcmp(command, "check") == 0) {
        std::fprintf(stderr, "anvil %s: not implemented yet\n", command);
        return kCannotCheck;
    }
    std::fprintf(stderr, "anvil: unknown command '%s'\n\n", command);
    print_usage(stderr);
    return kCannotCheck;
}
