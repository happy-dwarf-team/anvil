use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard};

use crate::error::{check, Error, ErrorCode, Result};
use crate::fnptr::FnPtr;
use crate::host::Host;
use crate::sys::{AnvilChange, AnvilPlugin};

/// A change's handle: dropping it removes the change; [`keep`](Change::keep)
/// keeps the change for the rest of the process.
///
/// Anvil never frees its own stubs, so a game thread still inside a removed
/// change finishes safely; what the change runs (an adapter of this crate)
/// is never freed either.
#[must_use = "dropping a Change removes the change; keep it in the plugin, or call keep()"]
pub struct Change {
    handle: Option<NonNull<AnvilChange>>,
}

// SAFETY: the handle is an opaque id; Anvil's remove may be called from any thread.
unsafe impl Send for Change {}
// SAFETY: as above; &Change only reads the id.
unsafe impl Sync for Change {}

impl Change {
    pub(crate) fn from_raw(handle: *mut AnvilChange) -> Self {
        Self {
            handle: NonNull::new(handle),
        }
    }

    /// The raw handle, for `AnvilHost::remove`.
    pub fn as_raw(&self) -> *mut AnvilChange {
        self.handle.map_or(ptr::null_mut(), NonNull::as_ptr)
    }

    /// Removes the change now. A change that is already gone (e.g. rolled
    /// back after a failed load) is no error.
    pub fn remove(mut self) -> Result<()> {
        remove_handle(self.handle.take())
    }

    /// Keeps the change for the rest of the process.
    pub fn keep(mut self) {
        self.handle = None;
    }
}

impl Drop for Change {
    fn drop(&mut self) {
        let _ = remove_handle(self.handle.take());
    }
}

impl fmt::Debug for Change {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Change").field(&self.as_raw()).finish()
    }
}

fn remove_handle(handle: Option<NonNull<AnvilChange>>) -> Result<()> {
    let (Some(handle), Some(host)) = (handle, Host::current()) else {
        return Ok(());
    };
    let Some(remove) = field!(host.c_api(), crate::sys::AnvilHost, remove).flatten() else {
        return Ok(());
    };
    // SAFETY: a handle Anvil gave this plugin; Anvil answers NOT_FOUND for one that is gone.
    let result = unsafe { remove(host.raw_plugin(), handle.as_ptr()) };
    match check(result, || "remove") {
        Err(error) if error.code() == ErrorCode::NotFound => Ok(()),
        other => other,
    }
}

/// Takes the change out of `slot` and removes it. A slot another thread is
/// filling (a request on it still runs) is left to that thread.
fn remove_slot(slot: &AtomicPtr<AnvilChange>, what: &str) -> Result<()> {
    loop {
        let current = slot.load(Ordering::Acquire);
        if current == claiming() {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                format!("{what}: another thread is installing a change on it; remove() after that returns"),
            ));
        }
        if slot
            .compare_exchange_weak(
                current,
                ptr::null_mut(),
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
        {
            return remove_handle(NonNull::new(current));
        }
    }
}

/// A slot filled in a group, and the handle (as an address) it got.
type FilledSlot = (&'static AtomicPtr<AnvilChange>, usize);

/// The open [`Host::group`](crate::Host::group)s of the plugin and its
/// modules, by owner handle (as an address), each with the hook and swap
/// slots filled in it and the handles they got (anvil#533). A group is its
/// owner's: a request of that owner from any thread joins it, and each module
/// is an owner of its own. A group that aborts or whose commit Anvil refuses
/// removes those changes, so the slots are emptied again (else `is_active`
/// stays true and the next request on the hook answers "already holds a
/// change").
static OPEN_GROUPS: Mutex<Vec<(usize, Vec<FilledSlot>)>> = Mutex::new(Vec::new());

/// Held shared by a request from its host call until its slot is recorded,
/// and exclusively by `Host::group` around `begin_group` and around the
/// commit or abort, so a request is recorded exactly when Anvil staged it.
static GROUP_GATE: RwLock<()> = RwLock::new(());

/// The shared hold of a request.
pub(crate) fn request_gate() -> RwLockReadGuard<'static, ()> {
    GROUP_GATE.read().unwrap_or_else(|e| e.into_inner())
}

