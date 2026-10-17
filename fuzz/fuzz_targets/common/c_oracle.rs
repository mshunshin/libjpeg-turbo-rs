//! Shared plumbing for the differential targets that compare against a C
//! oracle program — `examples/*_c_oracle.c`, built against a libjpeg-turbo
//! 3.x development install by `fuzz/build_c_oracles.sh`.
//!
//! Each oracle reads the JPEG from `/dev/stdin`, exits non-zero when libjpeg
//! refuses it, and writes libjpeg's first warning (if any) to stderr.

#![allow(dead_code)] // each target uses a subset

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::OnceLock;

/// One-shot SIGPIPE masking. With `#![no_main]` libfuzzer skips std's
/// default SIG_IGN setup, so a pipe write to an oracle that exited early
/// would kill the fuzz process with signal 13 instead of returning
/// `Err(BrokenPipe)` (same reason as `fuzz_decode_diff_c`).
pub fn ensure_sigpipe_ignored() {
    static ONCE: OnceLock<()> = OnceLock::new();
    ONCE.get_or_init(|| unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    });
}

/// The oracle binary named by the environment variable `env_var`.
///
/// A differential target without its reference compares nothing, so a
/// missing oracle stops the run with an explanation rather than letting it
/// finish "clean". (`fuzz_decode_diff_c` skips silently when `djpeg` is
/// absent; these oracles are never on a system path, so a skip would only
/// ever mean a misconfigured run.)
pub fn oracle_path(env_var: &'static str) -> PathBuf {
    match std::env::var_os(env_var).map(PathBuf::from) {
        Some(path) if path.is_file() => path,
        _ => {
            eprintln!(
                "{env_var} must name the C oracle built by fuzz/build_c_oracles.sh \
                 (this target compares against it and cannot run without it)"
            );
            std::process::exit(2);
        }
    }
}

/// What libjpeg produced for one input.
pub struct OracleRun {
    pub stdout: Vec<u8>,
    /// libjpeg emitted a warning: it recovered from corrupt data, so its
    /// output is one recovery strategy among several, not a reference.
    pub warned: bool,
}

/// Run `oracle` with `args` (the first is normally `/dev/stdin`), feeding
/// `jpeg` on stdin. `None` when libjpeg refused the input.
pub fn run_oracle(oracle: &Path, args: &[String], jpeg: &[u8]) -> Option<OracleRun> {
    let mut child = Command::new(oracle)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .ok()?;
    // Write on a thread while wait_with_output drains stdout and stderr, so
    // a decoded image larger than the pipe buffer cannot deadlock the pair.
    let mut stdin = child.stdin.take()?;
    let payload: Vec<u8> = jpeg.to_vec();
    let writer = std::thread::spawn(move || {
        let _ = stdin.write_all(&payload);
    });
    let output = child.wait_with_output().ok()?;
    let _ = writer.join();
    if !output.status.success() {
        return None;
    }
    Some(OracleRun {
        stdout: output.stdout,
        warned: !output.stderr.is_empty(),
    })
}

/// Split a `<text header>\n<binary body>` oracle output.
pub fn split_header(stdout: &[u8]) -> Option<(Vec<&str>, &[u8])> {
    let newline: usize = stdout.iter().position(|&b| b == b'\n')?;
    let header: &str = std::str::from_utf8(&stdout[..newline]).ok()?;
    Some((header.split_whitespace().collect(), &stdout[newline + 1..]))
}

/// `LTRS_FUZZ_INCLUDE_KNOWN=1` stops a target skipping the divergences
/// already filed (each skip names its LAST_MILE item). Use it to confirm a
/// target still detects them, or that a fix closed them.
pub fn include_known_gaps() -> bool {
    static INCLUDE: OnceLock<bool> = OnceLock::new();
    *INCLUDE.get_or_init(|| std::env::var("LTRS_FUZZ_INCLUDE_KNOWN").as_deref() == Ok("1"))
}

/// Position and channel of the first `limit` differing samples, for a panic
/// message.
pub fn first_differences(ours: &[u8], theirs: &[u8], channels: usize, limit: usize) -> String {
    ours.iter()
        .zip(theirs)
        .enumerate()
        .filter(|(_, (a, b))| a != b)
        .take(limit)
        .map(|(i, (a, b))| {
            format!(
                "[sample {} ch {}] rust {a} c {b}",
                i / channels,
                i % channels
            )
        })
        .collect::<Vec<String>>()
        .join(", ")
}
