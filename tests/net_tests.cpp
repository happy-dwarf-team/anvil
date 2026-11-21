// anvil_net (src/net) on this machine alone: the limits of a request
// (anvil#540) as rules on given times, then against a silent server: the
// watchdog's deadline and stall limit and WinHTTP's own receive timeout end a
// request that waits, and cancel_all() ends one and refuses later ones (last,
// since nothing runs after it). The server is a TCP listener on 127.0.0.1:443
// that takes connections and never answers the TLS handshake. When the port
// cannot be taken the server tests say so and pass. No request leaves the
// machine.

#include "net/https.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <string>
#include <thread>
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

using anvil::net::Clock;
using anvil::net::Millis;
using anvil::net::Overrun;
using anvil::net::Timeouts;
using std::chrono::seconds;

// Takes connections on 127.0.0.1:443 and holds them open, silent, until it ends.
class SilentServer {
public:
    SilentServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(443);
        inet_pton(AF_INET, "127.0.0.1", &at.sin_addr);
        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0 ||
            listen(listener_, 16) != 0) {
            error_ = WSAGetLastError();
            if (listener_ != INVALID_SOCKET) closesocket(listener_);
            listener_ = INVALID_SOCKET;
            return;
        }
        thread_ = std::thread([this] {
            std::vector<SOCKET> held;
            for (;;) {
                const SOCKET c = accept(listener_, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                held.push_back(c);
                ++accepted_;
            }
            for (SOCKET c : held) closesocket(c);
        });
    }
    ~SilentServer() {
        if (listener_ == INVALID_SOCKET) return;
        closesocket(listener_);  // ends accept()
        thread_.join();
    }
    bool up() const { return listener_ != INVALID_SOCKET; }
    int error() const { return error_; }
    int accepted() const { return accepted_.load(); }

private:
    SOCKET listener_ = INVALID_SOCKET;
    int error_ = 0;
    std::atomic<int> accepted_{0};
    std::thread thread_;
};

SilentServer* g_server = nullptr;

bool server_up() {
    if (g_server->up()) return true;
    std::printf("    skipped: 127.0.0.1:443 cannot be taken here (error %d)\n", g_server->error());
    return false;
}

// A GET of the silent server: "answered" or the English error.
std::string get(const Timeouts& timeouts) {
    std::string error;
    std::string english;
    const auto body = anvil::net::https_get(L"127.0.0.1", L"/", nullptr, 1024, timeouts, "the test server",
                                            L"anvil-net-test", error, &english);
    return body.has_value() ? std::string("answered") : english;
}

