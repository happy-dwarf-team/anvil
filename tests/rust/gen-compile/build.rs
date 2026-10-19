//! Writes symbols.gen.rs (and settings.gen.rs, when there is a settings.toml)
//! into OUT_DIR with `anvil gen --lang rust`.
//! `ANVIL` names the anvil executable (default: `anvil` on the PATH).
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=symbols.toml");
    println!("cargo:rerun-if-changed=settings.toml");
    println!("cargo:rerun-if-env-changed=ANVIL");
    let anvil = env::var_os("ANVIL").unwrap_or_else(|| "anvil".into());
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets OUT_DIR"));
    let mut command = Command::new(&anvil);
    command.args(["gen", "--lang", "rust", "symbols.toml"]);
    if Path::new("settings.toml").exists() {
        command.args(["--settings", "settings.toml"]);
    }
    let status = command.arg(&out).status().unwrap_or_else(|error| {
        panic!("cannot run {anvil:?}: {error}; put anvil on the PATH or set ANVIL to its path")
    });
    assert!(status.success(), "anvil gen failed: {status}");
}
