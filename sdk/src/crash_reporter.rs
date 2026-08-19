//! Rust-side half of the optional local crash handler (see
//! client/src/crash_handler.cpp for the native/C++ half). A panic that
//! unwinds across the cxx FFI boundary triggers a process abort before it
//! ever reaches the native signal handler, which by then has lost the
//! original panic message and location — so this hook captures that
//! information up front, while the panic is still live, and appends it to
//! the same crash-report file the C++ side already created.

use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Once, OnceLock};

static INSTALL_ONCE: Once = Once::new();
static CRASH_FILE: OnceLock<String> = OnceLock::new();
static ENABLED: AtomicBool = AtomicBool::new(false);

/// Installs `std::panic::set_hook`. Always installs (cheap, inert unless a
/// panic actually occurs) mirroring the native side always constructing
/// `backward::SignalHandling` — `enabled` only gates whether a panic
/// actually gets written, so `set_enabled` can flip it later without
/// reinstalling the hook. Idempotent: only the first call's `crash_file`
/// takes effect, matching `tesseract::install_crash_handler`'s one-file-
/// per-process-run design.
pub fn install(crash_file: &str, enabled: bool) {
    let _ = CRASH_FILE.set(crash_file.to_owned());
    ENABLED.store(enabled, Ordering::Relaxed);

    INSTALL_ONCE.call_once(|| {
        let default_hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            if ENABLED.load(Ordering::Relaxed) {
                if let Some(path) = CRASH_FILE.get() {
                    write_panic_report(path, info);
                }
            }
            default_hook(info);
        }));
    });
}

/// Flips whether a future panic actually gets written, without touching the
/// installed hook. Mirrors `tesseract::set_crash_reporting_enabled` on the
/// C++ side.
pub fn set_enabled(enabled: bool) {
    ENABLED.store(enabled, Ordering::Relaxed);
}

fn write_panic_report(path: &str, info: &std::panic::PanicHookInfo<'_>) {
    // Best-effort, no synchronization heavier than the append-mode open
    // itself: a crash is terminal, and each Matrix account runs its own
    // dedicated multi-thread tokio runtime (see ClientFfi::new) plus several
    // ad-hoc std::thread's in shared C++ code, so a panic can originate on
    // any of many threads. A Mutex held at panic time risks its own
    // deadlock/re-panic if poisoned; the worst case of a race here is two
    // reports' lines interleaving in one file, which is still readable.
    let backtrace = std::backtrace::Backtrace::force_capture();
    let location = info
        .location()
        .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
        .unwrap_or_else(|| "<unknown location>".to_string());
    let message = if let Some(s) = info.payload().downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = info.payload().downcast_ref::<String>() {
        s.clone()
    } else {
        "<non-string panic payload>".to_string()
    };
    let thread = std::thread::current();
    let thread_name = thread.name().unwrap_or("<unnamed>");

    // Best-effort: if the file can't be opened (disk full, permissions),
    // there is nothing more useful to do — the process aborts regardless
    // once this hook returns and unwinding continues across the FFI
    // boundary.
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(
            f,
            "\n--- Rust panic ---\nthread: {thread_name}\nlocation: {location}\nmessage: {message}\nbacktrace:\n{backtrace}\n"
        );
        let _ = f.flush();
    }
}
