#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace anvil::net {

// One HTTPS GET of `host` + `path` with WinHTTP, redirects followed as WinHTTP
// does (never from https to http), sent as `agent`. The body when the answer
// is 200 and at most `limit` bytes; nullopt with `error` (in the player's
// language, naming `name`) otherwise, and the same in English in
// `english_error` when it is given (for anvil.log). Blocks: call it on a
// worker thread.
std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const std::string& name, const std::wstring& agent, std::string& error,
                                     std::string* english_error = nullptr);

// A WinHTTP request handle (from WinHttpOpenRequest) that cancel_all() may close
// from another thread, which ends a blocking WinHTTP call on it. It closes the
// handle at its end unless cancel_all() did; after cancel_all() it closes a new
// handle at once and holds none.
class Request {
public:
    explicit Request(void* handle);
    ~Request();
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    void* handle() const { return handle_; }

private:
    void* handle_ = nullptr;
};

// Ends every request of this process that runs through a Request, and refuses
// later ones: for a process that is closing (the Manager when its window
// closes), whose worker threads would otherwise wait for their transfers.
void cancel_all();

}  // namespace anvil::net
