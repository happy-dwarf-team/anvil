// PROTOTYPE of the header-only C++ wrapper over anvil.h (API 2). Throwaway.
//
// What it adds over the C API:
//   - typed symbols from the generated header: a hook detour or a call must match
//     the signature declared in symbols.toml, or the plugin does not compile;
//   - load() is straight-line code: a failed call throws, ANVIL_PLUGIN turns that
//     into a result, and Anvil rolls back every change the plugin staged;
//   - std::format logging; mid hooks take any callable, lambdas with captures included;
//   - Change is a move-only handle; destroying it removes the change.
#pragma once

#include "anvil.h"

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace anvil {

using Cpu = AnvilCpuContext;

// --- Symbol kinds, as emitted by `anvil gen` ---------------------------------
struct Site { AnvilSymbol id; };              // a place in code: mid hook or byte patch
struct Slot { AnvilSymbol id; };              // a pointer slot: pointer swap
template <class T> struct Data { AnvilSymbol id; };  // a global or field to read
template <class F> struct Function;           // a function with its declared signature
template <class R, class... A> struct Function<R(A...)> {
    using Signature = R(A...);
    AnvilSymbol id;
};

struct Error {
    AnvilResult result;
    const char* what;
};

namespace detail {
inline const AnvilHost* host = nullptr;
inline AnvilPlugin* self = nullptr;
inline void check(AnvilResult r, const char* what) {
    if (r != ANVIL_OK) throw Error{r, what};
}
}  // namespace detail

class Change {
public:
    Change() = default;
    explicit Change(AnvilChange* c) : c_(c) {}
    Change(Change&& o) noexcept : c_(std::exchange(o.c_, nullptr)) {}
    Change& operator=(Change&& o) noexcept { reset(); c_ = std::exchange(o.c_, nullptr); return *this; }
    Change(const Change&) = delete;
    Change& operator=(const Change&) = delete;
    ~Change() { reset(); }
    void reset() { if (c_) detail::host->remove(detail::self, std::exchange(c_, nullptr)); }
private:
    AnvilChange* c_ = nullptr;
};

template <class F> class FunctionHook;
template <class R, class... A> class FunctionHook<R(A...)> {
public:
    R next(A... args) const { return next_(args...); }  // the rest of the chain
    void remove() { change_.reset(); }
private:
    friend class Host;
    Change change_;
    R (*next_)(A...) = nullptr;
};
// anvil::HookOf<sym::speed_object> is the hook type for that symbol.
template <auto Symbol> using HookOf = FunctionHook<typename decltype(Symbol)::Signature>;

class Host {
public:
    const AnvilGameInfo& game() const { return *h().game; }

    template <class... Args> void info(std::format_string<Args...> f, Args&&... a) { log(ANVIL_LOG_INFO, f, std::forward<Args>(a)...); }
    template <class... Args> void warn(std::format_string<Args...> f, Args&&... a) { log(ANVIL_LOG_WARNING, f, std::forward<Args>(a)...); }

    // Function hook: the detour must have exactly the declared signature.
    template <class R, class... A>
    void hook(FunctionHook<R(A...)>& hook, Function<R(A...)> symbol, R (*detour)(A...)) {
        AnvilChange* c = nullptr;
        void* next = nullptr;
        detail::check(h().hook_function(self(), symbol.id, reinterpret_cast<void*>(detour), &next, &c), "hook_function");
        hook.change_ = Change(c);
        hook.next_ = reinterpret_cast<R (*)(A...)>(next);
    }

    // Mid hook: any callable taking Cpu&. Captured state lives as long as the change.
    template <class F>
    [[nodiscard]] Change mid_hook(Site site, F&& callback) {
        auto* fn = new std::function<void(Cpu&)>(std::forward<F>(callback));  // kept for the process lifetime
        AnvilChange* c = nullptr;
        detail::check(h().mid_hook(self(), site.id,
                                   [](AnvilCpuContext* cpu, void* user) { (*static_cast<std::function<void(Cpu&)>*>(user))(*cpu); },
                                   fn, &c),
                      "mid_hook");
        return Change(c);
    }

    // Calling a game function: arguments are checked against the declared signature.
    template <class R, class... A, class... Args>
    R call(Function<R(A...)> f, Args&&... args) {
        return reinterpret_cast<R (*)(A...)>(h().address_of(self(), f.id))(std::forward<Args>(args)...);
    }

    template <class T> T* address(Data<T> d) { return static_cast<T*>(h().address_of(self(), d.id)); }

private:
    static const AnvilHost& h() { return *detail::host; }
    static AnvilPlugin* self() { return detail::self; }
    template <class... Args> void log(AnvilLogLevel level, std::format_string<Args...> f, Args&&... a) {
        const std::string text = std::format(f, std::forward<Args>(a)...);
        h().log(self(), level, text.c_str());
    }
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual void load(Host& host) = 0;  // throw (or let a Host call throw) to refuse loading
};

}  // namespace anvil

// One line per plugin: exports anvil_plugin_load and creates the plugin object,
// which lives for the whole process (it is never destroyed).
#define ANVIL_PLUGIN(Type)                                                                       \
    extern "C" ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost* h, AnvilPlugin* s) { \
        if (h == nullptr || h->api_version != ANVIL_API_VERSION) return ANVIL_ERROR_UNSUPPORTED;     \
        ::anvil::detail::host = h;                                                               \
        ::anvil::detail::self = s;                                                               \
        try {                                                                                    \
            ::anvil::Host host;                                                                  \
            (new Type)->load(host);                                                              \
            return ANVIL_OK;                                                                     \
        } catch (const ::anvil::Error& e) {                                                      \
            h->log(s, ANVIL_LOG_ERROR, e.what);                                                  \
            return e.result;                                                                     \
        } catch (...) {                                                                          \
            return ANVIL_ERROR_INTERNAL;                                                         \
        }                                                                                        \
    }
