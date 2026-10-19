//! Compile test of `anvil gen --lang rust`: each function below has the type
//! the signature in symbols.toml names, so assigning it to the generated alias
//! only compiles when the translation is exactly right (Windows x64 sizes).
#![allow(dead_code, clippy::too_many_arguments, clippy::type_complexity)]

#[allow(dead_code)]
mod sym {
    include!(concat!(env!("OUT_DIR"), "/symbols.gen.rs"));
}

#[allow(dead_code)]
mod cfg {
    include!(concat!(env!("OUT_DIR"), "/settings.gen.rs"));
}

use core::ffi::{
    c_char, c_int, c_long, c_longlong, c_schar, c_short, c_uchar, c_uint, c_ulong, c_ulonglong,
    c_ushort, c_void,
};

extern "C" fn scalars(
    _a: c_int,
    _b: c_uint,
    _c: c_long,
    _d: c_ulong,
    _e: c_longlong,
    _f: c_ulonglong,
    _g: c_short,
    _h: c_ushort,
    _i: c_schar,
    _j: c_uchar,
    _k: c_char,
    _l: f32,
    _m: bool,
) -> f64 {
    0.0
}
const _: sym::ScalarsFn = scalars;

extern "C" fn fixed_width(
    _a: i8,
    _b: i16,
    _c: i32,
    _d: i64,
    _e: u8,
    _f: u16,
    _g: u32,
    _h: u64,
    _i: usize,
    _j: isize,
    _k: usize,
    _l: c_ulong,
    _m: c_ulong,
) -> u64 {
    0
}
const _: sym::FixedWidthFn = fixed_width;

extern "C" fn pointers(
    _text: *const c_char,
    _user: *mut c_void,
    _out: *mut *mut c_void,
    _names: *const *const c_char,
    _bytes: *mut u8,
    _root: *mut c_void,
    _croot: *const c_void,
    _tagged: *mut c_void,
    _fixed: *mut c_uchar,
) -> *const c_char {
    core::ptr::null()
}
const _: sym::PointersFn = pointers;

extern "C" fn no_args() {}
const _: sym::NoArgsFn = no_args;

extern "C" fn void_args() -> bool {
    true
}
const _: sym::VoidArgsFn = void_args;

extern "C" fn unnamed(_: c_int, _: *const c_char, _: f64) -> c_int {
    0
}
const _: sym::UnnamedFn = unnamed;

extern "C" fn keywords(_type: c_int, _fn: c_int, _match: c_int) -> c_int {
    0
}
const _: sym::KeywordsFn = keywords;

// What gen cannot translate becomes *mut c_void (and a warning).
extern "C" fn unknown_value(_r: *mut c_void, _n: c_int) -> *mut c_void {
    core::ptr::null_mut()
}
const _: sym::UnknownValueFn = unknown_value;

extern "C" fn callback(_cb: *mut c_void, _n: c_int, _table: *mut c_void) {}
const _: sym::CallbackFn = callback;

// A function with "..." can be declared, not defined, on stable Rust.
extern "C" {
    fn variadic(format: *const c_char, ...) -> c_int;
}
const _: sym::VariadicFn = variadic;

// Ids, in declaration order, and the values from the verified bytes.
const _: () = {
    assert!(sym::SCALARS == 0);
    assert!(sym::VARIADIC == 9);
    assert!(sym::ROOT_AND_DATE == 10);
    assert!(sym::TICK_SLOT == 11);
    assert!(sym::ROOT_AND_DATE_ROOT == 0x233FE78);
    assert!(sym::ROOT_AND_DATE_DATE_OFFSET == 0x1DD0);
};

// Settings: ids in declaration order, enum option indices and the key flags.
const _: anvil_sys::AnvilSetting = cfg::MODE;
const _: () = {
    assert!(cfg::BUDGET_MB == 0);
    assert!(cfg::MODE == 1);
    assert!(cfg::TOGGLE == 2);
    assert!(cfg::MODE_FAST == 0);
    assert!(cfg::MODE_SLOW == 1);
    assert!(cfg::KEY_SHIFT == 0x100);
    assert!(cfg::KEY_CTRL == 0x200);
    assert!(cfg::KEY_ALT == 0x400);
    assert!(cfg::KEY_VK_MASK == 0xFF);
};
