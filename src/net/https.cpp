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
std::vector<HINTERNET> g_requests;  // the open handles of the Requests alive
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

}  // namespace

Request::Request(void* handle) {
    if (handle == nullptr) return;
    std::lock_guard lock(g_requests_mutex);
    if (g_cancelled) {
        WinHttpCloseHandle(handle);
        return;
    }
    g_requests.push_back(handle);
    handle_ = handle;
}

Request::~Request() {
    if (handle_ == nullptr) return;
    std::lock_guard lock(g_requests_mutex);
    const auto it = std::find(g_requests.begin(), g_requests.end(), handle_);
    if (it == g_requests.end()) return;  // cancel_all closed it
    g_requests.erase(it);
    WinHttpCloseHandle(handle_);
}

void cancel_all() {
    std::lock_guard lock(g_requests_mutex);
    g_cancelled = true;
    for (HINTERNET handle : g_requests) WinHttpCloseHandle(handle);
    g_requests.clear();
}

std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const std::string& name, const std::wstring& agent, std::string& error,
                                     std::string* english_error) {
    Internet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0)};
    Internet connection{session.handle != nullptr ? WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr};
    const Request request{connection.handle != nullptr
                              ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr};
    // A handle that cancel_all() closes fails the call it is in, and every later one.
    const HINTERNET handle = request.handle();
    if (handle == nullptr ||
        !WinHttpSendRequest(handle, headers, headers != nullptr ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(handle, nullptr)) {
        fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
        return std::nullopt;
    }
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
        if (!WinHttpQueryDataAvailable(handle, &available)) {
            fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        if (available == 0) break;
        const size_t at = body.size();
        if (at + available > limit) {
            fail(error, english_error, N_("{0} sent more than expected"), {name});
            return std::nullopt;
        }
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(handle, body.data() + at, available, &read)) {
            fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        body.resize(at + read);
    }
    return body;
}

}  // namespace anvil::net
