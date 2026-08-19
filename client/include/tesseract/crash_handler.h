#pragma once

namespace tesseract
{

// Both functions below are always declared, regardless of whether the
// TESSERACT_ENABLE_CRASH_HANDLER CMake option (off by default — the feature
// is still experimental) was on for this build. When it was off, both
// compile to inert no-ops in crash_handler.cpp, so call sites (the 4
// platform entry points, ShellBase) never need to #ifdef around them.

// Installs process-wide native fault handlers (SIGSEGV/SIGABRT/SIGFPE/SIGILL/
// SIGBUS via backward-cpp on POSIX; an unhandled-exception filter via
// backward-cpp's DbgHelp backend on Windows) and the Rust panic hook (see
// sdk/src/crash_reporter.rs). Both are always installed — cheap, and inert
// until an actual crash — so a later call to set_crash_reporting_enabled()
// takes effect without reinstalling any handler. `enabled` only gates
// whether output is retained: when false, a metadata-only file is left
// behind and nothing further is captured.
//
// Call exactly once, as early as possible after
// Settings::instance().load_from_disk() (so `enabled` reflects the
// persisted toggle) and before any other subsystem (AccountManager,
// MainWindow, ...) is constructed. Safe to call more than once; only the
// first call takes effect.
void install_crash_handler(bool enabled);

// Enable/disable writing a crash report at runtime, without reinstalling the
// underlying handlers. Called when the user flips the Settings checkbox —
// takes effect immediately, no restart required.
void set_crash_reporting_enabled(bool enabled);

} // namespace tesseract
