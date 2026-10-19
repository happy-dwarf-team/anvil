//! Hello in Rust: a function hook and a mid hook on declared symbols and an
//! event subscription, with the raw API (`anvil-sys`) and the symbols that
//! `anvil gen --lang rust` writes from `symbols.toml`.
//!
//! The rules of the C ABI apply to every function Anvil or the game calls into:
//! unwinding out of one aborts the process, so each body runs under [`guard`],
//! which catches the panic, logs it and answers with a fallback.
#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::{c_char, c_void, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

use anvil_sys::{
    AnvilCpuContext, AnvilHost, AnvilLogLevel, AnvilPlugin, AnvilPluginLoadFn, AnvilResult,
    ANVIL_API_VERSION, ANVIL_ERROR_INTERNAL, ANVIL_ERROR_INVALID_ARGUMENT, ANVIL_ERROR_UNSUPPORTED,
    ANVIL_EVENT_PLUGINS_LOADED, ANVIL_LOG_ERROR, ANVIL_LOG_INFO, ANVIL_OK,
};

/// Symbol ids and function types from `symbols.toml` (`anvil gen --lang rust`, see build.rs).
#[allow(dead_code)]
mod sym {
    include!(concat!(env!("OUT_DIR"), "/symbols.gen.rs"));
}

// The host table and this plugin, kept for the callbacks. Anvil keeps both for
// the life of the process.
static HOST: AtomicPtr<AnvilHost> = AtomicPtr::new(ptr::null_mut());
static SELF: AtomicPtr<AnvilPlugin> = AtomicPtr::new(ptr::null_mut());
// The rest of the speed_object chain, written by Anvil before the hook is enabled.
static SPEED_NEXT: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());
static FIRST_SPEED_OBJECT: AtomicBool = AtomicBool::new(true);

fn log(level: AnvilLogLevel, message: &str) {
    let host = HOST.load(Ordering::Acquire);
    if host.is_null() {
        return;
    }
    let text: Vec<u8> = message
        .bytes()
        .filter(|&byte| byte != 0)
        .chain(std::iter::once(0))
        .collect();
    // SAFETY: the host table is valid for the life of the process; the message is NUL-terminated.
    unsafe {
        if let Some(write) = (*host).log {
            write(
                SELF.load(Ordering::Acquire),
                level,
                text.as_ptr().cast::<c_char>(),
            );
        }
    }
}

/// Runs `body`; a panic is logged and `fallback` is returned instead of unwinding
/// into Anvil or the game.
fn guard<R>(fallback: R, what: &str, body: impl FnOnce() -> R) -> R {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => {
            // The panic hook logged the message and the place.
            let _ = catch_unwind(|| log(ANVIL_LOG_ERROR, &format!("{what} panicked; carrying on")));
            fallback
        }
    }
}

/// The game's speed_object: pass the call on and log the first result.
unsafe extern "C" fn on_speed_object(session: *mut c_void) -> *mut c_void {
    let next = SPEED_NEXT.load(Ordering::Acquire);
    if next.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: Anvil wrote the rest of the chain, a function with the declared signature.
    let next = unsafe { std::mem::transmute::<*mut c_void, sym::SpeedObjectFn>(next) };
    // SAFETY: the game's own function, called as it declares.
    let speed = unsafe { next(session) };
    guard((), "speed_object hook", || {
        if FIRST_SPEED_OBJECT.swap(false, Ordering::Relaxed) {
            log(
                ANVIL_LOG_INFO,
                &format!("first game-speed object: {speed:p}"),
            );
        }
    });
    speed
}

/// A mid hook at the start of the block that returns to the menu.
unsafe extern "C" fn on_idle_exit(cpu: *mut AnvilCpuContext, _user: *mut c_void) {
    guard((), "idle_exit mid hook", || {
        // SAFETY: the context is valid during the callback.
        let rcx = unsafe { (*cpu).rcx };
        log(
            ANVIL_LOG_INFO,
            &format!("returning to the menu (rcx={rcx:#x})"),
        );
    });
}

unsafe extern "C" fn on_plugins_loaded(
    _event: *const c_char,
    _payload: *const c_void,
    _context: *mut c_void,
) {
    guard((), "plugins_loaded handler", || {
        log(ANVIL_LOG_INFO, "every plugin is loaded")
    });
}

