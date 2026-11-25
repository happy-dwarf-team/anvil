// Unit tests for `anvil port` (design §16): two synthetic builds of the same
// code, moved, with an operand changed, with code gone and with code copied.
// The check is that what cannot be placed for certain is not placed, and that
// what is written reads back and verifies on both builds.
#include "cli/check.hpp"
#include "cli/port.hpp"
#include "core/game_build.hpp"
#include "core/symbols.hpp"
#include "test_image.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;
using namespace anvil::cli;
namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Where the pieces of the test code are in a build, 0 when it lacks one.
struct Layout {
    uint32_t far_fn = 0;
    uint32_t speed = 0;
    uint32_t caller = 0;
    uint32_t twin = 0;
    uint32_t second_twin = 0;
    uint32_t gone = 0;
    uint32_t data = 0;
    uint32_t table = 0;
    uint32_t speed_offset = 0x418;
    uint32_t big = 0;           // code with a struct offset past 64 KB
    uint32_t big_offset = 0;
    uint32_t switcher = 0;      // a jump table through __ImageBase
    uint32_t switch_table = 0;
};

const std::initializer_list<uint8_t> kFar = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x20, 0x8B,
                                             0xD9, 0x89, 0x5F, 0x10, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
const std::initializer_list<uint8_t> kTwin = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xF9, 0x89, 0x7B,
                                              0x20, 0x31, 0xC0, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0x90, 0xC3};
const std::initializer_list<uint8_t> kGone = {0x41, 0x54, 0x48, 0x83, 0xEC, 0x40, 0x44, 0x8B, 0xE1, 0x44,
                                              0x89, 0x63, 0x30, 0x48, 0x83, 0xC4, 0x40, 0x41, 0x5C, 0xC3};

std::vector<uint8_t> build_exe(const Layout& l) {
    TestImage image;
    std::fill(image.mapped.begin() + 0x1000, image.mapped.begin() + 0x1100, uint8_t{0xCC});
    image.put(l.far_fn, kFar);
    // mov rax, [rdx+speed_offset]; test rax, rax; je +3; mov rax, [rax]; ret
    image.put(l.speed, {0x48, 0x8B, 0x82, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x03, 0x48, 0x8B, 0x00, 0xC3});
    image.put_value<uint32_t>(l.speed + 3, l.speed_offset);
    // nop; push rbx; sub rsp, 0x28; call speed; lea rcx, [rip+data]; add rsp, 0x28; pop rbx; ret
    image.put(l.caller, {0x90, 0x53, 0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0x5B, 0xC3});
    image.put_value<int32_t>(l.caller + 7, static_cast<int32_t>(l.speed - (l.caller + 11)));
    image.put_value<int32_t>(l.caller + 14, static_cast<int32_t>(l.data - (l.caller + 18)));
    // mov rax, [rdi+big_offset]; test rax, rax; je +3; mov rax, [rax]; ret
    image.put(l.big, {0x48, 0x8B, 0x87, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0x03, 0x48, 0x8B, 0x00, 0xC3});
    image.put_value<uint32_t>(l.big + 3, l.big_offset);
    // lea r8, [__ImageBase]; mov ecx, [r8+rax*4+switch_table]; ret
    image.put(l.switcher, {0x4C, 0x8D, 0x05, 0, 0, 0, 0, 0x41, 0x8B, 0x8C, 0x80, 0, 0, 0, 0, 0xC3});
    image.put_value<int32_t>(l.switcher + 3, -static_cast<int32_t>(l.switcher + 7));
    image.put_value<uint32_t>(l.switcher + 11, l.switch_table);
    if (l.twin != 0) image.put(l.twin, kTwin);
    if (l.second_twin != 0) image.put(l.second_twin, kTwin);
    if (l.gone != 0) image.put(l.gone, kGone);
    image.put_value<uint64_t>(l.table, TestImage::kPreferredBase + l.far_fn);
    image.put_value<uint64_t>(l.table + 8, TestImage::kPreferredBase + l.caller);
    image.put_value<uint64_t>(l.table + 16, TestImage::kPreferredBase + l.speed);
    return image.file();
}

