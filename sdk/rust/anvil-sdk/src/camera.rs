//! `anvil-game.camera`: the game's map camera (anvil-game 1.25.0), client
//! state rather than game state. Only this client's view moves, as with the
//! player's own keys and mouse; nothing goes to the game state or to the other
//! players in multiplayer.
//!
//! **When.** On the game's main thread after an outer frame, in a session, as
//! for [`World`](crate::world::World): from a handler of an anvil-game event or
//! from work queued with [`Game::run_on_main_thread`]. Anywhere else, in the
//! main menu and during a load, [`ErrorCode::WrongPhase`].
//!
//! [`Game::run_on_main_thread`]: crate::game::Game::run_on_main_thread
//! [`ErrorCode::WrongPhase`]: crate::ErrorCode::WrongPhase

use std::fmt;
use std::mem::size_of;

use crate::error::{check, Result};
use crate::host::Host;
use crate::sys::{
    AnvilGameCameraService, AnvilGameCameraView, ANVIL_GAME_CAMERA_LOCKED,
    ANVIL_GAME_CAMERA_SERVICE_VERSION,
};

/// The service's name.
pub const SERVICE: &std::ffi::CStr = c"anvil-game.camera";
/// The service's major version.
pub const VERSION: u32 = ANVIL_GAME_CAMERA_SERVICE_VERSION;

/// The map camera, as [`Camera::view`] reads it, in map units: `[1]` is the
/// height above the map's plane, `[0]` and `[2]` lie on it. After a pan the
/// game moves both points towards their targets over the next frames.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct View {
    /// Where the camera is.
    pub position: [f32; 3],
    /// The point it looks at.
    pub look_at: [f32; 3],
    /// The game holds the camera still (a menu is open, or its graphics
    /// cannot draw): [`Camera::pan`] answers `WrongPhase`.
    pub locked: bool,
}

/// The `anvil-game.camera` service.
#[derive(Clone, Copy)]
pub struct Camera {
    table: &'static AnvilGameCameraService,
}

impl fmt::Debug for Camera {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Camera")
            .field("size", &self.table.size)
            .finish()
    }
}

impl Camera {
    /// The service; `None` without anvil-game 1.25.0 or on a game build whose
    /// camera symbols are declared absent.
    pub fn get(host: &Host) -> Option<Self> {
        // SAFETY: the table type of anvil-game.camera version 1.
        let table = unsafe { host.service::<AnvilGameCameraService>(SERVICE, VERSION) }?;
        Some(Self { table })
    }

    /// The C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilGameCameraService {
        self.table
    }

    /// The map camera now.
    pub fn view(&self) -> Result<View> {
        let f = function!(self.table, AnvilGameCameraService, view)?;
        let mut out = AnvilGameCameraView {
            size: size_of::<AnvilGameCameraView>() as u32,
            position: [0.0; 3],
            look_at: [0.0; 3],
            flags: 0,
        };
        // SAFETY: the service's function with a valid out pointer.
        check(unsafe { f(&mut out) }, || "camera view")?;
        Ok(View {
            position: out.position,
            look_at: out.look_at,
            locked: out.flags & ANVIL_GAME_CAMERA_LOCKED != 0,
        })
    }

    /// Pans the map camera for this frame as the arrow keys do: `right` and
    /// `up` from -1 to 1, a longer vector shortened to length 1, at the
    /// arrows' speed (the player's scroll speed, the camera's height and this
    /// frame's time). Call it once a frame while the input lasts; a second
    /// call in the same frame pans twice. `WrongPhase` also while the camera
    /// is locked, `InvalidArgument` for a value that is not finite; `(0, 0)`
    /// does nothing.
    pub fn pan(&self, right: f32, up: f32) -> Result<()> {
        let f = function!(self.table, AnvilGameCameraService, pan)?;
        // SAFETY: the service's function.
        check(unsafe { f(right, up) }, || "camera pan")
    }
}