/// # Safety
/// `host` is Anvil's host table (or null) and `me` is this plugin, valid for the life of the process.
unsafe fn load(host: *const AnvilHost, me: *mut AnvilPlugin) -> AnvilResult {
    // SAFETY: see above.
    let Some(table) = (unsafe { host.as_ref() }) else {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    };
    if table.api_version != ANVIL_API_VERSION {
        return ANVIL_ERROR_UNSUPPORTED;
    }
    HOST.store(host.cast_mut(), Ordering::Release);
    SELF.store(me, Ordering::Release);
    std::panic::set_hook(Box::new(|info| {
        log(ANVIL_LOG_ERROR, &format!("panic: {info}"))
    }));

    // SAFETY: the host fills game and anvil_version; build_id may be NULL.
    let version = unsafe { CStr::from_ptr(table.anvil_version) }
        .to_string_lossy()
        .into_owned();
    let build = unsafe { table.game.as_ref() }
        .filter(|game| !game.build_id.is_null())
        .map_or_else(
            || "unknown".to_owned(),
            |game| {
                unsafe { CStr::from_ptr(game.build_id) }
                    .to_string_lossy()
                    .into_owned()
            },
        );
    log(
        ANVIL_LOG_INFO,
        &format!("hello from Rust on Anvil {version}, game build {build}"),
    );

    let (Some(hook_function), Some(mid_hook), Some(subscribe)) =
        (table.hook_function, table.mid_hook, table.subscribe)
    else {
        return ANVIL_ERROR_UNSUPPORTED;
    };

    // On any failure return it: Anvil rolls back everything staged so far.
    // The generated type is the declared signature; a detour of another shape does not compile.
    let detour: sym::SpeedObjectFn = on_speed_object;
    // SAFETY: the symbol is declared and verified; the detour keeps its signature and never unwinds.
    let result = unsafe {
        hook_function(
            me,
            sym::SPEED_OBJECT,
            detour as usize as *mut c_void,
            SPEED_NEXT.as_ptr(),
            ptr::null_mut(),
        )
    };
    if result != ANVIL_OK {
        return result;
    }
    // SAFETY: as above; the callback never unwinds.
    let result = unsafe {
        mid_hook(
            me,
            sym::IDLE_EXIT,
            Some(on_idle_exit),
            ptr::null_mut(),
            ptr::null_mut(),
        )
    };
    if result != ANVIL_OK {
        return result;
    }
    // SAFETY: the event name is NUL-terminated; the handler never unwinds.
    unsafe {
        subscribe(
            me,
            ANVIL_EVENT_PLUGINS_LOADED.as_ptr().cast::<c_char>(),
            Some(on_plugins_loaded),
            ptr::null_mut(),
        )
    }
}

/// The entry point Anvil calls once, in the phase `plugin.toml` names.
///
/// # Safety
/// Called by Anvil with its host table and this plugin's handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn anvil_plugin_load(
    host: *const AnvilHost,
    me: *mut AnvilPlugin,
) -> AnvilResult {
    // SAFETY: forwarded from the caller.
    guard(ANVIL_ERROR_INTERNAL, "anvil_plugin_load", || unsafe {
        load(host, me)
    })
}

// The export has the type the header declares.
const _: AnvilPluginLoadFn = anvil_plugin_load;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guard_answers_with_the_fallback_when_the_body_panics() {
        assert_eq!(guard(7, "test", || 3), 3);
        assert_eq!(guard(7, "test", || panic!("on purpose")), 7);
    }

    #[test]
    fn a_failed_load_is_a_result_not_an_unwind() {
        // SAFETY: a null host is refused.
        assert_eq!(
            unsafe { anvil_plugin_load(ptr::null(), ptr::null_mut()) },
            ANVIL_ERROR_INVALID_ARGUMENT
        );
        let panicking = guard(ANVIL_ERROR_INTERNAL, "load", || -> AnvilResult {
            panic!("on purpose")
        });
        assert_eq!(panicking, ANVIL_ERROR_INTERNAL);
    }

    #[test]
    fn symbol_ids_follow_symbols_toml() {
        assert_eq!(sym::SPEED_OBJECT, 0);
        assert_eq!(sym::IDLE_EXIT, 1);
    }
}
