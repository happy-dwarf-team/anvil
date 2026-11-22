//! The crate against a fake host table: what reaches Anvil, what a panic in
//! an adapter does, and what `Drop` removes.
#![allow(clippy::undocumented_unsafe_blocks)]

use std::ffi::{c_char, c_void, CStr};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use crate::sys::*;
use crate::{
    camera, commands, game, input, log_throttle, map, panel, script, world, Change, ErrorCode,
    FunctionHook, Host, Payload, Plugin, PointerSwap, Tag,
};

/// One test at a time: the host is global to the crate, as in a plugin DLL.
static SERIAL: Mutex<()> = Mutex::new(());

#[derive(Default)]
struct Recorded {
    log: Vec<String>,
    removed: Vec<usize>,
    mid_hooks: Vec<(usize, usize)>, // (callback, user)
    subscriptions: Vec<(String, usize, usize)>,
    rows: Vec<String>,
    clicks: Vec<(usize, usize)>,
    map_infos: Vec<String>,
    describes: Vec<(usize, usize)>,
    colors: Vec<u32>,
    bindings: Vec<(i64, u32, usize, usize)>, // code, flags, handler, context
    day_ends: Vec<(usize, usize, usize)>,    // (self, callback, context)
    effects: Vec<(usize, String, usize, usize, usize)>, // (self, key, execute, describe, context)
    commands: Vec<String>,
}

static RECORDED: Mutex<Option<Recorded>> = Mutex::new(None);
static NEXT_CHANGE: AtomicUsize = AtomicUsize::new(0x1000);

fn with<R>(f: impl FnOnce(&mut Recorded) -> R) -> R {
    f(RECORDED
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get_or_insert_with(Recorded::default))
}

fn text(p: *const c_char) -> String {
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

unsafe extern "C" fn fake_log(_: *mut AnvilPlugin, _: AnvilLogLevel, message: *const c_char) {
    let line = text(message);
    with(|r| r.log.push(line));
}

unsafe extern "C" fn fake_original(x: i32) -> i32 {
    x + 1
}

unsafe extern "C" fn fake_hook_function(
    _: *mut AnvilPlugin,
    symbol: AnvilSymbol,
    _detour: *mut c_void,
    next: *mut *mut c_void,
    out: *mut *mut AnvilChange,
) -> AnvilResult {
    if symbol == 99 {
        return ANVIL_ERROR_NOT_DECLARED;
    }
    let original: unsafe extern "C" fn(i32) -> i32 = fake_original;
    let other: unsafe extern "C" fn(i32) -> i32 = fake_other;
    if symbol == 98 {
        // As the core does when enabling fails: `next` was written, then put
        // back (Anvil after 1.25.0) or cleared (1.25.0 and older).
        unsafe {
            let before = *next;
            *next = other as *mut c_void;
            if let Some(during) = *DURING.lock().unwrap_or_else(|e| e.into_inner()) {
                during();
            }
            *next = if OLD_CORE.load(Ordering::Relaxed) {
                ptr::null_mut()
            } else {
                before
            };
        }
        return ANVIL_ERROR_CONFLICT;
    }
    unsafe {
        *next = if symbol == 97 { other } else { original } as *mut c_void;
        // A game thread may enter the detour once the core enabled the change,
        // before the request returns.
        if let Some(during) = *DURING.lock().unwrap_or_else(|e| e.into_inner()) {
            during();
        }
        *out = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed) as *mut AnvilChange;
    }
    ANVIL_OK
}

unsafe extern "C" fn fake_other(x: i32) -> i32 {
    x + 2
}

/// The fake core of Anvil 1.25.0 and older: a failed request writes NULL
/// into a function hook's `next`.
static OLD_CORE: AtomicBool = AtomicBool::new(false);
/// Runs inside a fake hook request, after `next` was written.
static DURING: Mutex<Option<fn()>> = Mutex::new(None);

unsafe extern "C" fn fake_mid_hook(
    _: *mut AnvilPlugin,
    _site: AnvilSymbol,
    callback: AnvilCpuCallback,
    user: *mut c_void,
    out: *mut *mut AnvilChange,
) -> AnvilResult {
    let change = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed);
    with(|r| {
        r.mid_hooks
            .push((callback.map_or(0, |f| f as usize), user as usize))
    });
    unsafe { *out = change as *mut AnvilChange };
    ANVIL_OK
}

unsafe extern "C" fn fake_remove(_: *mut AnvilPlugin, change: *mut AnvilChange) -> AnvilResult {
    with(|r| r.removed.push(change as usize));
    ANVIL_OK
}

unsafe extern "C" fn fake_subscribe(
    _: *mut AnvilPlugin,
    event: *const c_char,
    callback: AnvilEventCallback,
    context: *mut c_void,
) -> AnvilResult {
    let name = text(event);
    with(|r| {
        r.subscriptions
            .push((name, callback.map_or(0, |f| f as usize), context as usize))
    });
    ANVIL_OK
}

unsafe extern "C" fn fake_publish(
    _: *mut AnvilPlugin,
    _: *const c_char,
    _: *const c_void,
) -> AnvilResult {
    ANVIL_OK
}

unsafe extern "C" fn fake_get_service(
    _: *mut AnvilPlugin,
    name: *const c_char,
    version: u32,
) -> *const c_void {
    match (text(name).as_str(), version) {
        ("anvil-overlay.panel", 1) => ptr::from_ref(&*FAKE_PANEL).cast(),
        ("anvil-game.world", 1) if FULL_WORLD.load(Ordering::Relaxed) => {
            ptr::from_ref(&*FAKE_WORLD_FULL).cast()
        }
        ("anvil-game.world", 1) => ptr::from_ref(&*FAKE_WORLD).cast(),
        ("anvil-game.game", 1) if WITH_GAME.load(Ordering::Relaxed) => {
            ptr::from_ref(&*FAKE_GAME).cast()
        }
        ("anvil-map.modes", 1) => ptr::from_ref(&*FAKE_MAP).cast(),
        ("anvil-overlay.input", 1) => ptr::from_ref(&*FAKE_INPUT).cast(),
        ("anvil-script.day_end", 1) => ptr::from_ref(&*FAKE_DAY_END).cast(),
        ("anvil-script.effects", 1) => ptr::from_ref(&*FAKE_EFFECTS).cast(),
        ("anvil-game.commands", 1..=3) => ptr::from_ref(&*FAKE_COMMANDS).cast(),
        ("anvil-log-throttle.messages", 1) => ptr::from_ref(&*FAKE_LOG_MESSAGES).cast(),
        ("anvil-game.camera", 1) => ptr::from_ref(&*FAKE_CAMERA).cast(),
        _ => ptr::null(),
    }
}

// --- a fake input service ------------------------------------------------------

unsafe extern "C" fn input_bind(
    code: i64,
    flags: u32,
    handler: AnvilInputHandler,
    context: *mut c_void,
    out: *mut *mut AnvilInputBinding,
) -> AnvilResult {
    if code == 0x77 {
        // As the overlay does: the inert binding, with the conflict.
        unsafe { *out = 0x98 as *mut AnvilInputBinding };
        return ANVIL_ERROR_CONFLICT;
    }
    let handler = handler.map_or(0, |f| f as usize);
    with(|r| r.bindings.push((code, flags, handler, context as usize)));
    unsafe { *out = 0x99 as *mut AnvilInputBinding };
    ANVIL_OK
}

unsafe extern "C" fn input_held(code: i64) -> core::ffi::c_int {
    core::ffi::c_int::from(code == 0x241)
}

static FAKE_INPUT: Shared<AnvilInputService> = Shared({
    // SAFETY: a table of Option<fn>: all zero is all None.
    let mut table: AnvilInputService = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilInputService>() as u32;
    table.bind = Some(input_bind);
    table.held = Some(input_held);
    table
});

// --- a fake commands service ----------------------------------------------------

