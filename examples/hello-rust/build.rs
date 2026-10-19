//! Writes symbols.gen.rs into OUT_DIR with `anvil gen --lang rust`.
//! `ANVIL` names the anvil executable (default: `anvil` on the PATH).
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=symbols.toml");
    println!("cargo:rerun-if-env-changed=ANVIL");
    let anvil = env::var_os("ANVIL").unwrap_or_else(|| "anvil".into());
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets OUT_DIR"));
    let status = Command::new(&anvil)
        .args(["gen", "--lang", "rust", "symbols.toml"])
        .arg(&out)
        .status()
        .unwrap_or_else(|error| {
            panic!("cannot run {anvil:?}: {error}; put anvil on the PATH or set ANVIL to its path")
        });
    assert!(status.success(), "anvil gen failed: {status}");
}
