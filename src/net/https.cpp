#include "https.hpp"

#include "i18n/translate.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace anvil::net {
namespace {

std::mutex g_requests_mutex;
std::vector<Request*> g_requests;  // the Requests alive whose handle is open
bool g_cancelled = false;

using i18n::N_;

struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() {
        if (handle != nullptr) WinHttpCloseHandle(handle);
    }
};

// `error` in the player's language, `english` (when given) as written.
void fail(std::string& error, std::string* english, const char* pattern, const std::vector<std::string>& args) {
    error = i18n::format(i18n::tr(pattern), args);
    if (english != nullptr) *english = i18n::format(pattern, args);
}

int to_int(Millis ms) { return static_cast<int>(std::clamp<Millis::rep>(ms.count(), 0, 0x7fffffff)); }

}  // namespace

Timeouts small_request_timeouts() {
    Timeouts out;
    out.deadline = Millis(60'000);
    return out;
}

Timeouts download_timeouts() {
    Timeouts out;
    out.stall = Millis(60'000);
    return out;
}

Overrun overrun(const Timeouts& timeouts, Clock::time_point start, Clock::time_point last_byte, Clock::time_point now) {
    if (timeouts.deadline && now - start >= *timeouts.deadline) return Overrun::deadline;
    if (timeouts.stall && now - std::max(start, last_byte) >= *timeouts.stall) return Overrun::stall;
    return Overrun::none;
}

std::optional<Clock::time_point> next_overrun(const Timeouts& timeouts, Clock::time_point start,
                                              Clock::time_point last_byte) {
    std::optional<Clock::time_point> out;
    if (timeouts.deadline) out = start + *timeouts.deadline;
    if (timeouts.stall) {
        const Clock::time_point stall = std::max(start, last_byte) + *timeouts.stall;
        if (!out || stall < *out) out = stall;
    }
    return out;
}

bool set_timeouts(void* session, const Timeouts& timeouts) {
    return session != nullptr && WinHttpSetTimeouts(session, to_int(timeouts.resolve), to_int(timeouts.connect),
                                                    to_int(timeouts.send), to_int(timeouts.receive)) != FALSE;
}

Request::Request(void* handle) {
    if (handle == nullptr) return;
    std::lock_guard lock(g_requests_mutex);
    if (g_cancelled) {
        WinHttpCloseHandle(handle);
        return;
    }
    g_requests.push_back(this);
    handle_ = handle;
    open_ = true;
}

Request::~Request() { close(); }

void Request::close() {
    std::lock_guard lock(g_requests_mutex);
    if (!open_) return;  // never opened, or closed already
    open_ = false;
    g_requests.erase(std::find(g_requests.begin(), g_requests.end(), this));
    WinHttpCloseHandle(handle_);
}

void cancel_all() {
    std::lock_guard lock(g_requests_mutex);
    g_cancelled = true;
    for (Request* request : g_requests) {
        request->open_ = false;
        WinHttpCloseHandle(request->handle_);
    }
    g_requests.clear();
}

Watchdog::Watchdog(Request& request, const Timeouts& timeouts)
    : request_(request), timeouts_(timeouts), start_(Clock::now()), last_byte_(start_) {
    if (timeouts_.deadline || timeouts_.stall) thread_ = std::thread([this] { watch(); });
}

Watchdog::~Watchdog() {
    {
        std::lock_guard lock(mutex_);
        done_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Watchdog::progress() {
    std::lock_guard lock(mutex_);
    last_byte_ = Clock::now();
}

bool Watchdog::fired() const {
    std::lock_guard lock(mutex_);
    return fired_;
}

void Watchdog::watch() {
    std::unique_lock lock(mutex_);
    while (!done_) {
        if (overrun(timeouts_, start_, last_byte_, Clock::now()) != Overrun::none) {
            fired_ = true;
            lock.unlock();
            request_.close();  // ends the WinHTTP call that waits
            return;
        }
        // progress() moves the limit later; waking at the old one just waits again.
        wake_.wait_until(lock, *next_overrun(timeouts_, start_, last_byte_));
    }
}

std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const Timeouts& timeouts, const std::string& name, const std::wstring& agent,
                                     std::string& error, std::string* english_error) {
    Internet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.handle != nullptr && !set_timeouts(session.handle, timeouts)) {
        fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
        return std::nullopt;
    }
    Internet connection{session.handle != nullptr ? WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr};
    Request request{connection.handle != nullptr
                        ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr};
    Watchdog watchdog(request, timeouts);
    // A handle that cancel_all() or the watchdog closes fails the call it is in, and every later one.
    const HINTERNET handle = request.handle();
    // The error of a failed call: a timeout when the watchdog ended it.
    auto unreachable = [&] {
        const DWORD code = watchdog.fired() ? ERROR_WINHTTP_TIMEOUT : GetLastError();
        fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(code)});
        return std::nullopt;
    };
    if (handle == nullptr ||
        !WinHttpSendRequest(handle, headers, headers != nullptr ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(handle, nullptr)) {
        return unreachable();
    }
    watchdog.progress();  // the headers
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        fail(error, english_error, N_("{0} answered {1}"), {name, std::to_string(status)});
        return std::nullopt;
    }
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle, &available)) return unreachable();
        if (available == 0) break;
        const size_t at = body.size();
        if (at + available > limit) {
            fail(error, english_error, N_("{0} sent more than expected"), {name});
            return std::nullopt;
        }
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle, body.data() + at, available, &read)) return unreachable();
        body.resize(at + read);
        if (read > 0) watchdog.progress();
    }
    return body;
}

}  // namespace anvil::net