// WinHTTP's own limits far beyond the test's, so only the watchdog ends it.
Timeouts long_winhttp() {
    Timeouts out;
    out.send = Millis(120'000);
    out.receive = Millis(120'000);
    return out;
}

// A GET with `timeouts` against the silent server ends within `within` with
// WinHTTP's timeout error, and not before `not_before`.
void ends_with_timeout(const Timeouts& timeouts, Millis not_before, Millis within) {
    const auto started = Clock::now();
    auto pending = std::async(std::launch::async, get, timeouts);
    const bool ended = pending.wait_for(within) == std::future_status::ready;
    CHECK(ended);
    if (!ended) return;  // the future's destructor waits for WinHTTP's own limit
    const std::string error = pending.get();
    const auto ms = std::chrono::duration_cast<Millis>(Clock::now() - started);
    std::printf("    ended after %lld ms: %s\n", static_cast<long long>(ms.count()), error.c_str());
    CHECK(error == "cannot reach the test server (error 12002)");
    CHECK(ms >= not_before);
}

}  // namespace

int main() {
    run("net: the limits anvil#540 decided", [] {
        const Timeouts small = anvil::net::small_request_timeouts();
        CHECK(small.resolve == seconds(10) && small.connect == seconds(10));
        CHECK(small.send == seconds(30) && small.receive == seconds(30));
        CHECK(small.deadline == seconds(60) && !small.stall);
        const Timeouts download = anvil::net::download_timeouts();
        CHECK(download.resolve == seconds(10) && download.connect == seconds(10));
        CHECK(download.send == seconds(30) && download.receive == seconds(30));
        CHECK(!download.deadline && download.stall == seconds(60));
    });

    run("net: a deadline counts from the start, whatever arrives", [] {
        const Timeouts small = anvil::net::small_request_timeouts();
        const Clock::time_point t0{};
        CHECK(anvil::net::overrun(small, t0, t0, t0) == Overrun::none);
        CHECK(anvil::net::overrun(small, t0, t0 + seconds(59), t0 + seconds(59)) == Overrun::none);
        CHECK(anvil::net::overrun(small, t0, t0 + seconds(59), t0 + seconds(60)) == Overrun::deadline);
        CHECK(anvil::net::overrun(small, t0, t0, t0 + seconds(300)) == Overrun::deadline);
        CHECK(anvil::net::next_overrun(small, t0, t0 + seconds(30)) == t0 + seconds(60));
    });

    run("net: a stall counts from the last byte, and from the start before the first", [] {
        const Timeouts download = anvil::net::download_timeouts();
        const Clock::time_point t0{};
        CHECK(anvil::net::overrun(download, t0, t0, t0 + seconds(59)) == Overrun::none);
        CHECK(anvil::net::overrun(download, t0, t0, t0 + seconds(60)) == Overrun::stall);
        // A byte every 59 s keeps a download going for an hour.
        CHECK(anvil::net::overrun(download, t0, t0 + seconds(3600), t0 + seconds(3659)) == Overrun::none);
        CHECK(anvil::net::overrun(download, t0, t0 + seconds(3600), t0 + seconds(3660)) == Overrun::stall);
        CHECK(anvil::net::next_overrun(download, t0, t0) == t0 + seconds(60));
        CHECK(anvil::net::next_overrun(download, t0, t0 + seconds(100)) == t0 + seconds(160));
    });

    run("net: with both limits the earlier one ends it; with neither nothing does", [] {
        Timeouts both;
        both.deadline = seconds(60);
        both.stall = seconds(20);
        const Clock::time_point t0{};
        CHECK(anvil::net::overrun(both, t0, t0 + seconds(30), t0 + seconds(50)) == Overrun::stall);
        CHECK(anvil::net::next_overrun(both, t0, t0 + seconds(30)) == t0 + seconds(50));
        CHECK(anvil::net::overrun(both, t0, t0 + seconds(55), t0 + seconds(60)) == Overrun::deadline);
        CHECK(anvil::net::next_overrun(both, t0, t0 + seconds(55)) == t0 + seconds(60));
        const Timeouts neither;
        CHECK(anvil::net::overrun(neither, t0, t0, t0 + seconds(100000)) == Overrun::none);
        CHECK(!anvil::net::next_overrun(neither, t0, t0));
    });

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 1;
    }
    {
        SilentServer server;
        g_server = &server;

        run("net: the deadline ends a request that waits on a silent server", [] {
            if (!server_up()) return;
            Timeouts timeouts = long_winhttp();
            timeouts.deadline = Millis(1000);
            ends_with_timeout(timeouts, Millis(900), Millis(10'000));
        });

        run("net: the stall limit ends a request that gets no byte from a silent server", [] {
            if (!server_up()) return;
            Timeouts timeouts = long_winhttp();
            timeouts.stall = Millis(1000);
            ends_with_timeout(timeouts, Millis(900), Millis(10'000));
        });

        run("net: WinHTTP's receive timeout is set on the session", [] {
            if (!server_up()) return;
            Timeouts timeouts;
            timeouts.receive = Millis(1000);  // the TLS handshake waits for the server's hello
            ends_with_timeout(timeouts, Millis(900), Millis(10'000));
        });

        run("net: cancel_all ends a request that waits on a silent server, and refuses later ones", [] {
            if (!server_up()) return;
            const int before = g_server->accepted();
            auto first = std::async(std::launch::async, get, long_winhttp());
            // The request has connected and waits for the server's handshake.
            const auto until = Clock::now() + seconds(10);
            while (g_server->accepted() == before && Clock::now() < until) {
                std::this_thread::sleep_for(Millis(10));
            }
            CHECK(g_server->accepted() == before + 1);
            std::this_thread::sleep_for(Millis(300));
            CHECK(first.wait_for(seconds(0)) == std::future_status::timeout);  // still waiting

            const auto cancelled_at = Clock::now();
            anvil::net::cancel_all();
            const bool ended = first.wait_for(seconds(5)) == std::future_status::ready;
            CHECK(ended);
            if (ended) {
                const std::string error = first.get();
                const auto ms = std::chrono::duration_cast<Millis>(Clock::now() - cancelled_at).count();
                std::printf("    ended %lld ms after cancel_all: %s\n", static_cast<long long>(ms), error.c_str());
                CHECK(error != "answered" && !error.empty());
                CHECK(error != "cannot reach the test server (error 12002)");  // not a timeout
            }

            // Later requests fail without connecting.
            const auto started = Clock::now();
            const std::string later = get(long_winhttp());
            CHECK(later != "answered" && !later.empty());
            CHECK(Clock::now() - started < seconds(5));
            std::this_thread::sleep_for(Millis(200));
            CHECK(g_server->accepted() == before + 1);
        });
        g_server = nullptr;
    }
    WSACleanup();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