/// The exclusive hold of a group's start and end.
pub(crate) fn group_gate() -> RwLockWriteGuard<'static, ()> {
    GROUP_GATE.write().unwrap_or_else(|e| e.into_inner())
}

fn open_groups() -> std::sync::MutexGuard<'static, Vec<(usize, Vec<FilledSlot>)>> {
    OPEN_GROUPS.lock().unwrap_or_else(|e| e.into_inner())
}

/// `owner` opened a group: slots its requests fill from now on are recorded.
pub(crate) fn group_opened(owner: *mut AnvilPlugin) {
    open_groups().push((owner as usize, Vec::new()));
}

/// `owner`'s group ended. When Anvil removed its changes (`discarded`), each
/// slot still holding the handle it got in the group is emptied; a slot that
/// has moved on (removed, or filled again) is left as it is.
pub(crate) fn group_closed(owner: *mut AnvilPlugin, discarded: bool) {
    let filled = {
        let mut groups = open_groups();
        let Some(at) = groups.iter().position(|(o, _)| *o == owner as usize) else {
            return;
        };
        groups.swap_remove(at).1
    };
    if !discarded {
        return;
    }
    for (slot, handle) in filled {
        let _ = slot.compare_exchange(
            handle as *mut AnvilChange,
            ptr::null_mut(),
            Ordering::AcqRel,
            Ordering::Acquire,
        );
    }
}

/// What a hook's or swap's `change` holds while a request on it runs.
fn claiming() -> *mut AnvilChange {
    NonNull::<AnvilChange>::dangling().as_ptr()
}

/// The `change` slot of a hook or swap, taken for one request with a
/// compare-exchange: two threads requesting a change on one static cannot both
/// get through (the later handle would overwrite the earlier one, which could
/// then never be removed). Dropped without [`finish`](Claim::finish) (the
/// request failed), the slot is empty again.
pub(crate) struct Claim {
    slot: &'static AtomicPtr<AnvilChange>,
}

impl Claim {
    fn take(slot: &'static AtomicPtr<AnvilChange>, what: &str) -> Result<Self> {
        slot.compare_exchange(
            ptr::null_mut(),
            claiming(),
            Ordering::AcqRel,
            Ordering::Acquire,
        )
        .map(|_| Self { slot })
        .map_err(|_| {
            Error::new(
                ErrorCode::InvalidArgument,
                format!("{what} already holds a change; remove() it first"),
            )
        })
    }

    /// The request of `owner` succeeded: the slot holds its handle. Called
    /// under the request's [`request_gate`].
    pub(crate) fn finish(self, change: *mut AnvilChange, owner: *mut AnvilPlugin) {
        self.slot.store(change, Ordering::Release);
        if !change.is_null() {
            let mut groups = open_groups();
            if let Some((_, filled)) = groups.iter_mut().find(|(o, _)| *o == owner as usize) {
                filled.push((self.slot, change as usize));
            }
        }
        std::mem::forget(self);
    }
}

impl Drop for Claim {
    fn drop(&mut self) {
        self.slot.store(ptr::null_mut(), Ordering::Release);
    }
}

/// A function hook, call redirect or import replacement, and the rest of its
/// chain. It lives in a `static`, so the detour (a plain function the game
/// calls) reaches `next`, and Anvil can write `next` into it before the change
/// is enabled (on a hook reused after [`remove`](FunctionHook::remove), only
/// on an Anvil newer than 1.25.0):
///
/// ```ignore
/// static SPEED_OBJECT: anvil::FunctionHook<sym::SpeedObjectFn> = anvil::FunctionHook::new();
///
/// unsafe extern "C" fn on_speed_object(session: *mut c_void) -> *mut c_void {
///     let Some(next) = SPEED_OBJECT.next() else { return std::ptr::null_mut() };
///     // SAFETY: the game's own function, called as it declares.
///     unsafe { next(session) }
/// }
/// ```
///
/// A detour is called by the game directly, with no adapter between: it must
/// not panic (a panic out of an `extern "C"` function aborts the process).
/// Wrap fallible work in [`catch_panic`](crate::catch_panic).
pub struct FunctionHook<F: FnPtr> {
    next: AtomicPtr<c_void>,
    change: AtomicPtr<AnvilChange>,
    _function: PhantomData<F>,
}