constexpr Layout kOld = {0x1000, 0x1020, 0x1040, 0x1060, 0, 0x1080, 0x3008, 0x2020, 0x418, 0x10A0, 0x32610, 0x10C0, 0x2060};
constexpr Layout kNew = {0x1010, 0x1030, 0x1050, 0x1070, 0x1090, 0, 0x3010, 0x2040, 0x408, 0x10B0, 0x32618, 0x10D0, 0x2080};

std::string hex_bytes(std::initializer_list<uint8_t> bytes) {
    std::string out;
    char pair[4];
    for (const uint8_t b : bytes) {
        std::snprintf(pair, sizeof(pair), "%02X ", b);
        out += pair;
    }
    out.pop_back();
    return out;
}

std::string digest_of(std::initializer_list<uint8_t> bytes) { return sha256_bytes(bytes.begin(), bytes.size()); }

// Every kind of symbol that can be placed, with a comment that must survive.
std::string placeable() {
    return "# the header comment\n"
           "build = \"old\"\n"
           "\n"
           "[symbol.far_fn]\n"
           "rva = 0x1000  # stays\n"
           "sha256 = \"" + digest_of(kFar) + "\"\n"
           "size = 20\n"
           "memory = \"code\"\n"
           "signature = \"void (int)\"\n"
           "\n"
           "[symbol.speed]\n"
           "rva = 0x1020\n"
           "bytes = \"48 8B 82 18 04 00 00 48 85 C0\"\n"
           "memory = \"code\"\n"
           "values.offset = { at = 3, size = 4 }\n"
           "\n"
           "[symbol.caller]\n"
           "rva = 0x1040\n"
           "bytes = \"90 53 48 83 EC 28 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ??\"\n"
           "memory = \"code\"\n"
           "\n"
           "[symbol.caller_tail]\n"
           "rva = 0x1052\n"
           "bytes = \"48 83 C4 28 5B C3\"\n"
           "memory = \"code\"\n"
           "\n"
           "[symbol.global]\n"
           "rva = 0x3008\n"
           "memory = \"write\"\n"
           "type = \"uint64_t\"\n"
           "\n"
           "[symbol.caller_slot]\n"
           "rva = 0x2028\n"
           "memory = \"read\"\n"
           "points_to = 0x1040\n";
}

struct PortFixture {
    fs::path root = fs::temp_directory_path() / ("anvil_port_tests_" + std::to_string(GetCurrentProcessId()));
    fs::path old_exe = root / "old" / "eu4.exe";
    fs::path new_exe = root / "new" / "eu4.exe";
    fs::path symbols = root / "symbols.toml";
    fs::path out = root / "out.toml";

    PortFixture() {
        fs::remove_all(root);
        const auto old_bytes = build_exe(kOld);
        const auto new_bytes = build_exe(kNew);
        write(old_exe, std::string(old_bytes.begin(), old_bytes.end()));
        write(new_exe, std::string(new_bytes.begin(), new_bytes.end()));
    }
    ~PortFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    PortReport port(const std::string& text, bool write_out = true) const {
        write(symbols, text);
        fs::remove(out);
        PortOptions options;
        options.symbols = symbols;
        options.from = old_exe;
        options.to = new_exe;
        options.build = "new";
        options.from_build = "old";
        if (write_out) options.out = out;
        return port_symbols(options);
    }
    // The file's symbols on `build`, as check and the loader would see them.
    bool verifies(const std::string& text, const char* build, const fs::path& exe) const {
        const auto parsed = parse_symbols_text(text);
        if (!parsed.file) {
            std::printf("  parse: %s\n", parsed.error.c_str());
            return false;
        }
        SymbolFile file = *parsed.file;
        std::string error;
        const auto reader = FileImageReader::load(exe, error);
        const auto image = PeImage::parse(*reader, error);
        if (!select_build(file, build)) return false;
        const auto verification = verify_symbols(file, *image);
        for (const auto& problem : verification.problems) std::printf("  verify: %s\n", problem.c_str());
        return verification.ok();
    }
};

