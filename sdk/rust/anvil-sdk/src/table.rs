use std::fmt;
use std::marker::PhantomData;
use std::mem::size_of;
use std::ptr::NonNull;

/// A C table that starts with `uint32_t size`: Anvil's host table or a
/// service's. Its provider may be older than this crate, so its table may be
/// shorter than `T`. `Table` keeps the pointer and the provider's `size` and
/// never makes a `&T` that reaches past them: the crate reads a field only
/// when `size` covers it (the `ANVIL_*_HAS` macros of the C headers), and
/// [`get`](Table::get) hands out `&T` only for a table at least as long as `T`.
pub struct Table<T: 'static> {
    ptr: NonNull<T>,
    size: u32,
}

// SAFETY: a table is immutable and lives for the process (Anvil's ADR 0001:
// nothing is unloaded in API 2); only copies of its fields leave it, and its
// functions may be called from any thread, as the headers say.
unsafe impl<T> Send for Table<T> {}
// SAFETY: as for Send.
unsafe impl<T> Sync for Table<T> {}

impl<T> Clone for Table<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Table<T> {}

impl<T> fmt::Debug for Table<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Table")
            .field("ptr", &self.ptr)
            .field("size", &self.size)
            .finish()
    }
}

impl<T> Table<T> {
    /// The table behind `ptr`; `None` for null. Only its first four bytes,
    /// the `size`, are read here.
    ///
    /// # Safety
    /// `ptr` is null or points to a table of `T`'s layout (or an older,
    /// shorter version of it) whose first field is `u32 size`, with at least
    /// `size` bytes readable, unchanged and valid for the process; `T` is
    /// plain data without interior mutability (a C table of `size`, values
    /// and function pointers), so that sharing it between threads is sound.
    pub unsafe fn from_raw(ptr: *const T) -> Option<Self> {
        let ptr = NonNull::new(ptr.cast_mut())?;
        // SAFETY: the caller's contract: the table starts with its size.
        let size = unsafe { ptr.cast::<u32>().read() };
        Some(Self { ptr, size })
    }

    /// The provider's `size`: how many bytes of `T` its table has.
    pub fn size(&self) -> u32 {
        self.size
    }

    /// The table's address.
    pub fn as_ptr(&self) -> *const T {
        self.ptr.as_ptr()
    }

    /// The whole table as `T`, when the provider's is at least as long as
    /// `T`; `None` from an older provider, whose table is shorter.
    pub fn get(&self) -> Option<&'static T> {
        if (self.size as usize) < size_of::<T>() {
            return None;
        }
        // SAFETY: `size` bytes are readable for the process (from_raw), and
        // they cover all of T.
        Some(unsafe { self.ptr.as_ref() })
    }

    /// The field of type `F` at `offset`, when the provider's `size` covers
    /// it; `None` otherwise. No reference to the table is made.
    ///
    /// # Safety
    /// `offset` and `F` are the offset and type of a field of `T`.
    pub(crate) unsafe fn read_field<F: Copy>(&self, offset: usize, _: PhantomData<F>) -> Option<F> {
        let end = offset.checked_add(size_of::<F>())?;
        if (self.size as usize) < end || end > size_of::<T>() {
            return None;
        }
        // SAFETY: inside the provider's `size` bytes, at a field of T of type F.
        Some(unsafe {
            self.ptr
                .as_ptr()
                .cast::<u8>()
                .add(offset)
                .cast::<F>()
                .read()
        })
    }
}

/// The type of a field, from a closure that is never called: `field!` names
/// it without making a reference to the table.
pub(crate) fn field_type<T, F>(_: fn(&T) -> &F) -> PhantomData<F> {
    PhantomData
}
