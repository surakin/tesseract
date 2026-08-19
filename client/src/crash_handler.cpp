#include "tesseract/crash_handler.h"

#ifdef TESSERACT_CRASH_HANDLER_ENABLED

#include "tesseract/paths.h"
#include "tesseract/version.h"

// cxx-generated header (produced by corrosion_add_cxxbridge)
#include "ffi_convert.h"

#include <backward.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace tesseract
{

namespace
{

namespace fs = std::filesystem;

// A crash report is a diagnostic aid, not an audit log — cap the directory
// rather than let it grow unbounded. A real retention policy (e.g. clearing
// files once a future uploader has sent them) is out of scope here.
constexpr std::size_t kMaxCrashFiles = 20;

std::atomic<bool> g_enabled{false};
std::once_flag g_install_once;
fs::path g_crash_file_path;
int g_saved_stderr_fd = -1;
std::ofstream g_cerr_crash_file;
std::streambuf* g_saved_cerr_rdbuf = nullptr;

long current_pid()
{
#if defined(_WIN32)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

const char* platform_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    // client/ has no notion of which of the two Linux UI backends (Qt6 vs
    // GTK4) is active — that distinction lives in ui/*, several layers up.
    return "linux";
#endif
}

std::string utc_timestamp_basic()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

fs::path crash_dir()
{
    return data_dir() / "crashes";
}

fs::path compute_crash_file_path()
{
    std::string filename =
        "crash-" + utc_timestamp_basic() + "-" + std::to_string(current_pid()) + ".log";
    return crash_dir() / filename;
}

void prune_old_crash_files()
{
    std::error_code ec;
    std::vector<fs::directory_entry> files;
    for (const auto& entry : fs::directory_iterator(crash_dir(), ec))
    {
        if (entry.is_regular_file())
            files.push_back(entry);
    }
    if (files.size() <= kMaxCrashFiles)
        return;

    std::sort(files.begin(), files.end(), [](const fs::directory_entry& a, const fs::directory_entry& b)
    {
        return a.last_write_time() < b.last_write_time();
    });
    std::size_t to_remove = files.size() - kMaxCrashFiles;
    for (std::size_t i = 0; i < to_remove; ++i)
    {
        std::error_code remove_ec;
        fs::remove(files[i].path(), remove_ec);
    }
}

void write_metadata_header(const fs::path& path)
{
    std::ofstream f(path, std::ios::app);
    if (!f.is_open())
        return;
    f << "Tesseract crash report\n"
      << "version: " << kVersion << "\n"
      << "platform: " << platform_name() << "\n"
      << "pid: " << current_pid() << "\n"
      << "started: " << utc_timestamp_basic() << "\n"
      << "---\n";
}

// backward::SignalHandling's internal Printer writes the trace differently
// per platform — confirmed by reading backward.hpp directly, since neither
// path takes a constructor parameter or other hook to redirect output:
//   - POSIX (Linux/macOS): printer.print(st, stderr) — the C runtime's
//     `stderr` FILE*.
//   - Windows: printer.print(st, std::cerr) — a *separate* C++ iostream,
//     independent of the C stderr FILE*. Retargeting fd 2 alone (as the
//     POSIX path needs) does nothing for this — std::cerr's streambuf has to
//     be redirected too, or a Windows crash's trace silently goes nowhere
//     (this app is a GUI-subsystem process with no console, so std::cerr's
//     original destination is likely invalid to begin with).
// Both are retargeted unconditionally below so the same code works
// regardless of which platform-specific SignalHandling variant is compiled;
// this happens once here, while the process is healthy — not inside the
// eventual signal handler — so it's safe. A side effect worth keeping
// rather than avoiding: any other stderr/cerr writes from this point on
// also land in the crash file, which usefully captures recent log context
// right before a crash.
void redirect_stderr_to(const fs::path& path)
{
#if defined(_WIN32)
    int fd = _fileno(stderr);
    g_saved_stderr_fd = _dup(fd);
    FILE* reopened = nullptr;
    (void)_wfreopen_s(&reopened, path.wstring().c_str(), L"a", stderr);
#else
    int fd = fileno(stderr);
    g_saved_stderr_fd = dup(fd);
    FILE* reopened = freopen(path.string().c_str(), "a", stderr);
    (void)reopened;
#endif

    g_cerr_crash_file.open(path, std::ios::app);
    if (g_cerr_crash_file.is_open())
    {
        g_saved_cerr_rdbuf = std::cerr.rdbuf(g_cerr_crash_file.rdbuf());
    }
}

void restore_stderr()
{
    if (g_saved_cerr_rdbuf)
    {
        std::cerr.rdbuf(g_saved_cerr_rdbuf);
        g_saved_cerr_rdbuf = nullptr;
    }
    g_cerr_crash_file.close();

    if (g_saved_stderr_fd < 0)
        return;
#if defined(_WIN32)
    _dup2(g_saved_stderr_fd, _fileno(stderr));
    _close(g_saved_stderr_fd);
#else
    dup2(g_saved_stderr_fd, fileno(stderr));
    close(g_saved_stderr_fd);
#endif
    g_saved_stderr_fd = -1;
}

} // namespace

void install_crash_handler(bool enabled)
{
    std::call_once(g_install_once, [enabled]
    {
        g_enabled.store(enabled, std::memory_order_relaxed);

        std::error_code ec;
        fs::create_directories(crash_dir(), ec);
        g_crash_file_path = compute_crash_file_path();
        write_metadata_header(g_crash_file_path);
        prune_old_crash_files();

        if (enabled)
            redirect_stderr_to(g_crash_file_path);

        // Process-lifetime: constructing this installs the platform fault
        // handlers (sigaction on POSIX, SetUnhandledExceptionFilter via
        // DbgHelp on Windows).
        static backward::SignalHandling sh;
        (void)sh;

        tesseract_ffi::install_rust_panic_hook(g_crash_file_path.string().c_str(), enabled);
    });
}

void set_crash_reporting_enabled(bool enabled)
{
    bool was_enabled = g_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled == was_enabled)
        return;

    if (enabled)
        redirect_stderr_to(g_crash_file_path);
    else
        restore_stderr();

    tesseract_ffi::set_rust_crash_reporting_enabled(enabled);
}

} // namespace tesseract

#else // !TESSERACT_CRASH_HANDLER_ENABLED

namespace tesseract
{

void install_crash_handler(bool) {}
void set_crash_reporting_enabled(bool) {}

} // namespace tesseract

#endif