const PortedSymbol* ported(const PortReport& report, const std::string& name) {
    const auto found = std::find_if(report.ported.begin(), report.ported.end(), [&](const PortedSymbol& p) { return p.name == name; });
    return found == report.ported.end() ? nullptr : &*found;
}

const ManualSymbol* manual(const PortReport& report, const std::string& name) {
    const auto found = std::find_if(report.manual.begin(), report.manual.end(), [&](const ManualSymbol& m) { return m.name == name; });
    return found == report.manual.end() ? nullptr : &*found;
}

}  // namespace

void port_tests() {
    const PortFixture fx;

    run("port places moved code and writes a file that verifies on both builds", [&] {
        const PortReport report = fx.port(placeable());
        CHECK(report.exit_code == kExitOk);
        CHECK(report.cannot_port.empty());
        CHECK(report.manual.empty());
        for (const auto& m : report.manual) std::printf("  manual %s: %s\n", m.name.c_str(), m.reason.c_str());
        CHECK(report.ported.size() == 6);
        for (const auto& [name, rva] : {std::pair{"far_fn", kNew.far_fn}, std::pair{"speed", kNew.speed}, std::pair{"caller", kNew.caller},
                                        std::pair{"caller_tail", kNew.caller + 0x12}, std::pair{"global", kNew.data},
                                        std::pair{"caller_slot", kNew.table + 8}}) {
            const PortedSymbol* symbol = ported(report, name);
            CHECK(symbol != nullptr && symbol->rva == rva);
        }
        if (report.ported.size() != 6) return;
        CHECK(ported(report, "caller_slot")->points_to == kNew.caller);
        CHECK(!ported(report, "far_fn")->bytes_changed);
        CHECK(ported(report, "speed")->bytes_changed);   // the operand a value declares
        CHECK(!ported(report, "caller")->bytes_changed);  // the call target is not compared
        const std::string text = read(fx.out);
        CHECK(fx.verifies(text, "old", fx.old_exe));
        CHECK(fx.verifies(text, "new", fx.new_exe));
        CHECK(contains(text, "builds = [\"old\", \"new\"]"));
        CHECK(contains(text, "# the header comment"));
        CHECK(contains(text, "# stays"));  // a key that moved leaves its comment
        CHECK(contains(text, "build.\"new\" = { rva = 0x1010 }"));
        CHECK(contains(text, "bytes = \"48 8B 82 08 04 00 00 48 85 C0\""));
    });

    run("port keeps digests as digests and adds to a file with builds", [&] {
        const auto first = fx.port(placeable());
        CHECK(first.exit_code == kExitOk);
        std::string text = read(fx.out);
        // The same file as the input of a third build, and the new one as the old one.
        PortOptions options;
        options.symbols = fx.out;
        options.from = fx.new_exe;
        options.to = fx.old_exe;
        options.build = "third";
        options.from_build = "new";
        options.out = fx.root / "third.toml";
        const PortReport report = port_symbols(options);
        CHECK(report.exit_code == kExitOk);
        const std::string third = read(options.out);
        CHECK(contains(third, "builds = [\"old\", \"new\", \"third\"]"));
        CHECK(fx.verifies(third, "old", fx.old_exe));
        CHECK(fx.verifies(third, "new", fx.new_exe));
        CHECK(fx.verifies(third, "third", fx.old_exe));
        // Symbols written as digests have no game bytes in the output.
        const std::string digest_file = "build = \"old\"\n[symbol.speed]\nrva = 0x1020\nsha256 = \"" +
                                        digest_of({0x48, 0x8B, 0x82, 0x18, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC0}) +
                                        "\"\nsize = 10\nmemory = \"code\"\nvalues.offset = { at = 3, size = 4 }\n";
        CHECK(fx.port(digest_file).exit_code == kExitOk);
        const std::string written = read(fx.out);
        CHECK(fx.verifies(written, "new", fx.new_exe));
        CHECK(!contains(written, "bytes"));
        CHECK(contains(written, digest_of({0x48, 0x8B, 0x82, 0x08, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC0})));
    });

    run("port reads digest-only builds from the old eu4.exe and writes the new build as digests", [&] {
        // No `bytes` anywhere: port reads them from --from at each rva, checks them against the digest,
        // and the new build's tables state sha256, size and any.
        const std::string far_digest = digest_of(kFar);
        const std::string speed_digest = digest_of({0x48, 0x8B, 0x82, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0});
        const std::string text = "builds = [\"old\", \"other\"]\n\n"
                                 "[symbol.far_fn]\nmemory = \"code\"\nsignature = \"void (int)\"\n"
                                 "build.\"old\" = { rva = 0x1000, sha256 = \"" + far_digest + "\", size = 20 }\n"
                                 "build.\"other\" = { rva = 0x1004, sha256 = \"" + far_digest + "\", size = 20 }\n\n"
                                 "[symbol.speed]\nmemory = \"code\"\n"
                                 "build.\"old\" = { rva = 0x1020, sha256 = \"" + speed_digest + "\", size = 10, any = [3, 4, 5, 6] }\n"
                                 "build.\"other\" = { rva = 0x1024, sha256 = \"" + speed_digest + "\", size = 10, any = [3, 4, 5, 6] }\n";
        const PortReport report = fx.port(text);
        if (!report.cannot_port.empty()) std::printf("  cannot port: %s\n", report.cannot_port.c_str());
        for (const auto& m : report.manual) std::printf("  manual %s: %s\n", m.name.c_str(), m.reason.c_str());
        CHECK(report.exit_code == kExitOk && report.ported.size() == 2);
        CHECK(ported(report, "far_fn") != nullptr && ported(report, "far_fn")->rva == kNew.far_fn);
        CHECK(ported(report, "speed") != nullptr && ported(report, "speed")->rva == kNew.speed);
        const std::string out = read(fx.out);
        CHECK(!contains(out, "bytes"));
        CHECK(contains(out, "build.\"new\" = { rva = 0x1010, sha256 = \"" + far_digest + "\", size = 20 }"));
        CHECK(contains(out, "build.\"new\" = { rva = 0x1030, sha256 = \"" + speed_digest + "\", size = 10, any = [3, 4, 5, 6] }"));
        CHECK(fx.verifies(out, "old", fx.old_exe));
        CHECK(fx.verifies(out, "new", fx.new_exe));
        // A digest the old eu4.exe does not match leaves the symbol by hand, as wrong bytes would.
        std::string wrong = text;
        wrong.replace(wrong.find(far_digest), far_digest.size(), digest_of(kTwin));
        const PortReport mismatch = fx.port(wrong);
        const ManualSymbol* far_fn = manual(mismatch, "far_fn");
        CHECK(mismatch.exit_code == kExitProblems && far_fn != nullptr && contains(far_fn->reason, "does not verify on old"));
    });

    run("port drops the is of a digest whose value moved, as a bytes file would allow it", [&] {
        const std::string digest = digest_of({0x48, 0x8B, 0x82, 0x18, 0x04, 0x00, 0x00, 0x48, 0x85, 0xC0});
        const std::string head = "build = \"old\"\n[symbol.speed]\nrva = 0x1020\nsha256 = \"" + digest + "\"\nsize = 10\nmemory = \"code\"\n";
        const auto port = [&](const std::string& values) {
            const PortReport report = fx.port(head + values);
            CHECK(report.exit_code == kExitOk);
            const PortedSymbol* speed = ported(report, "speed");
            CHECK(speed != nullptr && speed->values_changed == std::vector<std::string>{"offset"});
            const std::string out = read(fx.out);
            CHECK(fx.verifies(out, "old", fx.old_exe));
            CHECK(fx.verifies(out, "new", fx.new_exe));
            CHECK(!contains(out, "0x418"));
            return std::pair{report, out};
        };
        const auto [report, last] = port("values.offset = { at = 3, size = 4, is = 0x418 }  # kept\nvalues.prefix = { at = 0, size = 1, is = 0x48 }\n");
        CHECK(contains(last, "values.offset = { at = 3, size = 4 }  # kept\n"));
        CHECK(contains(last, "values.prefix = { at = 0, size = 1, is = 0x48 }\n"));
        CHECK(contains(format_port_report(report, fx.symbols, Format::Json), "\"values_changed\": [\"offset\"]"));
        CHECK(contains(format_port_report(report, fx.symbols, Format::Text), "1 value(s) differ in the new build"));
        CHECK(contains(port("values.offset = { is = 0x418, at = 3, size = 4 }\n").second, "values.offset = { at = 3, size = 4 }\n"));
        CHECK(contains(port("[symbol.speed.values.offset]\nat = 3\nsize = 4\nis = 0x418\n").second, "[symbol.speed.values.offset]\nat = 3\nsize = 4\n"));
    });

    run("port keeps per-build tables and their style", [&] {
        const std::string text = "builds = [\"old\", \"other\"]\n\n[symbol.far_fn]\nbytes = \"" + hex_bytes(kFar) +
                                 "\"\nmemory = \"code\"\nbuild.\"old\" = { rva = 0x1000 }\nbuild.\"other\" = { rva = 0x1004 }\n\n"
                                 "[symbol.speed]\nmemory = \"code\"\nvalues.offset = { at = 3, size = 4 }\n\n[symbol.speed.build.\"old\"]\nrva = 0x1020\n"
                                 "bytes = \"48 8B 82 18 04 00 00\"\n\n[symbol.speed.build.\"other\"]\nrva = 0x1024\n"
                                 "bytes = \"48 8B 82 18 04 00 00\"\n";
        const PortReport report = fx.port(text);
        CHECK(report.exit_code == kExitOk);
        const std::string out = read(fx.out);
        CHECK(contains(out, "build.\"new\" = { rva = 0x1010 }\n"));
        CHECK(contains(out, "[symbol.speed.build.\"new\"]\nrva = 0x1030\n"));
        const auto parsed = parse_symbols_text(out);
        CHECK(parsed.file.has_value());
    });

    run("port leaves by hand what has no certain place, and says why", [&] {
        const std::string extra =
            "\n[symbol.twin]\nrva = 0x1060\nbytes = \"" + hex_bytes(kTwin) + "\"\nmemory = \"code\"\n" +
            "\n[symbol.gone]\nrva = 0x1080\nbytes = \"" + hex_bytes(kGone) + "\"\nmemory = \"code\"\n"
            "\n[symbol.block]\nrva = 0x1020\nbytes = \"48 8B 82 18 04 00 00\"\nmemory = \"code\"\n"
            "\n[symbol.fixed]\nrva = 0x1020\nbytes = \"48 8B 82 18 04 00 00\"\nmemory = \"code\"\nvalues.offset = { at = 3, size = 4, is = 0x418 }\n";
        const PortReport report = fx.port(placeable() + extra);
        CHECK(report.exit_code == kExitProblems);
        CHECK(report.ported.size() == 6);
        CHECK(report.manual.size() == 4);
        const ManualSymbol* twin = manual(report, "twin");
        CHECK(twin != nullptr && contains(twin->reason, "more than once") && !twin->candidate);
        const ManualSymbol* gone = manual(report, "gone");
        CHECK(gone != nullptr && contains(gone->reason, "not in the new build"));
        const ManualSymbol* block = manual(report, "block");
        CHECK(block != nullptr && contains(block->reason, "operand differs") && block->candidate == kNew.speed);
        const ManualSymbol* fixed = manual(report, "fixed");
        CHECK(fixed != nullptr && contains(fixed->reason, "does not verify on the new build") && fixed->candidate == kNew.speed);
        // The file names them, and does not parse until they are placed.
        const std::string text = read(fx.out);
        CHECK(contains(text, "# anvil port: not placed in new: "));
        CHECK(!parse_symbols_text(text).file.has_value());
        const std::string shown = format_port_report(report, fx.symbols, Format::Text);
        CHECK(contains(shown, ": error: gone: "));
        CHECK(contains(shown, "6 of 10 symbol(s) placed"));
        CHECK(contains(shown, "4 left by hand"));
        const std::string json = format_port_report(report, fx.symbols, Format::Json);
        CHECK(contains(json, "\"result\": \"manual\""));
        CHECK(contains(json, "\"caller\": \"0x1050\""));
        CHECK(!contains(json, "\"gone\": \"0x"));  // a generator's map holds only what was placed
        CHECK(!contains(json, "48 8B"));           // no game bytes in a report
    });

    run("port treats a large displacement as an operand unless it goes through __ImageBase", [&] {
        const std::string text = "build = \"old\"\n[symbol.big]\nrva = 0x10A0\nbytes = \"48 8B 87 10 26 03 00\"\nmemory = \"code\"\n"
                                 "[symbol.switcher]\nrva = 0x10C0\nbytes = \"4C 8D 05 ?? ?? ?? ?? 41 8B 8C 80 ?? ?? ?? ??\"\nmemory = \"code\"\n"
                                 "signature = \"void ()\"\n";
        const PortReport report = fx.port(text);
        const ManualSymbol* big = manual(report, "big");
        CHECK(big != nullptr);                  // 0x32610 became 0x32618: not the same code
        CHECK(ported(report, "big") == nullptr);
        const PortedSymbol* table = ported(report, "switcher");
        CHECK(table != nullptr && table->rva == kNew.switcher && table->review.empty());
    });

    run("port keeps the file valid at its edges", [&] {
        // No newline at the end of the file: what follows the last line starts a line of its own.
        std::string text = placeable();
        text.pop_back();
        CHECK(fx.port(text).exit_code == kExitOk);
        CHECK(fx.verifies(read(fx.out), "new", fx.new_exe));
        // The same with a symbol left by hand: the file stays TOML.
        const PortReport left = fx.port("build = \"old\"\n[symbol.gone]\nrva = 0x1080\nbytes = \"" + hex_bytes(kGone) + "\"\nmemory = \"code\"");
        CHECK(left.exit_code == kExitProblems && left.written);
        CHECK(contains(parse_symbols_text(read(fx.out)).error, "gone"));
        // builds over several lines, with and without a last comma.
        for (const char* list : {"builds = [\n  \"old\",\n  \"other\",\n]\n", "builds = [\n  \"old\",\n  \"other\"  # a note\n]\n",
                                 "builds = [\"old\", \"other\",]\n"}) {
            const std::string many = std::string(list) + "[symbol.far_fn]\nbytes = \"" + hex_bytes(kFar) +
                                     "\"\nmemory = \"code\"\nbuild.\"old\" = { rva = 0x1000 }\nbuild.\"other\" = { rva = 0x1000 }\n";
            CHECK(fx.port(many).exit_code == kExitOk);
            const std::string out = read(fx.out);
            CHECK(fx.verifies(out, "new", fx.new_exe));
            const auto parsed = parse_symbols_text(out);
            CHECK(parsed.file && parsed.file->builds.size() == 3);
        }
    });

    run("port checks what it is given", [&] {
        CHECK(fx.port("build = \"old\"\n[symbol.a]\nrva = 0x1000\nmemroy = \"code\"\n").exit_code == kExitCannotCheck);
        CHECK(contains(fx.port("build = \"new\"\n").cannot_port, "already declares new"));
        CHECK(contains(fx.port("build = \"third\"\n").cannot_port, "does not declare old"));
        // Only [symbol.<name>] tables: an inline table or dotted keys would lose lines or land under [symbol].
        for (const std::string& written :
             {"far_fn = { rva = 0x1000, bytes = \"" + hex_bytes(kFar) + "\", memory = \"code\" }\n",
              "far_fn.rva = 0x1000\nfar_fn.bytes = \"" + hex_bytes(kFar) + "\"\nfar_fn.memory = \"code\"\n"}) {
            const PortReport refused = fx.port("build = \"old\"\n[symbol]\n" + written);
            CHECK(contains(refused.cannot_port, "not written as a [symbol.far_fn] table"));
            CHECK(!refused.written && !fs::exists(fx.out));
        }
        // Bytes that do not hold on the old build are not carried over.
        const PortReport wrong = fx.port("build = \"old\"\n[symbol.a]\nrva = 0x1000\nbytes = \"90 90 90\"\nmemory = \"code\"\n");
        CHECK(wrong.exit_code == kExitProblems);
        CHECK(manual(wrong, "a") != nullptr && contains(manual(wrong, "a")->reason, "does not verify on old"));
        const PortReport absent = fx.port("builds = [\"old\", \"other\"]\n[symbol.a]\nrva = 0x1000\nbytes = \"48 89 5C\"\n"
                                          "memory = \"code\"\nabsent = [\"old\"]\n");
        CHECK(manual(absent, "a") != nullptr && contains(manual(absent, "a")->reason, "absent on old"));
        PortOptions options;
        options.symbols = fx.symbols;
        options.from = fx.root / "nothing.exe";
        options.to = fx.new_exe;
        options.build = "new";
        options.from_build = "old";
        write(fx.symbols, placeable());
        CHECK(port_symbols(options).exit_code == kExitCannotCheck);
        options.from = fx.old_exe;
        options.from_build.clear();
        CHECK(contains(port_symbols(options).cannot_port, "--from-build"));
    });

    run("port: --build comes from the table of builds, or says it does not", [&] {
        PortOptions options;
        options.symbols = fx.symbols;
        options.from = fx.old_exe;
        options.to = fx.new_exe;
        options.build = "eu4-9.9.9-gog-win64";
        CHECK(contains(port_build_problem(options), "--to is not a build this Anvil knows"));
        CHECK(contains(port_build_problem(options), "--unknown-build"));
        options.unknown_build = true;
        CHECK(port_build_problem(options).empty());
        for (const char* id : {"eu4-1.38-steam-win64", "eu4-1.38.0.1-gog-win64"}) {
            options.build = id;
            CHECK(port_build_problem(options).empty());
        }
        for (const char* id : {"new", "not a build id!", "eu4-1.38-win64", "eu4--steam-win64", "eu4-1.38.-steam-win64",
                               "eu4-1.38-Steam-win64", "eu4-1.38-steam-win32", "eu4-v1.38-steam-win64"}) {
            options.build = id;
            CHECK(contains(port_build_problem(options), "--build must have the form"));
        }
        // A file it cannot read is for the port to report.
        options.to = fx.root / "nothing.exe";
        options.unknown_build = false;
        CHECK(port_build_problem(options).empty());
    });

    run("port: --unknown-build holds an id of the table to the hash of --to (anvil#545, CLI4)", [&] {
        const KnownBuild* steam = find_known_build_by_id("eu4-1.37.5-steam-win64");
        const KnownBuild* gog = find_known_build_by_id("eu4-1.37.5.2-gog-win64");
        CHECK(steam != nullptr && gog != nullptr && find_known_build_by_id("eu4-9.9.9-steam-win64") == nullptr);
        if (steam == nullptr || gog == nullptr) return;
        PortOptions options;
        options.symbols = fx.symbols;
        options.from = fx.old_exe;
        options.to = fx.new_exe;
        options.unknown_build = true;
        // --to is the GOG build: the Steam id would put Steam's name on GOG's addresses.
        options.build = steam->id;
        const std::string wrong = port_build_problem(options, gog->sha256);
        CHECK(contains(wrong, "--build eu4-1.37.5-steam-win64 is a build this Anvil knows, and --to is eu4-1.37.5.2-gog-win64"));
        // A --to the table does not have cannot be a build it has.
        CHECK(contains(port_build_problem(options, sha256_file(fx.new_exe)), "and --to is not that build"));
        CHECK(contains(port_build_problem(options), "and --to is not that build"));
        // Its own id, and an id of a trial that the table does not have, still go.
        options.build = gog->id;
        CHECK(port_build_problem(options, gog->sha256).empty());
        options.build = "eu4-0.0.0-test-win64";
        CHECK(port_build_problem(options, gog->sha256).empty());
        // Without the flag nothing changed: the id is the table's for --to.
        options.unknown_build = false;
        CHECK(contains(port_build_problem(options, gog->sha256), "--to is eu4-1.37.5.2-gog-win64, not eu4-0.0.0-test-win64"));
        options.build = gog->id;
        CHECK(port_build_problem(options, gog->sha256).empty());

        // The report of a trial on a known --to says the file does not load there, and gives no "Next".
        PortReport report;
        report.from_build = steam->id;
        report.build = "eu4-0.0.0-test-win64";
        report.to_build = gog->id;
        report.written = true;
        const std::string text = format_port_report(report, fx.symbols, Format::Text);
        CHECK(contains(text, "--to is eu4-1.37.5.2-gog-win64, which this Anvil knows: a file that declares "
                             "eu4-0.0.0-test-win64 does not load on it."));
        CHECK(!contains(text, "Next:"));
        CHECK(contains(format_port_report(report, fx.symbols, Format::Json), "\"to_build\": \"eu4-1.37.5.2-gog-win64\""));
        report.build = gog->id;
        report.build_known = true;
        CHECK(contains(format_port_report(report, fx.symbols, Format::Text), "Next: anvil check <plugin-dir> --exe <the new eu4.exe>.\n"));
    });

    run("port reads a malformed new build only within its bytes", [&] {
        const auto port_to = [&](const std::vector<uint8_t>& bytes, const char* name) {
            const fs::path exe = fx.root / name / "eu4.exe";
            write(exe, std::string(bytes.begin(), bytes.end()));
            write(fx.symbols, placeable());
            PortOptions options;
            options.symbols = fx.symbols;
            options.from = fx.old_exe;
            options.to = exe;
            options.build = "new";
            options.from_build = "old";
            return port_symbols(options);
        };
        // An exception directory whose end wraps around 32 bits.
        TestImage wrapping;
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, &wrapping.mapped[0x80], sizeof(nt));
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {0x2000, 0xFFFFF000u};
        wrapping.put_value(0x80, nt);
        CHECK(port_to(wrapping.file(), "wrapping").exit_code != kExitOk);
        // No code at all: .text is all int3.
        TestImage empty;
        std::fill(empty.mapped.begin() + 0x1000, empty.mapped.begin() + 0x1100, uint8_t{0xCC});
        const PortReport none = port_to(empty.file(), "no-code");
        CHECK(none.exit_code != kExitOk);
        CHECK(none.ported.empty());
    });

    run("port without --out writes nothing", [&] {
        const PortReport report = fx.port(placeable(), false);
        CHECK(report.exit_code == kExitOk && !report.written);
        CHECK(!fs::exists(fx.out));
        CHECK(contains(format_port_report(report, fx.symbols, Format::Text), "nothing written"));
    });

    run("port: --out may be a bare file name in the current folder", [&] {
        const fs::path before = fs::current_path();
        fs::current_path(fx.root);
        write(fx.symbols, placeable());
        PortOptions options;
        options.symbols = fx.symbols;
        options.from = fx.old_exe;
        options.to = fx.new_exe;
        options.build = "new";
        options.from_build = "old";
        options.out = "bare.toml";
        const PortReport report = port_symbols(options);
        fs::current_path(before);
        if (!report.cannot_port.empty()) std::printf("  cannot: %s\n", report.cannot_port.c_str());
        CHECK(report.exit_code == kExitOk && report.written);
        CHECK(fx.verifies(read(fx.root / "bare.toml"), "new", fx.new_exe));
    });

    run("port: a failed write says why", [&] {
        write(fx.symbols, placeable());
        PortOptions options;
        options.symbols = fx.symbols;
        options.from = fx.old_exe;
        options.to = fx.new_exe;
        options.build = "new";
        options.from_build = "old";
        options.out = fx.symbols / "inside-a-file.toml";  // its parent is a file
        const PortReport report = port_symbols(options);
        CHECK(report.exit_code != kExitOk && !report.written);
        CHECK(contains(report.cannot_port, "cannot write"));
        CHECK(contains(report.cannot_port, "(error "));
    });
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    port_tests();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
