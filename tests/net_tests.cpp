// anvil_net (src/net) on this machine alone: cancel_all() ends a request that
// waits on a server, and refuses later ones. The server is a TCP listener on
// 127.0.0.1:443 that takes the connection and never answers the TLS
// handshake, so WinHTTP waits (its own timeouts are 30 s) until the request
// is cancelled. When the port cannot be taken the test says so and passes.
// No request leaves the machine.

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

using Clock = std::chrono::steady_clock;

}  // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 1;
    }
    run("net: cancel_all ends a request that waits on a silent server, and refuses later ones", [] {
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(443);
        inet_pton(AF_INET, "127.0.0.1", &at.sin_addr);
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0 ||
            listen(listener, 4) != 0) {
            std::printf("    skipped: 127.0.0.1:443 cannot be taken here (error %d)\n", WSAGetLastError());
            if (listener != INVALID_SOCKET) closesocket(listener);
            return;
        }
        // Takes connections and holds them open, silent, until the test ends.
        std::atomic<int> accepted{0};
        std::thread server([&] {
            SOCKET held[4];
            int n = 0;
            for (;;) {
                const SOCKET c = accept(listener, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                if (n < 4) held[n++] = c;
                else closesocket(c);
                ++accepted;
            }
            for (int i = 0; i < n; ++i) closesocket(held[i]);
        });

        auto get = [] {
            std::string error;
            const auto body = anvil::net::https_get(L"127.0.0.1", L"/", nullptr, 1024, "the test server",
                                                    L"anvil-net-test", error);
            return body.has_value() ? std::string("answered") : error;
        };
        auto first = std::async(std::launch::async, get);
        // The request has connected and waits for the server's handshake.
        const auto until = Clock::now() + std::chrono::seconds(10);
        while (accepted.load() == 0 && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(accepted.load() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(first.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);  // still waiting

        const auto cancelled_at = Clock::now();
        anvil::net::cancel_all();
        const bool ended = first.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        CHECK(ended);
        if (ended) {
            const std::string error = first.get();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cancelled_at).count();
            std::printf("    ended %lld ms after cancel_all: %s\n", static_cast<long long>(ms), error.c_str());
            CHECK(error != "answered" && !error.empty());
        }

        // Later requests fail without connecting.
        const auto started = Clock::now();
        const std::string later = get();
        CHECK(later != "answered" && !later.empty());
        CHECK(Clock::now() - started < std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(accepted.load() == 1);

        closesocket(listener);  // ends accept()
        server.join();
        if (!ended) first.wait();  // WinHTTP's own timeout ends it
    });
    WSACleanup();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
