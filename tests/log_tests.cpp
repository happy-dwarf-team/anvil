// anvil.log after the game's own restart (ADR 0012): the new process starts while
// the old one still holds its log, so open() waits for the file instead of losing
// the whole log. A process of its own, since the log is opened once per process.
#include "core/log.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

int main() {
    run("log: a log still held by the replaced process opens once it is free", [] {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path dir =
            std::filesystem::path(temp) / (L"anvil-log-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "anvil.log";
        // The old process: the log open for writing, shared for reading only, as Anvil opens it.
        HANDLE old = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(old != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(old, "old process\r\n", 13, &written, nullptr);

        anvil::Log& log = anvil::Log::instance();
        log.write(ANVIL_LOG_INFO, "test", "before open");
        log.open(file);
        CHECK(!log.is_open());
        log.write(ANVIL_LOG_INFO, "test", "while the old process quits");
        CHECK(!log.is_open() && read(file) == "old process\r\n");

        CloseHandle(old);
        Sleep(300);
        log.write(ANVIL_LOG_INFO, "test", "after it quit");
        CHECK(log.is_open());
        const std::string now = read(file);
        CHECK(now.find("before open") != std::string::npos);
        CHECK(now.find("while the old process quits") != std::string::npos);
        CHECK(now.find("anvil.log was still in use by the process this one replaced") != std::string::npos);
        CHECK(now.find("after it quit") != std::string::npos);
        CHECK(now.find("before open") < now.find("after it quit"));
        CHECK(read(dir / "anvil.prev.log") == "old process\r\n");

        // The Manager's Log page reads each line's level back from what write() wrote.
        log.write(ANVIL_LOG_INFO, "test", "level info");
        log.write(ANVIL_LOG_WARNING, "test", "level warning");
        log.write(ANVIL_LOG_ERROR, "test", "level error ms] WARN  inside");
        std::istringstream lines(read(file));
        int seen = 0;
        for (std::string line; std::getline(lines, line);) {
            const auto level = anvil::log_line_level(line);
            for (const auto& [text, want] : {std::pair{"level info", ANVIL_LOG_INFO}, std::pair{"level warning", ANVIL_LOG_WARNING},
                                             std::pair{"level error", ANVIL_LOG_ERROR}}) {
                if (line.find(text) == std::string::npos) continue;
                CHECK(level == want);
                ++seen;
            }
        }
        CHECK(seen == 3);
        CHECK(!anvil::log_line_level("old process"));
        CHECK(!anvil::log_line_level("[ not a time ] WARN  x"));
    });
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
