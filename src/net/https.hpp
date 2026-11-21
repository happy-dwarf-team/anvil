#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace anvil::net {

using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

// How long a request may take (anvil#540). The first four are WinHTTP's own
// (WinHttpSetTimeouts on the session): name resolution, connect, each send and
// each receive. `deadline` limits the whole request from its start, `stall`
// the time without a byte from the server (counted from the start until the
// first one); a Watchdog closes the request when either passes.
struct Timeouts {
    Millis resolve{10'000};
    Millis connect{10'000};
    Millis send{30'000};
    Millis receive{30'000};
    std::optional<Millis> deadline;
    std::optional<Millis> stall;
};

// The release check, the catalog and other small requests: 60 s in all.
Timeouts small_request_timeouts();
// A zip download (up to 256 MiB): no total limit, but 60 s without a byte ends it.
Timeouts download_timeouts();

// Which limit a request has passed at `now`, having started at `start` and
// received its last byte at `last_byte` (`start` before the first one).
enum class Overrun { none, deadline, stall };
Overrun overrun(const Timeouts& timeouts, Clock::time_point start, Clock::time_point last_byte, Clock::time_point now);
// The earliest time overrun() can answer other than none, for the same
// request; nullopt when it has neither a deadline nor a stall limit.
std::optional<Clock::time_point> next_overrun(const Timeouts& timeouts, Clock::time_point start,
                                              Clock::time_point last_byte);

// Sets the WinHTTP timeouts of `timeouts` on a session handle (from WinHttpOpen),
// for the requests opened on it after.
bool set_timeouts(void* session, const Timeouts& timeouts);

// One HTTPS GET of `host` + `path` with WinHTTP, redirects followed as WinHTTP
// does (never from https to http), sent as `agent`, limited by `timeouts`. The
// body when the answer is 200 and at most `limit` bytes; nullopt with `error`
// (in the player's language, naming `name`) otherwise, and the same in English
// in `english_error` when it is given (for anvil.log). A request a limit of
// `timeouts` ends fails with WinHTTP's timeout error (12002). Blocks: call it on
// a worker thread.
std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const Timeouts& timeouts, const std::string& name, const std::wstring& agent,
                                     std::string& error, std::string* english_error = nullptr);

// A WinHTTP request handle (from WinHttpOpenRequest) that cancel_all() or
// close() may close from another thread, which ends a blocking WinHTTP call on
// it. It closes the handle at its end unless one of them did; after
// cancel_all() it closes a new handle at once and holds none.
class Request {
public:
    explicit Request(void* handle);
    ~Request();
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    // The handle as opened, also after it was closed: a closed handle fails
    // every WinHTTP call on it.
    void* handle() const { return handle_; }
    // Closes the handle now, from any thread; nothing when it is closed already.
    void close();

private:
    friend void cancel_all();
    void* handle_ = nullptr;
    bool open_ = false;  // under the registry's lock
};

// Ends every request of this process that runs through a Request, and refuses
// later ones: for a process that is closing (the Manager when its window
// closes), whose worker threads would otherwise wait for their transfers.
void cancel_all();

// Closes `request` when its deadline or stall limit passes (overrun()), on a
// thread of its own that ends with the Watchdog. Call progress() after every
// byte received. Destroy it before the Request.
class Watchdog {
public:
    Watchdog(Request& request, const Timeouts& timeouts);
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    void progress();
    // Whether it closed the request.
    bool fired() const;

private:
    void watch();

    Request& request_;
    const Timeouts timeouts_;
    const Clock::time_point start_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Clock::time_point last_byte_;
    bool done_ = false;
    bool fired_ = false;
    std::thread thread_;
};

}  // namespace anvil::net