impl<F: FnPtr> FunctionHook<F> {
    /// An unused hook.
    pub const fn new() -> Self {
        Self {
            next: AtomicPtr::new(ptr::null_mut()),
            change: AtomicPtr::new(ptr::null_mut()),
            _function: PhantomData,
        }
    }

    /// The rest of the chain: the next plugin's detour or the game's code;
    /// `None` before Anvil wrote it. It stays valid after the change is
    /// removed, for a thread still inside the detour.
    pub fn next(&self) -> Option<F> {
        let raw = self.next.load(Ordering::Acquire);
        // SAFETY: Anvil wrote a function of the declared type (or nothing).
        (!raw.is_null()).then(|| unsafe { F::from_raw(raw) })
    }

    /// Whether the hook holds a change (or another thread is installing one).
    pub fn is_active(&self) -> bool {
        !self.change.load(Ordering::Acquire).is_null()
    }

    /// Removes the change; nothing when there is none. The hook can then take
    /// a new change; until it does, `next` keeps the removed change's chain
    /// for a thread still inside the detour.
    pub fn remove(&self) -> Result<()> {
        remove_slot(&self.change, "the hook object")
    }

    pub(crate) fn claim(&'static self, call: &'static str) -> Result<Claim> {
        Claim::take(&self.change, &format!("{call}: the hook object"))
    }

    /// Where Anvil writes `next` during a request (it writes it before it
    /// enables the change): the hook's own cell, so a detour the game enters
    /// as soon as the change is enabled finds `next`. On a reused hook that
    /// holds to an Anvil that restores the cell when a request fails
    /// (`restores`); an older one writes NULL there, so the reused hook gets
    /// `local` and a failed request leaves the removed change's chain for a
    /// thread still inside its detour.
    pub(crate) fn next_cell(&self, local: &mut *mut c_void, restores: bool) -> *mut *mut c_void {
        if restores || self.next.load(Ordering::Acquire).is_null() {
            self.next.as_ptr()
        } else {
            local
        }
    }

    /// A request that succeeded: Anvil wrote `next` into `cell` during the
    /// call (and never changes it afterwards); a local is copied in only now.
    pub(crate) fn finish(
        &self,
        claim: Claim,
        cell: *mut *mut c_void,
        local: *mut c_void,
        change: *mut AnvilChange,
        owner: *mut AnvilPlugin,
    ) {
        if cell != self.next.as_ptr() {
            self.next.store(local, Ordering::Release);
        }
        claim.finish(change, owner);
    }
}

impl<F: FnPtr> Default for FunctionHook<F> {
    fn default() -> Self {
        Self::new()
    }
}

impl<F: FnPtr> fmt::Debug for FunctionHook<F> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FunctionHook")
            .field("next", &self.next.load(Ordering::Relaxed))
            .field("active", &self.is_active())
            .finish()
    }
}

/// A pointer swap on a slot symbol, and the rest of its chain. Anvil rewrites
/// [`previous`](PointerSwap::previous) whenever the chain changes, so it lives
/// in a `static`.
pub struct PointerSwap {
    previous: AtomicPtr<c_void>,
    change: AtomicPtr<AnvilChange>,
}

impl PointerSwap {
    /// An unused swap.
    pub const fn new() -> Self {
        Self {
            previous: AtomicPtr::new(ptr::null_mut()),
            change: AtomicPtr::new(ptr::null_mut()),
        }
    }

    /// The rest of the chain, read on each use: another plugin's value or the
    /// slot's original pointer.
    pub fn previous(&self) -> *mut c_void {
        self.previous.load(Ordering::Acquire)
    }

    /// Whether the swap holds a change.
    pub fn is_active(&self) -> bool {
        !self.change.load(Ordering::Acquire).is_null()
    }

    /// Removes the change; nothing when there is none.
    pub fn remove(&self) -> Result<()> {
        remove_slot(&self.change, "the swap object")
    }

    pub(crate) fn claim(&'static self) -> Result<Claim> {
        Claim::take(&self.change, "swap_pointer: the swap object")
    }

    pub(crate) fn previous_slot(&self) -> *mut *mut c_void {
        self.previous.as_ptr()
    }
}

impl Default for PointerSwap {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Debug for PointerSwap {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PointerSwap")
            .field("previous", &self.previous())
            .field("active", &self.is_active())
            .finish()
    }
}