// "<self> <type> <flags> <id>/<tag>/<kind> ..."; a count of 0 is refused, and a
// drill start skips one.
unsafe extern "C" fn commands_post(
    plugin: *mut AnvilPlugin,
    command: *const AnvilGameCommand,
    out: *mut AnvilGameCommandResult,
) -> AnvilResult {
    let c = unsafe { &*command };
    if c.type_ == ANVIL_GAME_COMMAND_EXPLORE
        || c.type_ == ANVIL_GAME_COMMAND_MOVE
        || c.type_ == ANVIL_GAME_COMMAND_SIEGE_AREA
        || c.type_ == ANVIL_GAME_COMMAND_SUPPRESS_AREA
    {
        let line = format!(
            "{:x} {} {} explore {} province {} units {} size {}",
            plugin as usize, c.type_, c.flags, c.explore, c.province, c.count, c.size
        );
        with(|r| r.commands.push(line));
        let out = unsafe { &mut *out };
        out.posted = c.count;
        return ANVIL_OK;
    }
    if c.type_ == ANVIL_GAME_COMMAND_SPEED {
        let line = format!("{:x} speed {} size {}", plugin as usize, c.speed, c.size);
        with(|r| r.commands.push(line));
        let out = unsafe { &mut *out };
        out.posted = 2;
        return ANVIL_OK;
    }
    if c.type_ == ANVIL_GAME_COMMAND_EVENT_OPTION {
        let line = format!(
            "{:x} event {} {} size {}",
            plugin as usize, c.pending_event_id, c.option_index, c.size
        );
        with(|r| r.commands.push(line));
        let out = unsafe { &mut *out };
        out.posted = 1;
        return ANVIL_OK;
    }
    if (c.size as usize) < std::mem::offset_of!(AnvilGameCommand, pending_event_id) || c.count == 0
    {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    let units = unsafe { std::slice::from_raw_parts(c.units, c.count as usize) };
    let mut line = format!("{:x} {} {}", plugin as usize, c.type_, c.flags);
    for u in units {
        let tag: String = u.country.text[..3]
            .iter()
            .map(|&b| b as u8 as char)
            .collect();
        line += &format!(" {:x}/{tag}/{}", u.id, u.kind);
    }
    with(|r| r.commands.push(line));
    let out = unsafe { &mut *out };
    let skipped =
        u32::from(c.type_ == ANVIL_GAME_COMMAND_DRILL && c.flags == ANVIL_GAME_COMMAND_START);
    out.posted = c.count - skipped;
    out.skipped = skipped;
    out.refused = 0;
    ANVIL_OK
}

static FAKE_COMMANDS: Shared<AnvilGameCommandsService> = Shared(AnvilGameCommandsService {
    size: std::mem::size_of::<AnvilGameCommandsService>() as u32,
    post: Some(commands_post),
});

// --- a fake anvil-log-throttle.messages ------------------------------------------

static LOG_KEPT: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn log_keep() -> AnvilResult {
    LOG_KEPT.store(1, Ordering::SeqCst);
    ANVIL_OK
}

unsafe extern "C" fn log_totals(out: *mut AnvilLogTotals) -> AnvilResult {
    let out = unsafe { &mut *out };
    if out.size < std::mem::size_of::<AnvilLogTotals>() as u32 {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    out.keeping = LOG_KEPT.load(Ordering::SeqCst);
    out.version = 2500;
    out.messages = 2500;
    out.written = 2600;
    out.counted = 7;
    ANVIL_OK
}

// 2500 messages: message i has version i + 1, i written lines and, for the
// first, 7 counted ones.
unsafe extern "C" fn log_read(
    first: u64,
    changed_after: u64,
    out: *mut AnvilLogMessage,
    capacity: u32,
    message_size: u32,
    count: *mut u32,
    next: *mut u64,
) -> AnvilResult {
    if message_size < std::mem::size_of::<AnvilLogMessage>() as u32 {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    let mut n = 0u32;
    let mut i = first;
    while i < 2500 && n < capacity {
        if i + 1 > changed_after {
            let m = unsafe { &mut *out.add(n as usize) };
            m.index = i;
            m.version = i + 1;
            m.written = i;
            m.counted = if i == 0 { 7 } else { 0 };
            m.log = c"error.log".as_ptr();
            m.source = c"gamestate.cpp".as_ptr();
            m.source_line = 7653;
            m.text = c"Undefined event target: destination_province".as_ptr();
            n += 1;
        }
        i += 1;
    }
    unsafe {
        *count = n;
        *next = i;
    }
    ANVIL_OK
}

static FAKE_LOG_MESSAGES: Shared<AnvilLogMessagesService> = Shared(AnvilLogMessagesService {
    size: std::mem::size_of::<AnvilLogMessagesService>() as u32,
    keep: Some(log_keep),
    totals: Some(log_totals),
    read: Some(log_read),
});

// --- a fake camera service ------------------------------------------------------

unsafe extern "C" fn camera_view(out: *mut AnvilGameCameraView) -> AnvilResult {
    let out = unsafe { &mut *out };
    assert_eq!(
        out.size as usize,
        std::mem::size_of::<AnvilGameCameraView>()
    );
    out.position = [10.0, 300.0, 20.0];
    out.look_at = [10.0, 0.0, 40.0];
    out.flags = ANVIL_GAME_CAMERA_LOCKED;
    ANVIL_OK
}

unsafe extern "C" fn camera_pan(right: f32, up: f32) -> AnvilResult {
    if right == 1.0 && up == -0.5 {
        ANVIL_OK
    } else {
        ANVIL_ERROR_WRONG_PHASE
    }
}

static FAKE_CAMERA: Shared<AnvilGameCameraService> = Shared(AnvilGameCameraService {
    size: std::mem::size_of::<AnvilGameCameraService>() as u32,
    view: Some(camera_view),
    pan: Some(camera_pan),
});

// --- a fake panel service -------------------------------------------------------

unsafe extern "C" fn panel_create(_: *const c_char, out: *mut *mut AnvilPanel) -> AnvilResult {
    unsafe { *out = 0x77 as *mut AnvilPanel };
    ANVIL_OK
}

unsafe extern "C" fn panel_set_rows(
    _: *mut AnvilPanel,
    rows: *const AnvilPanelRow,
    count: u32,
    size: u32,
) -> AnvilResult {
    assert_eq!(size as usize, std::mem::size_of::<AnvilPanelRow>());
    let rows = unsafe { std::slice::from_raw_parts(rows, count as usize) };
    let lines: Vec<String> = rows
        .iter()
        .map(|row| {
            let label = if row.label.is_null() {
                String::new()
            } else {
                text(row.label)
            };
            let value = if row.value.is_null() {
                String::new()
            } else {
                format!("={}", text(row.value))
            };
            format!("{}:{label}{value}:{}", row.kind, row.id)
        })
        .collect();
    with(|r| r.rows = lines);
    ANVIL_OK
}

unsafe extern "C" fn panel_on_click(
    _: *mut AnvilPanel,
    click: AnvilPanelClick,
    context: *mut c_void,
) -> AnvilResult {
    with(|r| {
        r.clicks
            .push((click.map_or(0, |f| f as usize), context as usize))
    });
    ANVIL_OK
}

unsafe extern "C" fn panel_show(_: *mut AnvilPanel, _: core::ffi::c_int) -> AnvilResult {
    ANVIL_OK
}

struct Shared<T>(T);
// SAFETY: test tables of function pointers, never written.
unsafe impl<T> Sync for Shared<T> {}
impl<T> std::ops::Deref for Shared<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

static FAKE_PANEL: Shared<AnvilPanelService> = Shared(AnvilPanelService {
    size: std::mem::size_of::<AnvilPanelService>() as u32,
    create: Some(panel_create),
    set_rows: Some(panel_set_rows),
    on_click: Some(panel_on_click),
    show: Some(panel_show),
    shown: None,
    language: None,
});

// --- a fake map modes service: one mode, 0x88 --------------------------------------

const MAP_MODE: *const AnvilMapMode = 0x88 as *const AnvilMapMode;

unsafe extern "C" fn map_register(
    info: *const AnvilMapModeInfo,
    out: *mut *const AnvilMapMode,
) -> AnvilResult {
    let info = unsafe { &*info };
    assert_eq!(info.size as usize, std::mem::size_of::<AnvilMapModeInfo>());
    let optional = |p: *const c_char| if p.is_null() { String::new() } else { text(p) };
    let id = text(info.id);
    if id == "p.taken" {
        return ANVIL_ERROR_ALREADY_EXISTS;
    }
    with(|r| {
        r.map_infos.push(format!(
            "{}:{id}:{}:{}:{}",
            info.tab,
            text(info.label),
            optional(info.description),
            optional(info.sprite)
        ));
        if let Some(describe) = info.describe {
            r.describes.push((describe as usize, info.context as usize));
        }
    });
    unsafe { *out = MAP_MODE };
    ANVIL_OK
}

unsafe extern "C" fn map_set_colors(
    mode: *const AnvilMapMode,
    colors: *const u32,
    count: u32,
) -> AnvilResult {
    assert_eq!(mode, MAP_MODE);
    let colors = unsafe { std::slice::from_raw_parts(colors, count as usize) };
    with(|r| r.colors = colors.to_vec());
    ANVIL_OK
}

unsafe extern "C" fn map_mode_id(number: i32) -> *const c_char {
    if number == 80 {
        c"p.cores".as_ptr()
    } else {
        ptr::null()
    }
}

unsafe extern "C" fn map_shown() -> *const AnvilMapMode {
    MAP_MODE
}

unsafe extern "C" fn map_id(mode: *const AnvilMapMode) -> *const c_char {
    if mode == MAP_MODE {
        c"p.cores".as_ptr()
    } else {
        ptr::null()
    }
}

static MAP_SHOWS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

unsafe extern "C" fn map_show(mode: *const AnvilMapMode) -> AnvilResult {
    if mode != MAP_MODE {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    MAP_SHOWS.fetch_add(1, Ordering::Relaxed);
    ANVIL_OK
}

static FAKE_MAP: Shared<AnvilMapModesService> = Shared(AnvilMapModesService {
    size: std::mem::size_of::<AnvilMapModesService>() as u32,
    register_mode: Some(map_register),
    set_colors: Some(map_set_colors),
    mode_id: Some(map_mode_id),
    shown: Some(map_shown),
    id: Some(map_id),
    show: Some(map_show),
});

// --- a fake day end service: refuses after two subscriptions ----------------------

unsafe extern "C" fn day_end_subscribe(
    plugin: *mut AnvilPlugin,
    callback: AnvilScriptDayEnd,
    context: *mut c_void,
) -> AnvilResult {
    with(|r| {
        if r.day_ends.len() == 2 {
            return ANVIL_ERROR_WRONG_PHASE;
        }
        r.day_ends.push((
            plugin as usize,
            callback.map_or(0, |f| f as usize),
            context as usize,
        ));
        ANVIL_OK
    })
}

static FAKE_DAY_END: Shared<AnvilScriptDayEndService> = Shared(AnvilScriptDayEndService {
    size: std::mem::size_of::<AnvilScriptDayEndService>() as u32,
    subscribe: Some(day_end_subscribe),
});

// --- a fake effects service: copies the key, refuses one that is taken -----------------

unsafe extern "C" fn effects_register(
    plugin: *mut AnvilPlugin,
    effect: *const AnvilScriptEffect,
) -> AnvilResult {
    let effect = unsafe { &*effect };
    let key = text(effect.key);
    with(|r| {
        if r.effects.iter().any(|e| e.1 == key) {
            return ANVIL_ERROR_ALREADY_EXISTS;
        }
        r.effects.push((
            plugin as usize,
            key,
            effect.execute.map_or(0, |f| f as usize),
            effect.describe.map_or(0, |f| f as usize),
            effect.context as usize,
        ));
        ANVIL_OK
    })
}

static FAKE_EFFECTS: Shared<AnvilScriptEffectsService> = Shared(AnvilScriptEffectsService {
    size: std::mem::size_of::<AnvilScriptEffectsService>() as u32,
    register_effect: Some(effects_register),
});

// --- a fake world: a province name that grows between the calls, a list ------------

static NAME_CALLS: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn world_province_name(
    id: i32,
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    if id != 295 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    // The first answer says 3 bytes, then the name is longer: the crate asks again.
    let name: &[u8] = if NAME_CALLS.fetch_add(1, Ordering::Relaxed) == 0 {
        b"Mos"
    } else {
        b"Moskva"
    };
    unsafe {
        *length = name.len() as u32;
        let fits = name.len().min(capacity.saturating_sub(1) as usize);
        if capacity > 0 {
            ptr::copy_nonoverlapping(name.as_ptr(), utf8.cast::<u8>(), fits);
            *utf8.add(fits) = 0;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_provinces(
    tag: *const c_char,
    ids: *mut i32,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    if text(tag) != "RUS" {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let all = [295, 296, 310];
    unsafe {
        *count = all.len() as u32;
        for (i, id) in all.iter().take(capacity as usize).enumerate() {
            *ids.add(i) = *id;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country(tag: *const c_char, out: *mut AnvilGameCountry) -> AnvilResult {
    let size = unsafe { (*out).size };
    assert_eq!(size as usize, std::mem::size_of::<AnvilGameCountry>());
    let mut c: AnvilGameCountry = unsafe { std::mem::zeroed() };
    c.size = size;
    c.tag = Tag::new(&text(tag)).unwrap().to_raw();
    c.capital = 295;
    c.province_count = 3;
    c.values = ANVIL_GAME_VALUE_TREASURY;
    c.treasury = 123_450;
    c.stability = 1000; // without its bit: not known on this build
    unsafe { *out = c };
    ANVIL_OK
}

static FAKE_WORLD: Shared<AnvilGameWorldService> = Shared({
    // SAFETY: a table of Option<fn>: all zero is all None.
    let mut table: AnvilGameWorldService = unsafe { std::mem::zeroed() };
    // Only up to war_side: the 1.15 calls are not there.
    table.size = (std::mem::offset_of!(AnvilGameWorldService, war_side) + 8) as u32;
    table.province_name = Some(world_province_name);
    table.country_provinces = Some(world_country_provinces);
    table.country = Some(world_country);
    table
});

// --- a fake world of anvil-game 1.20: a ruler, flags, variable names -----------

static FULL_WORLD: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

// --- a fake anvil-game.game: speed 3, paused by the player ------------------------

static WITH_GAME: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

unsafe extern "C" fn game_speed(out: *mut i32) -> core::ffi::c_int {
    unsafe { *out = 3 };
    1
}

unsafe extern "C" fn game_paused(flags: *mut u32) -> core::ffi::c_int {
    unsafe { *flags = ANVIL_GAME_PAUSE_PLAYER };
    1
}

unsafe extern "C" fn game_user_dir() -> *const u16 {
    static DIR: [u16; 9] = [
        b'C' as u16,
        b':' as u16,
        b'\\' as u16,
        b'u' as u16,
        b's' as u16,
        b'e' as u16,
        b'r' as u16,
        0x00EB,
        0,
    ];
    DIR.as_ptr()
}

static FAKE_GAME: Shared<AnvilGameService> = Shared({
    let mut table: AnvilGameService = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilGameService>() as u32;
    table.speed = Some(game_speed);
    table.paused = Some(game_paused);
    table.user_dir = Some(game_user_dir);
    table
});

unsafe extern "C" fn world_monarch(
    tag: *const c_char,
    kind: u32,
    out: *mut AnvilGameMonarch,
) -> AnvilResult {
    if text(tag) != "RUS" || kind == ANVIL_GAME_MONARCH_HEIR {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let size = unsafe { (*out).size };
    assert_eq!(size as usize, std::mem::size_of::<AnvilGameMonarch>());
    let mut m: AnvilGameMonarch = unsafe { std::mem::zeroed() };
    m.size = size;
    m.kind = kind;
    m.id = 0x1234_0000_0030;
    m.country = Tag::new("RUS").unwrap().to_raw();
    m.flags = ANVIL_GAME_MONARCH_FEMALE;
    m.adm = 6;
    m.dip = 4;
    m.mil = 5;
    m.birth.year = 1672;
    m.birth.month = 6;
    m.birth.day = 9;
    m.birth.session = 1;
    unsafe { *out = m };
    ANVIL_OK
}

unsafe extern "C" fn world_global_flags(
    out: *mut AnvilGameFlag,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    unsafe {
        *count = 1;
        if capacity > 0 {
            let mut f: AnvilGameFlag = std::mem::zeroed();
            f.id = 2;
            f.date.year = 1700;
            f.date.month = 8;
            f.date.day = 19;
            *out = f;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_variable_names(
    _tag: *const c_char,
    names: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    let all = b"treasury_plan\0war_goal\0";
    unsafe {
        *length = all.len() as u32;
        if capacity as usize >= all.len() {
            ptr::copy_nonoverlapping(all.as_ptr(), names.cast::<u8>(), all.len());
        }
    }
    ANVIL_OK
}

// --- and of anvil-game 1.21: pending events and statistics -------------------------

unsafe extern "C" fn world_pending_events(
    tag: *const c_char,
    ids: *mut i32,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    // The player's own country (null) has 5 and 8; TUR has 7.
    let list: &[i32] = if tag.is_null() {
        &[5, 8]
    } else if text(tag) == "TUR" {
        &[7]
    } else {
        return ANVIL_ERROR_NOT_FOUND;
    };
    unsafe {
        *count = list.len() as u32;
        ptr::copy_nonoverlapping(list.as_ptr(), ids, list.len().min(capacity as usize));
    }
    ANVIL_OK
}

unsafe extern "C" fn world_pending_event(
    pending_id: i32,
    out: *mut AnvilGamePendingEvent,
) -> AnvilResult {
    if pending_id != 7 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let size = unsafe { (*out).size };
    assert_eq!(size as usize, std::mem::size_of::<AnvilGamePendingEvent>());
    let mut e: AnvilGamePendingEvent = unsafe { std::mem::zeroed() };
    e.size = size;
    e.pending_id = 7;
    e.country = Tag::new("TUR").unwrap().to_raw();
    e.province = 151;
    e.options = 2;
    e.flags = ANVIL_GAME_PENDING_HIDDEN;
    e.auto_select.year = 1719;
    e.auto_select.month = 7;
    e.auto_select.day = 17;
    unsafe { *out = e };
    ANVIL_OK
}

unsafe extern "C" fn world_pending_event_key(
    pending_id: i32,
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    if pending_id != 7 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let key = b"flavor_tur.203\0";
    unsafe {
        *length = (key.len() - 1) as u32;
        if capacity as usize >= key.len() {
            ptr::copy_nonoverlapping(key.as_ptr(), utf8.cast::<u8>(), key.len());
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_statistics(
    tag: *const c_char,
    kind: u32,
    out: *mut AnvilGameStatisticPoint,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    if text(tag) != "RUS" || kind != ANVIL_GAME_STATISTIC_INCOME {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let points = [
        AnvilGameStatisticPoint {
            year: 1717,
            value: 0,
        },
        AnvilGameStatisticPoint {
            year: 1718,
            value: 1584,
        },
    ];
    unsafe {
        *count = points.len() as u32;
        ptr::copy_nonoverlapping(points.as_ptr(), out, points.len().min(capacity as usize));
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_timed_modifiers(
    _tag: *const c_char,
    out: *mut AnvilGameTimedModifier,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    unsafe {
        *count = 2;
        if capacity >= 2 {
            let mut a: AnvilGameTimedModifier = std::mem::zeroed();
            a.flags = ANVIL_GAME_TIMED_HIDDEN;
            a.start.year = 1715;
            a.start.month = 1;
            a.start.day = 1;
            a.end.year = 1725;
            a.end.month = 1;
            a.end.day = 1;
            let mut b: AnvilGameTimedModifier = std::mem::zeroed();
            b.flags = ANVIL_GAME_TIMED_RULER | ANVIL_GAME_TIMED_NO_END | ANVIL_GAME_TIMED_NO_START;
            *out = a;
            *out.add(1) = b;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_timed_modifier_keys(
    _tag: *const c_char,
    keys: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    let all = b"obstacle_shipbuilding\0tsars_reforms\0";
    unsafe {
        *length = all.len() as u32;
        if capacity as usize >= all.len() {
            ptr::copy_nonoverlapping(all.as_ptr(), keys.cast::<u8>(), all.len());
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_province_triggered_modifier_keys(
    province: i32,
    keys: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    let all: &[u8] = if province == 1 {
        b"pr_mughal_tomb\0center_of_trade_modifier\0"
    } else {
        b""
    };
    unsafe {
        *length = all.len() as u32;
        if capacity as usize >= all.len() && !all.is_empty() {
            ptr::copy_nonoverlapping(all.as_ptr(), keys.cast::<u8>(), all.len());
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_province_view(province: *mut i32) -> AnvilResult {
    unsafe { *province = 295 };
    ANVIL_OK
}

unsafe extern "C" fn world_selected_province(province: *mut i32) -> AnvilResult {
    unsafe { *province = 1 };
    ANVIL_OK
}

// The fake map: RUS's navy 7 at x = 100, no unit elsewhere.
unsafe extern "C" fn world_unit_at(x: i32, _y: i32, out: *mut AnvilGameUnitRef) -> AnvilResult {
    if x != 100 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    unsafe {
        (*out).id = 7;
        (*out).country = AnvilGameTag {
            text: [b'R' as c_char, b'U' as c_char, b'S' as c_char, 0],
        };
        (*out).kind = ANVIL_GAME_UNIT_NAVY;
    }
    ANVIL_OK
}

// The fake map: the province is x, off the map left of it.
unsafe extern "C" fn world_province_at(x: i32, _y: i32, province: *mut i32) -> AnvilResult {
    unsafe { *province = x.max(0) };
    ANVIL_OK
}

// RUS's alliance with TUR since 1700.1.1, without an end date.
unsafe extern "C" fn world_country_relations(
    tag: *const c_char,
    kind: u32,
    out: *mut AnvilGameRelation,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    if text(tag) != "RUS" {
        return ANVIL_ERROR_NOT_FOUND;
    }
    unsafe {
        *count = u32::from(kind == ANVIL_GAME_RELATION_ALLIANCE);
        if kind == ANVIL_GAME_RELATION_ALLIANCE && capacity >= 1 {
            let mut r: AnvilGameRelation = std::mem::zeroed();
            r.first = Tag::new("RUS").unwrap().to_raw();
            r.second = Tag::new("TUR").unwrap().to_raw();
            r.start.size = std::mem::size_of::<AnvilGameDate>() as u32;
            r.start.year = 1700;
            r.start.month = 1;
            r.start.day = 1;
            *out = r;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_diplomacy_status(
    from: *const c_char,
    to: *const c_char,
    out: *mut AnvilGameDiplomacyStatus,
) -> AnvilResult {
    if text(from) == text(to) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    unsafe {
        assert_eq!(
            (*out).size,
            std::mem::size_of::<AnvilGameDiplomacyStatus>() as u32
        );
        (*out).opinion = -108;
        (*out).timed_sum = -399;
        (*out).trust = 99;
        (*out).favors = 37590;
        (*out).flags = ANVIL_GAME_DIPLOMACY_TRUCE;
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_overlord(
    tag: *const c_char,
    out: *mut AnvilGameTag,
) -> AnvilResult {
    if text(tag) != "TUR" {
        return ANVIL_ERROR_NOT_FOUND;
    }
    unsafe { *out = Tag::new("RUS").unwrap().to_raw() };
    ANVIL_OK
}

unsafe extern "C" fn world_country_subjects(
    _tag: *const c_char,
    out: *mut AnvilGameTag,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    unsafe {
        *count = 1;
        if capacity >= 1 {
            *out = Tag::new("TUR").unwrap().to_raw();
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_player_tags(out: *mut AnvilGamePlayer) -> AnvilResult {
    unsafe {
        assert_eq!((*out).size, std::mem::size_of::<AnvilGamePlayer>() as u32);
        (*out).tag = Tag::new("RUS").unwrap().to_raw();
        (*out).own = Tag::new("O00").unwrap().to_raw();
        (*out).observed = Tag::new("RUS").unwrap().to_raw();
        (*out).flags = ANVIL_GAME_PLAYER_OBSERVER;
    }
    ANVIL_OK
}

// RUS's navy 7 may explore the coast of regions 12 and 4, the sea of region 30.
unsafe extern "C" fn world_exploration_regions(
    fleet: *const AnvilGameUnitRef,
    explore: i32,
    out: *mut i32,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    let fleet = unsafe { &*fleet };
    if fleet.kind != ANVIL_GAME_UNIT_NAVY {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    if fleet.id != 7
        || Tag::from_raw(&fleet.country).map(|t| t.as_str().to_owned()) != Some("RUS".into())
    {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let list: &[i32] = match explore as u32 {
        ANVIL_GAME_EXPLORE_COAST => &[12, 4],
        ANVIL_GAME_EXPLORE_SEA => &[30],
        _ => return ANVIL_ERROR_INVALID_ARGUMENT,
    };
    unsafe {
        *count = list.len() as u32;
        for (i, region) in list.iter().take(capacity as usize).enumerate() {
            *out.add(i) = *region;
        }
    }
    ANVIL_OK
}

// Region 12 alone: its translation and its key.
unsafe fn region_text(
    region: i32,
    text: &[u8],
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    if region != 12 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    unsafe {
        *length = text.len() as u32;
        if capacity > 0 {
            let fits = text.len().min(capacity as usize - 1);
            ptr::copy_nonoverlapping(text.as_ptr().cast(), utf8, fits);
            *utf8.add(fits) = 0;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_region_name(
    region: i32,
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    unsafe { region_text(region, "Östersjön".as_bytes(), utf8, capacity, length) }
}

unsafe extern "C" fn world_region_key(
    region: i32,
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    unsafe { region_text(region, b"baltic_sea_region", utf8, capacity, length) }
}

static FAKE_WORLD_FULL: Shared<AnvilGameWorldService> = Shared({
    let mut table: AnvilGameWorldService = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilGameWorldService>() as u32;
    table.monarch = Some(world_monarch);
    table.global_flags = Some(world_global_flags);
    table.country_variable_names = Some(world_country_variable_names);
    table.pending_events = Some(world_pending_events);
    table.pending_event = Some(world_pending_event);
    table.pending_event_key = Some(world_pending_event_key);
    table.country_statistics = Some(world_country_statistics);
    table.country_timed_modifiers = Some(world_country_timed_modifiers);
    table.country_timed_modifier_keys = Some(world_country_timed_modifier_keys);
    table.province_view = Some(world_province_view);
    table.country_relations = Some(world_country_relations);
    table.diplomacy_status = Some(world_diplomacy_status);
    table.country_overlord = Some(world_country_overlord);
    table.country_subjects = Some(world_country_subjects);
    table.player_tags = Some(world_player_tags);
    table.selected_province = Some(world_selected_province);
    table.province_at = Some(world_province_at);
    table.unit_at = Some(world_unit_at);
    table.exploration_regions = Some(world_exploration_regions);
    table.region_name = Some(world_region_name);
    table.region_key = Some(world_region_key);
    table.province_triggered_modifier_keys = Some(world_province_triggered_modifier_keys);
    table
});

/// A fresh host table and record; the crate's host is this one afterwards.
fn fake_host() -> (MutexGuard<'static, ()>, &'static AnvilHost) {
    let guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    FULL_WORLD.store(false, Ordering::Relaxed);
    OLD_CORE.store(false, Ordering::Relaxed);
    *DURING.lock().unwrap_or_else(|e| e.into_inner()) = None;
    WITH_GAME.store(false, Ordering::Relaxed);
    *RECORDED.lock().unwrap_or_else(|e| e.into_inner()) = Some(Recorded::default());
    let mut table: AnvilHost = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilHost>() as u32;
    table.api_version = ANVIL_API_VERSION;
    table.anvil_version = c"9.9.9".as_ptr();
    table.log = Some(fake_log);
    table.hook_function = Some(fake_hook_function);
    table.mid_hook = Some(fake_mid_hook);
    table.remove = Some(fake_remove);
    table.subscribe = Some(fake_subscribe);
    table.publish = Some(fake_publish);
    table.get_service = Some(fake_get_service);
    (guard, Box::leak(Box::new(table)))
}

const SELF: *mut AnvilPlugin = 0x5E1F as *mut AnvilPlugin;

fn load<P: Plugin>(table: &'static AnvilHost) -> AnvilResult {
    unsafe { crate::__private::load::<P>(table, SELF) }
}

fn logged() -> Vec<String> {
    with(|r| r.log.clone())
}

struct Quiet;
impl Plugin for Quiet {
    fn load(_: &mut Host) -> crate::Result<Self> {
        Ok(Quiet)
    }
}

#[test]
fn load_refuses_a_missing_or_foreign_host() {
    let (_guard, table) = fake_host();
    assert_eq!(
        unsafe { crate::__private::load::<Quiet>(ptr::null(), SELF) },
        ANVIL_ERROR_INVALID_ARGUMENT
    );
    let mut old = *table;
    old.api_version = 1;
    assert_eq!(
        load::<Quiet>(Box::leak(Box::new(old))),
        ANVIL_ERROR_UNSUPPORTED
    );
    let mut short = *table;
    short.size = std::mem::offset_of!(AnvilHost, publish) as u32;
    assert_eq!(
        load::<Quiet>(Box::leak(Box::new(short))),
        ANVIL_ERROR_UNSUPPORTED
    );
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
}

#[test]
fn an_error_from_load_is_its_result_and_a_log_line() {
    struct Refuses;
    impl Plugin for Refuses {
        fn load(host: &mut Host) -> crate::Result<Self> {
            unsafe { host.hook(&HOOK, 99, add_two) }?;
            Ok(Refuses)
        }
    }
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    unsafe extern "C" fn add_two(x: i32) -> i32 {
        x + 2
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Refuses>(table), ANVIL_ERROR_NOT_DECLARED);
    assert_eq!(
        logged(),
        ["load() failed: hook_function symbol 99: not declared in symbols.toml or settings.toml"]
    );
    assert!(!HOOK.is_active());
}

#[test]
fn a_panic_in_load_is_logged_with_its_place_and_does_not_unwind() {
    struct Panics;
    impl Plugin for Panics {
        fn load(_: &mut Host) -> crate::Result<Self> {
            panic!("on purpose")
        }
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Panics>(table), ANVIL_ERROR_INTERNAL);
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("load() panicked: on purpose at "),
        "{log:?}"
    );
    assert!(log[0].contains("tests.rs"), "{log:?}");
}

#[test]
fn a_function_hook_gets_next_and_holds_one_change() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    unsafe extern "C" fn detour(x: i32) -> i32 {
        unsafe { HOOK.next().unwrap()(x) * 10 }
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    assert!(HOOK.next().is_none());
    unsafe { host.hook(&HOOK, 1, detour) }.unwrap();
    assert!(HOOK.is_active());
    assert_eq!(unsafe { detour(4) }, 50);
    let again = unsafe { host.hook(&HOOK, 1, detour) }.unwrap_err();
    assert_eq!(again.code(), ErrorCode::InvalidArgument);
    HOOK.remove().unwrap();
    assert!(!HOOK.is_active());
    assert_eq!(with(|r| r.removed.len()), 1);
}

#[test]
fn a_failed_request_on_a_reused_hook_leaves_next_to_a_thread_still_in_the_old_detour() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    unsafe extern "C" fn detour(x: i32) -> i32 {
        unsafe { HOOK.next().unwrap()(x) * 10 }
    }
    // On Anvil 1.25.0 (the core clears the cell) and on a later one (it puts
    // the cell back).
    for (version, old_core) in [(c"1.25.0", true), (c"1.26.0", false)] {
        let (_guard, table) = fake_host();
        let mut versioned = *table;
        versioned.anvil_version = version.as_ptr();
        OLD_CORE.store(old_core, Ordering::Relaxed);
        assert_eq!(load::<Quiet>(Box::leak(Box::new(versioned))), ANVIL_OK);
        let host = Host::current().unwrap();
        assert_eq!(host.restores_next(), !old_core);
        unsafe { host.hook(&HOOK, 1, detour) }.unwrap();
        HOOK.remove().unwrap();
        // The hook is reused and Anvil refuses the new change, after it wrote `next`.
        let refused = unsafe { host.hook(&HOOK, 98, detour) }.unwrap_err();
        assert_eq!(refused.code(), ErrorCode::Conflict);
        assert!(!HOOK.is_active());
        // A thread still inside the removed change's detour passes on as before.
        assert_eq!(unsafe { detour(4) }, 50, "{version:?}");
        // The hook takes a change again.
        unsafe { host.hook(&HOOK, 1, detour) }.unwrap();
        assert!(HOOK.is_active());
        HOOK.remove().unwrap();
    }
}

#[test]
fn a_reused_hook_has_its_new_next_as_soon_as_the_change_is_enabled() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static SEEN: AtomicUsize = AtomicUsize::new(0);
    fn enter_detour() {
        SEEN.store(HOOK.next().map_or(0, |f| f as usize), Ordering::Relaxed);
    }
    let other: unsafe extern "C" fn(i32) -> i32 = fake_other;
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    unsafe { host.hook(&HOOK, 1, fake_original) }.unwrap();
    HOOK.remove().unwrap();
    // A game thread enters the detour while the request on the reused hook
    // runs: it reads the new change's next, not the removed one's.
    *DURING.lock().unwrap() = Some(enter_detour);
    unsafe { host.hook(&HOOK, 97, fake_original) }.unwrap();
    assert_eq!(SEEN.load(Ordering::Relaxed), other as usize);
    assert_eq!(unsafe { HOOK.next().unwrap()(1) }, 3);
    HOOK.remove().unwrap();
}

#[test]
fn restores_next_needs_an_anvil_after_1_25_0() {
    use crate::host::restores_next;
    for old in [
        "", "1", "1.25", "1.25.0", "1.24.9", "1.9.99", "0.99.0", "x.y.z", "1.25.x",
    ] {
        assert!(!restores_next(old), "{old}");
    }
    for new in [
        "1.25.1",
        "1.26.0",
        "1.26.0-beta.1",
        "2.0.0",
        "1.100.0",
        "9.9.9",
    ] {
        assert!(restores_next(new), "{new}");
    }
}

#[test]
fn a_hook_object_is_claimed_by_one_request_at_a_time() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    // A request on another thread holds the hook while Anvil answers it.
    let claim = HOOK.claim("hook_function").unwrap();
    assert!(HOOK.is_active());
    let second = unsafe { host.hook(&HOOK, 1, fake_original) }.unwrap_err();
    assert_eq!(second.code(), ErrorCode::InvalidArgument);
    assert!(
        second.to_string().contains("already holds a change"),
        "{second}"
    );
    let removed = HOOK.remove().unwrap_err();
    assert!(
        removed.to_string().contains("another thread is installing"),
        "{removed}"
    );
    assert!(with(|r| r.removed.is_empty()));
    // That request fails: the hook is free again.
    drop(claim);
    assert!(!HOOK.is_active());
    unsafe { host.hook(&HOOK, 1, fake_original) }.unwrap();
    HOOK.remove().unwrap();
    assert_eq!(with(|r| r.removed.len()), 1);
}

#[test]
fn dropping_a_change_removes_it_and_keep_keeps_it() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let dropped = host.mid_hook(1, |_| {}).unwrap();
    let id = dropped.as_raw() as usize;
    drop(dropped);
    assert_eq!(with(|r| r.removed.clone()), [id]);
    host.mid_hook(1, |_| {}).unwrap().keep();
    let removed: Change = host.mid_hook(1, |_| {}).unwrap();
    let removed_id = removed.as_raw() as usize;
    removed.remove().unwrap();
    assert_eq!(with(|r| r.removed.clone()), [id, removed_id]);
}

#[test]
fn a_panic_in_a_mid_hook_is_logged_once_and_the_game_carries_on() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    host.mid_hook(5, |cpu| {
        assert!(cpu.rcx != 13, "unlucky {}", cpu.rcx);
        // SAFETY: a test context.
        unsafe { cpu.regs_mut().rax = cpu.rcx + 1 };
    })
    .unwrap()
    .keep();
    let (callback, user) = with(|r| r.mid_hooks[0]);
    let callback: unsafe extern "C" fn(*mut AnvilCpuContext, *mut c_void) =
        unsafe { std::mem::transmute(callback) };
    let mut cpu: AnvilCpuContext = unsafe { std::mem::zeroed() };
    for rcx in [1, 13, 13, 2] {
        cpu.rcx = rcx;
        cpu.rax = 0;
        unsafe { callback(&mut cpu, user as *mut c_void) };
        assert_eq!(cpu.rax, if rcx == 13 { 0 } else { rcx + 1 });
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("mid_hook symbol 5: the callback panicked (unlucky 13 at "),
        "{log:?}"
    );
}

#[test]
fn a_panic_out_of_a_group_is_logged_once_where_it_is_caught() {
    static ABORTS: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn begin(_: *mut AnvilPlugin) -> AnvilResult {
        ANVIL_OK
    }
    unsafe extern "C" fn commit(_: *mut AnvilPlugin) -> AnvilResult {
        ANVIL_OK
    }
    unsafe extern "C" fn abort(_: *mut AnvilPlugin) {
        ABORTS.fetch_add(1, Ordering::Relaxed);
    }
    let (_guard, table) = fake_host();
    let mut with_groups = *table;
    with_groups.begin_group = Some(begin);
    with_groups.commit_group = Some(commit);
    with_groups.abort_group = Some(abort);
    assert_eq!(load::<Quiet>(Box::leak(Box::new(with_groups))), ANVIL_OK);
    let host = Host::current().unwrap();
    ABORTS.store(0, Ordering::Relaxed);

    // Outside any adapter (a plugin's own thread, say): the group aborts,
    // logs the panic with its place, and the panic goes on.
    let result =
        std::panic::catch_unwind(|| host.group(|| -> crate::Result<()> { panic!("in a group") }));
    assert!(result.is_err());
    assert_eq!(ABORTS.load(Ordering::Relaxed), 1);
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(log[0].starts_with("panic: in a group at "), "{log:?}");
    assert!(log[0].contains("tests.rs"), "{log:?}");

    // Inside an adapter: the adapter's line only, with the group's message.
    host.mid_hook(5, move |_| {
        let _ = host.group(|| -> crate::Result<()> { panic!("again") });
    })
    .unwrap()
    .keep();
    let (callback, user) = with(|r| r.mid_hooks[0]);
    let callback: unsafe extern "C" fn(*mut AnvilCpuContext, *mut c_void) =
        unsafe { std::mem::transmute(callback) };
    let mut cpu: AnvilCpuContext = unsafe { std::mem::zeroed() };
    unsafe { callback(&mut cpu, user as *mut c_void) };
    assert_eq!(ABORTS.load(Ordering::Relaxed), 2);
    let log = logged();
    assert_eq!(log.len(), 2, "{log:?}");
    assert!(
        log[1].starts_with("mid_hook symbol 5: the callback panicked (again at "),
        "{log:?}"
    );
    assert_eq!(host.group(|| Ok(7)).unwrap(), 7);
}

#[test]
fn a_group_whose_changes_are_dropped_frees_the_hooks_it_filled() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static SWAP: PointerSwap = PointerSwap::new();
    static KEPT: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static COMMIT: AtomicU32 = AtomicU32::new(0);
    unsafe extern "C" fn begin(_: *mut AnvilPlugin) -> AnvilResult {
        ANVIL_OK
    }
    unsafe extern "C" fn commit(_: *mut AnvilPlugin) -> AnvilResult {
        COMMIT.load(Ordering::Relaxed) as AnvilResult
    }
    unsafe extern "C" fn abort(_: *mut AnvilPlugin) {}
    unsafe extern "C" fn swap(
        _: *mut AnvilPlugin,
        _slot: AnvilSymbol,
        _value: *mut c_void,
        _previous: *mut *mut c_void,
        out: *mut *mut AnvilChange,
    ) -> AnvilResult {
        unsafe { *out = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed) as *mut AnvilChange };
        ANVIL_OK
    }
    let (_guard, table) = fake_host();
    let mut with_groups = *table;
    with_groups.begin_group = Some(begin);
    with_groups.commit_group = Some(commit);
    with_groups.abort_group = Some(abort);
    with_groups.swap_pointer = Some(swap);
    assert_eq!(load::<Quiet>(Box::leak(Box::new(with_groups))), ANVIL_OK);
    let host = Host::current().unwrap();
    COMMIT.store(ANVIL_OK as u32, Ordering::Relaxed);
    // A hook kept from before the group is not the group's.
    unsafe { host.hook(&KEPT, 1, fake_original) }.unwrap();

    // The body fails: Anvil drops what it requested.
    let failed = host.group(|| -> crate::Result<()> {
        unsafe { host.hook(&HOOK, 1, fake_original) }?;
        unsafe { host.swap_pointer(&SWAP, 2, ptr::null_mut()) }?;
        Err(crate::Error::new(
            ErrorCode::InvalidArgument,
            "the body failed",
        ))
    });
    assert!(failed.is_err());
    assert!(!HOOK.is_active() && !SWAP.is_active());
    assert!(KEPT.is_active());

    // A panic in the body: the same.
    let panicked = std::panic::catch_unwind(|| {
        host.group(|| -> crate::Result<()> {
            unsafe { host.hook(&HOOK, 1, fake_original) }?;
            panic!("in a group")
        })
    });
    assert!(panicked.is_err());
    assert!(!HOOK.is_active());

    // Anvil refuses the commit (enabling failed: none enabled).
    COMMIT.store(ANVIL_ERROR_CONFLICT as u32, Ordering::Relaxed);
    let refused = host.group(|| unsafe { host.hook(&HOOK, 1, fake_original) }.map(|_| ()));
    assert_eq!(refused.unwrap_err().code(), ErrorCode::Conflict);
    assert!(!HOOK.is_active());

    // Committed: the hook keeps its change, and a later failed group leaves it.
    COMMIT.store(ANVIL_OK as u32, Ordering::Relaxed);
    host.group(|| unsafe { host.hook(&HOOK, 1, fake_original) }.map(|_| ()))
        .unwrap();
    assert!(HOOK.is_active());
    let _ = host.group(|| -> crate::Result<()> {
        Err(crate::Error::new(
            ErrorCode::InvalidArgument,
            "nothing requested",
        ))
    });
    assert!(HOOK.is_active() && KEPT.is_active());
    assert!(with(|r| r.removed.is_empty()));
    HOOK.remove().unwrap();
    KEPT.remove().unwrap();
}

/// A fake core for groups whose commit fails only for `FAIL_COMMIT_OF` (an
/// owner handle as an address), and with modules.
static FAIL_COMMIT_OF: AtomicUsize = AtomicUsize::new(0);

fn host_with_groups_and_modules() -> (MutexGuard<'static, ()>, Host) {
    unsafe extern "C" fn begin(_: *mut AnvilPlugin) -> AnvilResult {
        ANVIL_OK
    }
    unsafe extern "C" fn commit(owner: *mut AnvilPlugin) -> AnvilResult {
        if owner as usize == FAIL_COMMIT_OF.load(Ordering::Relaxed) {
            ANVIL_ERROR_CONFLICT
        } else {
            ANVIL_OK
        }
    }
    unsafe extern "C" fn abort(_: *mut AnvilPlugin) {}
    let (guard, table) = fake_host();
    let mut with_groups = *table;
    with_groups.begin_group = Some(begin);
    with_groups.commit_group = Some(commit);
    with_groups.abort_group = Some(abort);
    with_groups.module = Some(fake_module);
    assert_eq!(load::<Quiet>(Box::leak(Box::new(with_groups))), ANVIL_OK);
    FAIL_COMMIT_OF.store(0, Ordering::Relaxed);
    (guard, Host::current().unwrap())
}

#[test]
fn a_hook_filled_on_another_thread_in_a_dropped_group_is_freed() {
    static MINE: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static THEIRS: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    let (_guard, host) = host_with_groups_and_modules();
    let failed = host.group(|| -> crate::Result<()> {
        unsafe { host.hook(&MINE, 1, fake_original) }?;
        // A thread the body started (or a panel click) requests on the same host.
        std::thread::spawn(move || unsafe { host.hook(&THEIRS, 1, fake_original) }.map(|_| ()))
            .join()
            .unwrap()?;
        Err(crate::Error::new(
            ErrorCode::InvalidArgument,
            "the body failed",
        ))
    });
    assert!(failed.is_err());
    assert!(!MINE.is_active() && !THEIRS.is_active());
    // Both take a new request.
    unsafe { host.hook(&THEIRS, 1, fake_original) }.unwrap();
    assert!(THEIRS.is_active());
    THEIRS.remove().unwrap();
}

#[test]
fn a_modules_hook_is_not_part_of_the_plugins_group() {
    static PLUGIN: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static MODULE: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    let (_guard, host) = host_with_groups_and_modules();
    let module = host.module("drill_all").unwrap();
    let failed = host.group(|| -> crate::Result<()> {
        unsafe { host.hook(&PLUGIN, 1, fake_original) }?;
        // The module's own change, enabled at once.
        unsafe { module.hook(&MODULE, 1, fake_original) }?;
        Err(crate::Error::new(
            ErrorCode::InvalidArgument,
            "the body failed",
        ))
    });
    assert!(failed.is_err());
    assert!(!PLUGIN.is_active() && MODULE.is_active());
    MODULE.remove().unwrap();
}

#[test]
fn a_modules_group_inside_the_plugins_keeps_the_plugins_record() {
    static BEFORE: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static MODULE: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static AFTER: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    let (_guard, host) = host_with_groups_and_modules();
    let module = host.module("drill_all").unwrap();
    // The module's group commits, the plugin's fails: only the plugin's hooks are freed.
    FAIL_COMMIT_OF.store(host.raw_plugin() as usize, Ordering::Relaxed);
    let refused = host.group(|| -> crate::Result<()> {
        unsafe { host.hook(&BEFORE, 1, fake_original) }?;
        module.group(|| unsafe { module.hook(&MODULE, 1, fake_original) }.map(|_| ()))?;
        unsafe { host.hook(&AFTER, 1, fake_original) }?;
        Ok(())
    });
    assert_eq!(refused.unwrap_err().code(), ErrorCode::Conflict);
    assert!(!BEFORE.is_active() && !AFTER.is_active() && MODULE.is_active());
    MODULE.remove().unwrap();
    // The module's group fails inside the plugin's, which commits: only the module's hook is freed.
    FAIL_COMMIT_OF.store(module.raw_plugin() as usize, Ordering::Relaxed);
    let mut inner = None;
    host.group(|| -> crate::Result<()> {
        unsafe { host.hook(&BEFORE, 1, fake_original) }?;
        inner =
            Some(module.group(|| unsafe { module.hook(&MODULE, 1, fake_original) }.map(|_| ())));
        unsafe { host.hook(&AFTER, 1, fake_original) }?;
        Ok(())
    })
    .unwrap();
    assert_eq!(inner.unwrap().unwrap_err().code(), ErrorCode::Conflict);
    assert!(BEFORE.is_active() && AFTER.is_active() && !MODULE.is_active());
    BEFORE.remove().unwrap();
    AFTER.remove().unwrap();
}

#[test]
fn an_event_reads_only_the_size_its_publisher_knows() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let seen = std::sync::Arc::new(Mutex::new(Vec::new()));
    let sink = seen.clone();
    host.subscribe(game::SESSION, move |session: &game::Session| {
        sink.lock().unwrap().push(*session)
    })
    .unwrap();
    let (name, callback, context) = with(|r| r.subscriptions[0].clone());
    assert_eq!(name, "anvil-game.session");
    let callback: unsafe extern "C" fn(*const c_char, *const c_void, *mut c_void) =
        unsafe { std::mem::transmute(callback) };
    // An older publisher that knows only size and change; ironman is set behind it.
    let raw = AnvilGameSession {
        size: 8,
        change: ANVIL_GAME_SESSION_STARTED as u32,
        ironman: 1,
        multiplayer: 1,
        number: 7,
    };
    unsafe {
        callback(
            c"anvil-game.session".as_ptr(),
            ptr::from_ref(&raw).cast(),
            context as *mut c_void,
        )
    };
    let raw = AnvilGameSession {
        size: 24,
        change: ANVIL_GAME_SESSION_ENDED as u32,
        ironman: 1,
        multiplayer: 0,
        number: 2,
    };
    unsafe {
        callback(
            c"anvil-game.session".as_ptr(),
            ptr::from_ref(&raw).cast(),
            context as *mut c_void,
        )
    };
    assert_eq!(
        *seen.lock().unwrap(),
        [
            game::Session {
                change: game::SessionChange::Started,
                ironman: false,
                multiplayer: false,
                number: 0
            },
            game::Session {
                change: game::SessionChange::Ended,
                ironman: true,
                multiplayer: false,
                number: 2
            },
        ]
    );
}

#[test]
fn panel_rows_reach_the_overlay_and_a_panicking_click_is_logged_once() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let panels = panel::Panels::get(&host).unwrap();
    assert_eq!(panels.language(), "en"); // no language() in this table
    let p = panels.create("Test").unwrap();
    p.set_rows(&[
        panel::Row::value("Treasury", "12.5"),
        panel::Row::Separator,
        panel::Row::button("Ref\0resh", 3),
        panel::Row::text("plain"),
    ])
    .unwrap();
    assert_eq!(
        with(|r| r.rows.clone()),
        ["1:Treasury=12.5:0", "3::0", "2:Refresh:3", "1:plain:0"]
    );
    p.on_click(|id| assert!(id != 3, "button {id}")).unwrap();
    let (click, context) = with(|r| r.clicks[0]);
    let click: unsafe extern "C" fn(*mut c_void, u64) = unsafe { std::mem::transmute(click) };
    for id in [1, 3, 3] {
        unsafe { click(context as *mut c_void, id) };
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("panel click handler: the callback panicked (button 3 at "),
        "{log:?}"
    );
}

#[test]
fn world_reads_grow_their_buffers_and_old_services_answer_unsupported() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    NAME_CALLS.store(0, Ordering::Relaxed);
    assert_eq!(w.province_name(295).unwrap(), "Moskva");
    assert_eq!(w.province_name(1).unwrap_err().code(), ErrorCode::NotFound);
    let rus = Tag::new("RUS").unwrap();
    assert_eq!(w.country_provinces(rus).unwrap(), [295, 296, 310]);
    let country = w.country(rus).unwrap();
    assert_eq!(country.capital, Some(295));
    assert_eq!(country.treasury, Some(world::Milli(123_450)));
    assert_eq!(country.stability, None);
    // war_ids is in the table's size but not set; the 1.15 calls are past it.
    assert_eq!(w.war_ids().unwrap_err().code(), ErrorCode::Unsupported);
    assert_eq!(
        w.selected_units().unwrap_err().code(),
        ErrorCode::Unsupported
    );
    assert!(game::Game::get(&host).is_none());
}

#[test]
fn world_reads_rulers_flags_and_variable_names() {
    let (_guard, table) = fake_host();
    FULL_WORLD.store(true, Ordering::Relaxed);
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    let rus = Tag::new("RUS").unwrap();
    let ruler = w.monarch(rus, world::MonarchKind::Ruler).unwrap();
    assert_eq!(ruler.kind, world::MonarchKind::Ruler);
    assert_eq!((ruler.adm, ruler.dip, ruler.mil), (6, 4, 5));
    assert!(ruler.female && !ruler.regency);
    assert_eq!(ruler.birth.to_string(), "1672.6.9");
    assert_eq!(ruler.leader, None);
    assert_eq!(
        w.monarch(rus, world::MonarchKind::Heir).unwrap_err().code(),
        ErrorCode::NotFound
    );
    let flags = w.global_flags().unwrap();
    assert_eq!(flags.len(), 1);
    assert_eq!(flags[0].id, world::FlagId(2));
    assert_eq!(flags[0].date.to_string(), "1700.8.19");
    assert_eq!(
        w.country_variable_names(rus).unwrap(),
        ["treasury_plan", "war_goal"]
    );
    let timed = w.country_timed_modifiers(rus).unwrap();
    assert_eq!(timed.len(), 2);
    assert_eq!(timed[0].key, "obstacle_shipbuilding");
    assert!(timed[0].hidden && !timed[0].ruler);
    assert_eq!(timed[0].start.unwrap().to_string(), "1715.1.1");
    assert_eq!(timed[0].end.unwrap().to_string(), "1725.1.1");
    assert_eq!(timed[1].key, "tsars_reforms");
    assert!(
        timed[1].ruler && !timed[1].hidden && timed[1].start.is_none() && timed[1].end.is_none()
    );
    assert_eq!(w.province_view().unwrap(), 295);
    assert_eq!(w.selected_province().unwrap(), 1);
    assert_eq!(
        (
            w.province_at(226, 9).unwrap(),
            w.province_at(-5, 9).unwrap()
        ),
        (226, 0)
    );
    let unit = w.unit_at(100, 9).unwrap();
    assert_eq!(
        (unit.id, unit.country.unwrap().as_str(), unit.kind),
        (7, "RUS", world::UnitKind::Navy)
    );
    assert_eq!(w.unit_at(5, 9).unwrap_err().code(), ErrorCode::NotFound);
    use world::ExplorationList::{Coast, Sea};
    assert_eq!(w.exploration_regions(&unit, Coast).unwrap(), vec![12, 4]);
    assert_eq!(w.exploration_regions(&unit, Sea).unwrap(), vec![30]);
    let army = world::UnitRef {
        kind: world::UnitKind::Army,
        ..unit
    };
    assert_eq!(
        w.exploration_regions(&army, Sea).unwrap_err().code(),
        ErrorCode::InvalidArgument
    );
    assert_eq!(
        (w.region_name(12).unwrap(), w.region_key(12).unwrap()),
        ("Östersjön".to_owned(), "baltic_sea_region".to_owned())
    );
    assert_eq!(w.region_name(13).unwrap_err().code(), ErrorCode::NotFound);
    assert_eq!(
        w.province_triggered_modifiers(1).unwrap(),
        ["pr_mughal_tomb", "center_of_trade_modifier"]
    );
    assert!(w.province_triggered_modifiers(2).unwrap().is_empty());
    assert_eq!(
        w.province_timed_modifiers(295).unwrap_err().code(),
        ErrorCode::Unsupported
    );
    // Set in the table's size but not filled in: Unsupported, like an older anvil-game.
    assert_eq!(w.flag_id("x").unwrap_err().code(), ErrorCode::Unsupported);
    FULL_WORLD.store(false, Ordering::Relaxed);
}

#[test]
fn world_reads_diplomacy() {
    let (_guard, table) = fake_host();
    FULL_WORLD.store(true, Ordering::Relaxed);
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    let rus = Tag::new("RUS").unwrap();
    let tur = Tag::new("TUR").unwrap();
    let alliances = w
        .country_relations(rus, world::RelationKind::Alliance)
        .unwrap();
    assert_eq!(alliances.len(), 1);
    assert_eq!(
        (alliances[0].first, alliances[0].second),
        (Some(rus), Some(tur))
    );
    assert_eq!(alliances[0].start.unwrap().to_string(), "1700.1.1");
    assert!(alliances[0].end.is_none());
    assert!(w
        .country_relations(rus, world::RelationKind::Warning)
        .unwrap()
        .is_empty());
    assert_eq!(
        w.country_relations(tur, world::RelationKind::Alliance)
            .unwrap_err()
            .code(),
        ErrorCode::NotFound
    );
    let status = w.diplomacy_status(rus, tur).unwrap();
    assert_eq!(
        status,
        world::DiplomacyStatus {
            opinion: -108,
            timed_sum: -399,
            trust: 99,
            favors: world::Milli(37590),
            truce: true
        }
    );
    assert_eq!(
        w.diplomacy_status(rus, rus).unwrap_err().code(),
        ErrorCode::InvalidArgument
    );
    assert_eq!(w.country_overlord(tur).unwrap(), rus);
    assert_eq!(
        w.country_overlord(rus).unwrap_err().code(),
        ErrorCode::NotFound
    );
    assert_eq!(w.country_subjects(rus).unwrap(), [tur]);
    assert_eq!(
        w.player_tags().unwrap(),
        world::PlayerTags {
            tag: Some(rus),
            own: Tag::new("O00"),
            observed: Some(rus),
            observer: true
        }
    );
    // In the table's size but not filled in: Unsupported.
    assert_eq!(
        w.subject_type(rus, tur).unwrap_err().code(),
        ErrorCode::Unsupported
    );
    assert_eq!(
        w.country_rivals(rus).unwrap_err().code(),
        ErrorCode::Unsupported
    );
    FULL_WORLD.store(false, Ordering::Relaxed);
}

#[test]
fn world_reads_pending_events_and_statistics() {
    let (_guard, table) = fake_host();
    FULL_WORLD.store(true, Ordering::Relaxed);
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    let tur = Tag::new("TUR").unwrap();
    assert_eq!(w.pending_events(None).unwrap(), [5, 8]);
    assert_eq!(w.pending_events(Some(tur)).unwrap(), [7]);
    assert_eq!(
        w.pending_events(Tag::new("SWE")).unwrap_err().code(),
        ErrorCode::NotFound
    );
    let e = w.pending_event(7).unwrap();
    assert_eq!(e.pending_id, 7);
    assert_eq!(e.country, Some(tur));
    assert_eq!(e.province, Some(151));
    assert_eq!(e.options, 2);
    assert!(e.hidden);
    assert_eq!(e.auto_select.to_string(), "1719.7.17");
    assert_eq!(w.pending_event(8).unwrap_err().code(), ErrorCode::NotFound);
    assert_eq!(w.pending_event_key(7).unwrap(), "flavor_tur.203");
    // In the table's size but not filled in: Unsupported.
    assert_eq!(
        w.pending_event_title(7).unwrap_err().code(),
        ErrorCode::Unsupported
    );
    let rus = Tag::new("RUS").unwrap();
    let income = w
        .country_statistics(rus, world::StatisticKind::Income)
        .unwrap();
    assert_eq!(
        income,
        [
            world::StatisticPoint {
                year: 1717,
                value: 0
            },
            world::StatisticPoint {
                year: 1718,
                value: 1584
            }
        ]
    );
    assert_eq!(
        w.country_statistics(rus, world::StatisticKind::Score)
            .unwrap_err()
            .code(),
        ErrorCode::NotFound
    );
    FULL_WORLD.store(false, Ordering::Relaxed);

    let change = AnvilGamePendingChange {
        size: std::mem::size_of::<AnvilGamePendingChange>() as u32,
        change: ANVIL_GAME_PENDING_REMOVED as u32,
        pending_id: 5,
        country: rus.to_raw(),
        session: 3,
    };
    let read = unsafe { game::PendingChange::read(ptr::from_ref(&change).cast()) }.unwrap();
    assert_eq!(read.change, game::PendingChangeKind::Removed);
    assert_eq!(
        (read.pending_id, read.country, read.session),
        (5, Some(rus), 3)
    );
}

#[test]
fn lobby_payload_reads_its_change() {
    let lobby = |change: u32| AnvilGameLobby {
        size: std::mem::size_of::<AnvilGameLobby>() as u32,
        change,
    };
    for (raw, expected) in [
        (ANVIL_GAME_LOBBY_ENTERED as u32, game::LobbyChange::Entered),
        (ANVIL_GAME_LOBBY_LEFT as u32, game::LobbyChange::Left),
        (7, game::LobbyChange::Other(7)),
    ] {
        let l = lobby(raw);
        let read = unsafe { game::LobbyChange::read(ptr::from_ref(&l).cast()) };
        assert_eq!(read, Some(expected));
    }
    let short = AnvilGameLobby { size: 4, change: 1 };
    assert_eq!(
        unsafe { game::LobbyChange::read(ptr::from_ref(&short).cast()) },
        None
    );
}

#[test]
fn input_binds_and_delivers_presses_through_its_adapter() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let i = input::Input::get(&host).unwrap();
    static PRESSES: Mutex<Vec<input::Press>> = Mutex::new(Vec::new());
    let flags = input::BindFlags {
        consume: true,
        repeat: false,
    };
    let binding = i
        .bind(crate::Key(0x24B), flags, |press| {
            PRESSES.lock().unwrap().push(press)
        })
        .unwrap();
    assert_eq!(binding.as_raw() as usize, 0x99);
    assert_eq!(
        i.bind(crate::Key(0x77), flags, |_| {}).unwrap_err().code(),
        ErrorCode::Conflict
    );
    let (inert, conflict) = i.bind_or_inert(crate::Key(0x77), flags, |_| {}).unwrap();
    assert_eq!(inert.as_raw() as usize, 0x98);
    assert_eq!(conflict.unwrap().code(), ErrorCode::Conflict);
    let (held, conflict) = i.bind_or_inert(crate::Key(0x24C), flags, |_| {}).unwrap();
    assert_eq!(held.as_raw() as usize, 0x99);
    assert!(conflict.is_none());
    let (code, raw_flags, handler, context) = with(|r| r.bindings[0]);
    assert_eq!((code, raw_flags), (0x24B, ANVIL_INPUT_CONSUME));
    let handler: unsafe extern "C" fn(*mut c_void, *const AnvilInputEvent) =
        unsafe { std::mem::transmute(handler) };
    let event = AnvilInputEvent {
        size: std::mem::size_of::<AnvilInputEvent>() as u32,
        flags: ANVIL_INPUT_EVENT_REPEAT,
        code: 0x24B,
        x: 10,
        y: 20,
        frame: 7,
    };
    unsafe { handler(context as *mut c_void, &event) };
    let presses = PRESSES.lock().unwrap().clone();
    assert_eq!(
        presses,
        [input::Press {
            code: crate::Key(0x24B),
            repeat: true,
            x: 10,
            y: 20,
            frame: 7
        }]
    );
    assert!(i.held(crate::Key(0x241)) && !i.held(crate::Key(0x242)));
    // state, set_code and unbind are past nothing but unset: Unsupported.
    assert!(i.state().is_none());
    assert_eq!(binding.unbind().unwrap_err().code(), ErrorCode::Unsupported);
}

#[test]
fn commands_post_for_this_plugin_with_the_units_as_given() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let c = commands::Commands::get(&host).unwrap();
    let rus = Tag::new("RUS");
    let army = world::UnitRef {
        id: 0x36,
        country: rus,
        kind: world::UnitKind::Army,
    };
    let navy = world::UnitRef {
        id: 0x37,
        country: rus,
        kind: world::UnitKind::Navy,
    };
    assert_eq!(
        c.drill(&[army, army], true).unwrap(),
        commands::Posted {
            posted: 1,
            skipped: 1,
            refused: 0
        }
    );
    assert_eq!(c.split(&navy).unwrap().posted, 1);
    assert_eq!(c.version(), 3);
    assert_eq!(c.choose_event_option(41, 2).unwrap().posted, 1);
    assert_eq!(c.set_speed(5).unwrap().posted, 2);
    assert_eq!(
        c.explore(&[army], commands::Explore::Land).unwrap().posted,
        1
    );
    assert_eq!(c.move_to(&[army, navy], 1300, true).unwrap().posted, 2);
    assert_eq!(c.siege_area(&[army], 151).unwrap().posted, 1);
    assert_eq!(c.suppress_area(&[army], 151).unwrap().posted, 1);
    assert_eq!(c.merge(&[]).unwrap_err().code(), ErrorCode::InvalidArgument);
    assert_eq!(
        with(|r| r.commands.clone()),
        [
            "5e1f 1 1 36/RUS/0 36/RUS/0",
            "5e1f 3 0 37/RUS/1",
            "5e1f event 41 2 size 48",
            "5e1f speed 5 size 48",
            "5e1f 6 0 explore 1 province 0 units 1 size 48",
            "5e1f 7 2 explore 0 province 1300 units 2 size 48",
            "5e1f 8 0 explore 0 province 151 units 1 size 48",
            "5e1f 9 0 explore 0 province 151 units 1 size 48"
        ]
    );
    assert_eq!(commands::MAX_UNITS, 64);
    assert_eq!(
        ErrorCode::from_result(ANVIL_ERROR_LIMIT),
        Some(ErrorCode::Limit)
    );
    assert_eq!(ErrorCode::Suspended.result(), ANVIL_ERROR_SUSPENDED);
}

#[test]
fn map_modes_register_paint_and_cut_tooltips_at_a_whole_character() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let modes = map::MapModes::get(&host).unwrap();
    let info = map::ModeInfo {
        tab: map::Tab::Trade,
        description: Some("Yours"),
        ..map::ModeInfo::new("p.cores", "Cores")
    };
    let mode = modes
        .register_with_tooltip(&info, |_, province| match province {
            1 => Some("Krak\u{f3}w \u{17c}\nline".to_string()),
            2 => panic!("province 2"),
            _ => None,
        })
        .unwrap();
    assert_eq!(with(|r| r.map_infos.clone()), ["3:p.cores:Cores:Yours:"]);
    modes
        .register(&map::ModeInfo::new("p.plain", "Plain"))
        .unwrap();
    assert_eq!(
        with(|r| (r.map_infos[1].clone(), r.describes.len())),
        ("0:p.plain:Plain::".to_string(), 1)
    );
    assert_eq!(
        modes
            .register(&map::ModeInfo::new("p.taken", "X"))
            .unwrap_err()
            .code(),
        ErrorCode::AlreadyExists
    );

    modes
        .set_colors(mode, &[map::NO_COLOR, map::rgb(1, 2, 3)])
        .unwrap();
    assert_eq!(with(|r| r.colors.clone()), [0xFF00_0000, 0x0001_0203]);
    assert_eq!(
        (modes.mode_id(80), modes.mode_id(1)),
        (Some("p.cores"), None)
    );
    assert_eq!(
        (modes.shown(), modes.id(mode)),
        (Some(mode), Some("p.cores"))
    );
    let shows = MAP_SHOWS.load(Ordering::Relaxed);
    modes.show(mode).unwrap();
    assert_eq!(MAP_SHOWS.load(Ordering::Relaxed), shows + 1);

    let (describe, context) = with(|r| r.describes[0]);
    let describe: unsafe extern "C" fn(
        *mut c_void,
        *const AnvilMapMode,
        i32,
        *mut c_char,
        usize,
    ) -> usize = unsafe { std::mem::transmute(describe) };
    let context = context as *mut c_void;
    // "Kraków " is 8 bytes and "ż" 2 more: 9 fit, so the cut is before "ż".
    let mut buffer = [0x55 as c_char; 10];
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 1, buffer.as_mut_ptr(), 10) },
        8
    );
    assert_eq!(text(buffer.as_ptr()), "Krak\u{f3}w ");
    let mut wide = [0 as c_char; 64];
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 1, wide.as_mut_ptr(), 64) },
        15
    );
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 3, wide.as_mut_ptr(), 64) },
        0
    );
    for _ in 0..2 {
        assert_eq!(
            unsafe { describe(context, mode.as_raw(), 2, wide.as_mut_ptr(), 64) },
            0
        );
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("map mode tooltip: the callback panicked (province 2 at "),
        "{log:?}"
    );

    let change = AnvilMapModeChange {
        size: std::mem::size_of::<AnvilMapModeChange>() as u32,
        reserved: 0,
        mode: MAP_MODE,
        id: c"p.cores".as_ptr(),
    };
    let read = unsafe { map::ModeChange::read(ptr::from_ref(&change).cast()) }.unwrap();
    assert_eq!((read.mode, read.id), (Some(mode), Some("p.cores")));
    let old = AnvilMapModeChange { size: 8, ..change };
    let read = unsafe { map::ModeChange::read(ptr::from_ref(&old).cast()) }.unwrap();
    assert_eq!((read.mode, read.id), (None, None));
}

#[test]
fn milli_tags_and_errors_read_as_people_write_them() {
    assert_eq!(world::Milli(4500).to_string(), "4.5");
    assert_eq!(world::Milli(-12000).to_string(), "-12");
    assert_eq!(world::Milli(125).to_string(), "0.125");
    assert_eq!(world::Milli(-1).to_string(), "-0.001");
    assert_eq!(
        Tag::new("RUS").map(|t| t.to_string()).as_deref(),
        Some("RUS")
    );
    assert!(Tag::new("RU").is_none() && Tag::new("RUSS").is_none() && Tag::new("R-S").is_none());
    assert_eq!(Tag::from_raw(&AnvilGameTag { text: [0; 4] }), None);
    let error = crate::Error::new(ErrorCode::Conflict, "hook_function symbol 0");
    assert_eq!(
        error.to_string(),
        "hook_function symbol 0: conflict with another change"
    );
    assert_eq!(ErrorCode::from_result(ANVIL_OK), None);
    assert_eq!(ErrorCode::from_result(42), Some(ErrorCode::Other(42)));
    assert_eq!(ErrorCode::Other(0).result(), ANVIL_ERROR_INTERNAL);
}

#[test]
fn event_and_service_names_are_the_headers() {
    let pairs: [(&CStr, &[u8]); 13] = [
        (crate::PLUGINS_LOADED.name(), ANVIL_EVENT_PLUGINS_LOADED),
        (crate::SUSPENDED.name(), ANVIL_EVENT_SUSPENDED),
        (crate::RESUMED.name(), ANVIL_EVENT_RESUMED),
        (game::SESSION.name(), ANVIL_GAME_EVENT_SESSION),
        (game::WAR.name(), ANVIL_GAME_EVENT_WAR),
        (game::SERVICE, ANVIL_GAME_SERVICE),
        (world::SERVICE, ANVIL_GAME_WORLD_SERVICE),
        (commands::SERVICE, ANVIL_GAME_COMMANDS_SERVICE),
        (camera::SERVICE, ANVIL_GAME_CAMERA_SERVICE),
        (panel::SERVICE, ANVIL_PANEL_SERVICE),
        (input::SERVICE, ANVIL_INPUT_SERVICE),
        (crate::benchmark::SERVICE, ANVIL_BENCHMARK_SERVICE),
        (map::SERVICE, ANVIL_MAP_MODES_SERVICE),
    ];
    for (ours, header) in pairs {
        assert_eq!(ours.to_bytes_with_nul(), header);
    }
    for (ours, header) in [
        (game::READY.name(), ANVIL_GAME_EVENT_READY.as_slice()),
        (game::FRAME.name(), ANVIL_GAME_EVENT_FRAME),
        (game::MAIN_MENU.name(), ANVIL_GAME_EVENT_MAIN_MENU),
        (game::OOS.name(), ANVIL_GAME_EVENT_OOS),
        (game::DAY.name(), ANVIL_GAME_EVENT_DAY),
        (game::MONTH.name(), ANVIL_GAME_EVENT_MONTH),
        (game::YEAR.name(), ANVIL_GAME_EVENT_YEAR),
        (game::PLAYER.name(), ANVIL_GAME_EVENT_PLAYER),
        (game::PENDING_EVENT.name(), ANVIL_GAME_EVENT_PENDING),
        (game::LOBBY.name(), ANVIL_GAME_EVENT_LOBBY),
        (crate::benchmark::QUIT.name(), ANVIL_BENCHMARK_EVENT_QUIT),
        (map::SHOWN.name(), ANVIL_MAP_EVENT_SHOWN),
        (map::HIDDEN.name(), ANVIL_MAP_EVENT_HIDDEN),
    ] {
        assert_eq!(ours.to_bytes_with_nul(), header);
    }
}

#[test]
fn day_end_subscribes_with_the_plugin_and_passes_the_day() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let days = script::DayEnd::get(&host).unwrap();
    static SEEN: Mutex<Vec<script::Day>> = Mutex::new(Vec::new());
    SEEN.lock().unwrap().clear();
    days.subscribe(&host, |day| SEEN.lock().unwrap().push(*day))
        .unwrap();
    days.subscribe(&host, |day| {
        if day.new_year {
            panic!("a new year");
        }
    })
    .unwrap();
    assert_eq!(
        days.subscribe(&host, |_| {}).unwrap_err().code(),
        ErrorCode::WrongPhase
    );
    let subscribed = with(|r| r.day_ends.clone());
    assert_eq!(subscribed.len(), 2);
    assert_eq!(subscribed[0].0, SELF as usize);

    let call = |index: usize, raw: &AnvilScriptDay| {
        let (_, callback, context) = subscribed[index];
        let callback: unsafe extern "C" fn(*mut c_void, *const AnvilScriptDay) =
            unsafe { std::mem::transmute(callback) };
        unsafe { callback(context as *mut c_void, raw) };
    };
    let new_year = AnvilScriptDay {
        size: std::mem::size_of::<AnvilScriptDay>() as u32,
        flags: ANVIL_SCRIPT_DAY_NEW_MONTH | ANVIL_SCRIPT_DAY_NEW_YEAR,
        year: 1445,
        month: 1,
        day: 1,
    };
    call(0, &new_year);
    assert_eq!(
        *SEEN.lock().unwrap(),
        [script::Day {
            year: 1445,
            month: 1,
            day: 1,
            new_month: true,
            new_year: true
        }]
    );
    // A panic stays in the adapter and is logged once.
    call(1, &new_year);
    call(1, &new_year);
    let panics = logged()
        .iter()
        .filter(|l| l.starts_with("day end: the callback panicked"))
        .count();
    assert_eq!(panics, 1);
    let ordinary = AnvilScriptDay {
        flags: 0,
        day: 2,
        ..new_year
    };
    call(1, &ordinary);
    call(0, &ordinary);
    assert_eq!(
        SEEN.lock().unwrap()[1],
        script::Day {
            year: 1445,
            month: 1,
            day: 2,
            new_month: false,
            new_year: false
        }
    );
    // A day of a publisher that knows only `size` and `flags`: nothing past
    // them is read, the date is zero.
    let short = AnvilScriptDay {
        size: std::mem::offset_of!(AnvilScriptDay, year) as u32,
        ..new_year
    };
    call(0, &short);
    assert_eq!(
        *SEEN.lock().unwrap().last().unwrap(),
        script::Day {
            year: 0,
            month: 0,
            day: 0,
            new_month: true,
            new_year: true
        }
    );
}

fn raw_call(key: &CStr, value: &CStr, province: i32, country: &[u8; 4]) -> AnvilScriptCall {
    AnvilScriptCall {
        size: std::mem::size_of::<AnvilScriptCall>() as u32,
        reserved: 0,
        key: key.as_ptr(),
        value: value.as_ptr(),
        scope: AnvilScriptScope {
            size: std::mem::size_of::<AnvilScriptScope>() as u32,
            province,
            country: country.map(|b| b as c_char),
        },
    }
}

#[test]
fn effects_register_with_the_plugin_and_pass_the_call() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let effects = script::Effects::get(&host).unwrap();
    type Seen = (String, String, Option<i32>, Option<Tag>);
    static SEEN: Mutex<Vec<Seen>> = Mutex::new(Vec::new());
    SEEN.lock().unwrap().clear();
    effects
        .register_with_tooltip(
            &host,
            "chisel_set_terrain",
            |call| {
                SEEN.lock().unwrap().push((
                    call.key.to_owned(),
                    call.value.clone().into_owned(),
                    call.province,
                    call.country,
                ));
                if call.value == "boom" {
                    panic!("a bad value");
                }
            },
            |call| match call.value.as_ref() {
                "none" => None,
                "boom" => panic!("a bad tooltip"),
                value => Some(format!("Terrain becomes {value}\nignored")),
            },
        )
        .unwrap();
    effects
        .register(&host, "chisel_reset_terrain", |_| {})
        .unwrap();
    // A taken key, and a key the C API cannot carry (no call made).
    assert_eq!(
        effects
            .register(&host, "chisel_set_terrain", |_| {})
            .unwrap_err()
            .code(),
        ErrorCode::AlreadyExists
    );
    assert_eq!(
        effects
            .register(&host, "chisel\0x", |_| {})
            .unwrap_err()
            .code(),
        ErrorCode::InvalidArgument
    );

    let registered = with(|r| r.effects.clone());
    assert_eq!(registered.len(), 2);
    assert_eq!(registered[0].0, SELF as usize);
    assert_eq!(registered[0].1, "chisel_set_terrain");
    assert_ne!(registered[0].3, 0); // a describe callback
    assert_eq!(registered[1].1, "chisel_reset_terrain");
    assert_eq!(registered[1].3, 0); // none: no tooltip line

    let execute = |index: usize, call: &AnvilScriptCall| {
        let (_, _, f, _, context) = registered[index].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall) =
            unsafe { std::mem::transmute(f) };
        unsafe { f(context as *mut c_void, call) };
    };
    let describe = |call: &AnvilScriptCall, capacity: usize| -> (u32, String) {
        let (_, _, _, f, context) = registered[0].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall, *mut c_char, u32) -> u32 =
            unsafe { std::mem::transmute(f) };
        let mut buffer = vec![0x55u8; capacity.max(1)];
        let whole = unsafe {
            f(
                context as *mut c_void,
                call,
                if capacity == 0 {
                    ptr::null_mut()
                } else {
                    buffer.as_mut_ptr().cast()
                },
                capacity as u32,
            )
        };
        let end = buffer.iter().position(|&b| b == 0).unwrap_or(0);
        (whole, String::from_utf8(buffer[..end].to_vec()).unwrap())
    };

    // A province scope and a country scope; a NULL call is ignored.
    execute(
        0,
        &raw_call(c"chisel_set_terrain", c"mountain", 295, b"\0\0\0\0"),
    );
    execute(0, &raw_call(c"chisel_set_terrain", c"hills", 0, b"RUS\0"));
    {
        let (_, _, f, _, context) = registered[0].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall) =
            unsafe { std::mem::transmute(f) };
        unsafe { f(context as *mut c_void, ptr::null()) };
    }
    assert_eq!(
        *SEEN.lock().unwrap(),
        [
            (
                "chisel_set_terrain".to_owned(),
                "mountain".to_owned(),
                Some(295),
                None
            ),
            (
                "chisel_set_terrain".to_owned(),
                "hills".to_owned(),
                None,
                Tag::new("RUS")
            ),
        ]
    );
    // A call of an anvil-script that knows no scope: the scope is not read.
    let unscoped = AnvilScriptCall {
        size: std::mem::offset_of!(AnvilScriptCall, scope) as u32,
        ..raw_call(c"chisel_set_terrain", c"steppe", 295, b"RUS\0")
    };
    execute(0, &unscoped);
    assert_eq!(
        SEEN.lock().unwrap().last().unwrap(),
        &(
            "chisel_set_terrain".to_owned(),
            "steppe".to_owned(),
            None,
            None
        )
    );

    // The first line only, its whole length, cut at a whole character.
    let call = raw_call(c"chisel_set_terrain", c"desert", 183, b"\0\0\0\0");
    assert_eq!(
        describe(&call, 64),
        (22, "Terrain becomes desert".to_owned())
    );
    assert_eq!(describe(&call, 8), (22, "Terrain".to_owned()));
    assert_eq!(describe(&call, 0).0, 22);
    let wide = raw_call(
        c"chisel_set_terrain",
        c"\xc3\xa9t\xc3\xa9",
        183,
        b"\0\0\0\0",
    );
    // "Terrain becomes été": 16 + 5 bytes; 18 bytes of room cut before the second byte of é.
    assert_eq!(describe(&wide, 18), (21, "Terrain becomes ".to_owned()));
    assert_eq!(
        describe(
            &raw_call(c"chisel_set_terrain", c"none", 1, b"\0\0\0\0"),
            64
        )
        .0,
        0
    );

    // Panics stay in the adapters and are logged once per callback.
    let boom = raw_call(c"chisel_set_terrain", c"boom", 1, b"\0\0\0\0");
    execute(0, &boom);
    execute(0, &boom);
    assert_eq!(describe(&boom, 64).0, 0);
    assert_eq!(describe(&boom, 64).0, 0);
    let log = logged();
    assert_eq!(
        log.iter()
            .filter(|l| l.starts_with("script effect: the callback panicked"))
            .count(),
        1
    );
    assert_eq!(
        log.iter()
            .filter(|l| l.starts_with("script effect tooltip: the callback panicked"))
            .count(),
        1
    );
}

#[test]
fn log_messages_keep_totals_and_what_changed() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let m = log_throttle::Messages::get(&host).unwrap();
    m.keep().unwrap();
    let totals = m.totals().unwrap();
    assert!(totals.keeping);
    assert_eq!(
        (
            totals.version,
            totals.messages,
            totals.written,
            totals.counted
        ),
        (2500, 2500, 2600, 7)
    );
    let (part, next) = m.read(0, 0, 3).unwrap();
    assert_eq!(next, 3);
    assert_eq!(part.len(), 3);
    assert_eq!(part[0].count(), 7);
    assert_eq!(part[0].text, "Undefined event target: destination_province");
    assert_eq!(
        (
            part[0].log.as_str(),
            part[0].source.as_str(),
            part[0].source_line
        ),
        ("error.log", "gamestate.cpp", 7653)
    );
    // The whole table in pieces, and only what changed after a version.
    assert_eq!(m.changed_since(0).unwrap().len(), 2500);
    let changed = m.changed_since(2400).unwrap();
    assert_eq!(changed.len(), 100);
    assert_eq!(changed[0].index, 2400);
    assert!(m.read(0, 0, 0).unwrap().0.is_empty());
}

unsafe extern "C" fn fake_module(
    plugin: *mut AnvilPlugin,
    name: *const c_char,
    out: *mut *mut AnvilPlugin,
) -> AnvilResult {
    assert_eq!(plugin, SELF);
    unsafe { *out = ptr::null_mut() };
    match text(name).as_str() {
        "drill_all" => {
            unsafe { *out = 0x40D as *mut AnvilPlugin };
            ANVIL_OK
        }
        "chase" => ANVIL_ERROR_DISABLED,
        _ => ANVIL_ERROR_NOT_DECLARED,
    }
}

#[test]
fn a_module_is_a_host_of_its_own_handle() {
    let (_guard, table) = fake_host();
    let mut with_modules = *table;
    with_modules.module = Some(fake_module);
    let host = Host::from_parts(leak_table(with_modules), SELF);
    let drill = host.module("drill_all").unwrap();
    assert_eq!(drill.raw_plugin(), 0x40D as *mut AnvilPlugin);
    assert_eq!(
        host.module("chase").unwrap_err().code(),
        ErrorCode::Disabled
    );
    assert_eq!(
        host.module("nope").unwrap_err().code(),
        ErrorCode::NotDeclared
    );
    assert_eq!(ErrorCode::Disabled.result(), ANVIL_ERROR_DISABLED);
    assert_eq!(
        ErrorCode::from_result(ANVIL_ERROR_DISABLED),
        Some(ErrorCode::Disabled)
    );
    // A host table that ends before the field: the call is not there.
    let mut short = with_modules;
    short.size = std::mem::offset_of!(AnvilHost, module) as u32;
    let old = Host::from_parts(leak_table(short), SELF);
    assert_eq!(
        old.module("drill_all").unwrap_err().code(),
        ErrorCode::Unsupported
    );
}

#[test]
fn game_reads_the_speed_and_the_pause() {
    let (_guard, table) = fake_host();
    WITH_GAME.store(true, Ordering::Relaxed);
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let g = game::Game::get(&host).unwrap();
    assert_eq!(g.speed(), Some(3));
    let pause = g.paused().unwrap();
    assert!(pause.player() && !pause.game());
    assert_eq!(
        g.user_dir(),
        Some(std::path::PathBuf::from("C:\\user\u{EB}"))
    );
    // In the table's size but not filled in: None.
    assert_eq!(g.out_of_sync(), None);
}

#[test]
fn camera_reads_the_view_and_pans() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let c = camera::Camera::get(&host).unwrap();
    assert_eq!(
        c.view().unwrap(),
        camera::View {
            position: [10.0, 300.0, 20.0],
            look_at: [10.0, 0.0, 40.0],
            locked: true,
        }
    );
    assert!(c.pan(1.0, -0.5).is_ok());
    assert_eq!(c.pan(0.0, 1.0).unwrap_err().code(), ErrorCode::WrongPhase);
}

/// A host table for the life of the test process, as Anvil keeps its own.
fn leak_table(table: AnvilHost) -> crate::Table<AnvilHost> {
    let table: &'static AnvilHost = Box::leak(Box::new(table));
    unsafe { crate::Table::from_raw(table) }.unwrap()
}

#[test]
fn a_shorter_table_is_read_only_as_far_as_its_size() {
    // An older provider's table: `view` only, and no bytes for `pan` behind it.
    #[repr(C)]
    struct OldCamera {
        size: u32,
        view: Option<unsafe extern "C" fn(*mut AnvilGameCameraView) -> AnvilResult>,
    }
    static OLD: OldCamera = OldCamera {
        size: std::mem::size_of::<OldCamera>() as u32,
        view: Some(camera_view),
    };
    let table =
        unsafe { crate::Table::from_raw(ptr::from_ref(&OLD).cast::<AnvilGameCameraService>()) }
            .unwrap();
    assert_eq!(table.size() as usize, std::mem::size_of::<OldCamera>());
    assert!(table.get().is_none());
    assert!(function!(table, AnvilGameCameraService, view).is_ok());
    assert_eq!(
        function!(table, AnvilGameCameraService, pan)
            .unwrap_err()
            .code(),
        ErrorCode::Unsupported
    );
    assert!(unsafe { crate::Table::<AnvilGameCameraService>::from_raw(ptr::null()) }.is_none());
    let full = unsafe { crate::Table::from_raw(ptr::from_ref(&FAKE_CAMERA.0)) }.unwrap();
    assert!(full.get().is_some());
}

#[test]
fn a_detour_entered_as_soon_as_a_first_hook_is_enabled_has_next() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    static SEEN_AT_ENABLE: AtomicU32 = AtomicU32::new(0); // 1 = None, 2 = Some
    unsafe extern "C" fn detour(x: i32) -> i32 {
        match HOOK.next() {
            Some(next) => (unsafe { next(x) }) * 10,
            None => -1,
        }
    }
    // As the core does after load, outside a group: *next is written, then the
    // change is enabled inside the call; a game thread may enter the detour at once.
    unsafe extern "C" fn hook_and_enable(
        _: *mut AnvilPlugin,
        _: AnvilSymbol,
        detour: *mut c_void,
        next: *mut *mut c_void,
        out: *mut *mut AnvilChange,
    ) -> AnvilResult {
        let original: unsafe extern "C" fn(i32) -> i32 = fake_original;
        unsafe { *next = original as *mut c_void };
        // enable(): the game's next call goes through the detour.
        let d: unsafe extern "C" fn(i32) -> i32 = unsafe { std::mem::transmute(detour) };
        let r = unsafe { d(4) };
        SEEN_AT_ENABLE.store(if r == -1 { 1 } else { 2 }, Ordering::Relaxed);
        unsafe { *out = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed) as *mut AnvilChange };
        ANVIL_OK
    }
    let (_guard, table) = fake_host();
    let mut t = *table;
    t.hook_function = Some(hook_and_enable);
    assert_eq!(load::<Quiet>(Box::leak(Box::new(t))), ANVIL_OK);
    let host = Host::current().unwrap();
    unsafe { host.hook(&HOOK, 1, detour) }.unwrap();
    // The game call that came right after enabling (inside the request) has next.
    assert_eq!(
        SEEN_AT_ENABLE.load(Ordering::Relaxed),
        2,
        "detour ran with next() == None"
    );
    HOOK.remove().unwrap();
}

#[test]
fn a_panic_the_plugin_caught_inside_an_adapter_does_not_name_a_later_one() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    crate::catch_panic("first", (), || {
        let _ = std::panic::catch_unwind(|| panic!("swallowed by the plugin"));
    });
    crate::catch_panic("second", (), || {
        std::panic::resume_unwind(Box::new("the real one"))
    });
    let log = logged();
    let line = log.iter().find(|l| l.starts_with("second")).unwrap();
    assert!(line.contains("the real one"), "{line}");
}

#[test]
fn a_kept_panic_text_names_a_later_panic_only_when_it_is_that_panics_text() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    for (swallowed, later, kind) in [("boom!!!", "boom", "prefix"), ("anything", "", "empty")] {
        crate::catch_panic("first", (), || {
            let _ = std::panic::catch_unwind(|| panic!("{swallowed}"));
        });
        crate::catch_panic(kind, (), || std::panic::resume_unwind(Box::new(later)));
        let log = logged();
        let line = log.iter().rev().find(|l| l.starts_with(kind)).unwrap();
        assert!(!line.contains(swallowed), "{line}");
    }
}
