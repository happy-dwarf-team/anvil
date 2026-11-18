use std::ffi::c_void;
use std::marker::PhantomData;

use crate::sys::AnvilSymbol;

/// A function pointer type the game can call: `unsafe extern "C" fn(…) -> R`
/// with up to 12 arguments, as `anvil gen --lang rust` writes them
/// (`sym::<Name>Fn`).
///
/// Only `unsafe` function pointers implement it, so calling a hook's
/// [`next`](crate::FunctionHook::next) or a game function is always an
/// `unsafe` call. Variadic functions are not covered.
///
/// # Safety
/// An implementation must be a function pointer type, so that the raw
/// conversions keep its value.
pub unsafe trait FnPtr: Copy + Send + Sync + 'static {
    /// The pointer as Anvil takes it.
    fn into_raw(self) -> *mut c_void;

    /// The function pointer back from Anvil's pointer.
    ///
    /// # Safety
    /// `raw` is not null and points to a function of this type.
    unsafe fn from_raw(raw: *mut c_void) -> Self;
}

macro_rules! fn_ptr {
    ($($arg:ident),*) => {
        // SAFETY: a function pointer type.
        unsafe impl<R: 'static, $($arg: 'static),*> FnPtr for unsafe extern "C" fn($($arg),*) -> R {
            fn into_raw(self) -> *mut c_void {
                self as *const () as *mut c_void
            }

            unsafe fn from_raw(raw: *mut c_void) -> Self {
                // SAFETY: the caller's contract; both are one pointer wide.
                unsafe { std::mem::transmute_copy::<*mut c_void, Self>(&raw) }
            }
        }
    };
}

fn_ptr!();
fn_ptr!(A);
fn_ptr!(A, B);
fn_ptr!(A, B, C);
fn_ptr!(A, B, C, D);
fn_ptr!(A, B, C, D, E);
fn_ptr!(A, B, C, D, E, F);
fn_ptr!(A, B, C, D, E, F, G);
fn_ptr!(A, B, C, D, E, F, G, H);
fn_ptr!(A, B, C, D, E, F, G, H, I);
fn_ptr!(A, B, C, D, E, F, G, H, I, J);
fn_ptr!(A, B, C, D, E, F, G, H, I, J, K);
fn_ptr!(A, B, C, D, E, F, G, H, I, J, K, L);

/// A function symbol together with its declared type: what
/// `anvil gen --lang rust-sdk` writes as `sym::typed::<NAME>` for each
/// function symbol of `symbols.toml`, from the same signature as
/// `sym::<Name>Fn`. [`Host::hook_symbol`](crate::Host::hook_symbol) takes it
/// with a hook and a detour of that type, so a detour of another shape does
/// not compile and the call needs no `unsafe`.
///
/// ```no_run
/// use std::ffi::c_void;
/// use anvil::{FnSymbol, FunctionHook, Host};
///
/// mod sym {
///     // What anvil gen --lang rust-sdk writes for `void* (void* session)`.
///     pub type SpeedObjectFn = unsafe extern "C" fn(session: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
///     pub const SPEED_OBJECT: u32 = 0;
///     pub mod typed {
///         // SAFETY: the type of the signature above.
///         pub const SPEED_OBJECT: anvil::FnSymbol<super::SpeedObjectFn> =
///             unsafe { anvil::FnSymbol::new(super::SPEED_OBJECT) };
///     }
/// }
///
/// static HOOK: FunctionHook<sym::SpeedObjectFn> = FunctionHook::new();
///
/// unsafe extern "C" fn detour(session: *mut c_void) -> *mut c_void {
///     // SAFETY: the game's function, with the game's argument.
///     HOOK.next().map_or(std::ptr::null_mut(), |next| unsafe { next(session) })
/// }
///
/// fn load(host: &Host) -> anvil::Result<()> {
///     host.hook_symbol(&HOOK, sym::typed::SPEED_OBJECT, detour)
/// }
/// ```
///
/// A detour of another type is a compile error:
///
/// ```compile_fail
/// use std::ffi::c_void;
/// use anvil::{FnSymbol, FunctionHook, Host};
///
/// type SpeedObjectFn = unsafe extern "C" fn(session: *mut c_void) -> *mut c_void;
/// // SAFETY: the type of the symbol's signature.
/// const SPEED_OBJECT: FnSymbol<SpeedObjectFn> = unsafe { FnSymbol::new(0) };
/// static HOOK: FunctionHook<SpeedObjectFn> = FunctionHook::new();
///
/// unsafe extern "C" fn detour(_speed: i32) -> i32 {
///     0
/// }
///
/// fn load(host: &Host) -> anvil::Result<()> {
///     host.hook_symbol(&HOOK, SPEED_OBJECT, detour)
/// }
/// ```
pub struct FnSymbol<F: FnPtr> {
    id: AnvilSymbol,
    function: PhantomData<F>,
}

impl<F: FnPtr> FnSymbol<F> {
    /// The symbol `id` as a function of type `F`.
    ///
    /// # Safety
    /// `id` is a function symbol of this plugin's `symbols.toml` whose declared
    /// signature is exactly `F` (as `anvil gen` translates it): the game calls
    /// a detour of `F` with the arguments of that signature.
    pub const unsafe fn new(id: AnvilSymbol) -> Self {
        FnSymbol {
            id,
            function: PhantomData,
        }
    }

    /// The symbol's id, `sym::<NAME>`.
    pub const fn id(self) -> AnvilSymbol {
        self.id
    }
}

impl<F: FnPtr> Clone for FnSymbol<F> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<F: FnPtr> Copy for FnSymbol<F> {}

impl<F: FnPtr> std::fmt::Debug for FnSymbol<F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("FnSymbol").field(&self.id).finish()
    }
}
