#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include <tesseract/paths.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using tesseract::ShellBase;

namespace
{

// Redirects SessionStore's config/data dirs to an empty, private temp
// directory so restore_all_accounts_blocking_() sees a clean, empty account
// index — no network, no real matrix-sdk store, deterministic. Mirrors
// test_session_store.cpp's SessionFixture.
struct RestoreDirFixture
{
    std::string dir;
#if defined(__APPLE__)
    std::string saved_home;
#endif

    RestoreDirFixture()
    {
#if defined(_WIN32)
        const auto pid = static_cast<unsigned long>(_getpid());
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        dir = (fs::temp_directory_path() /
               ("tesseract_restore_async_test_" + std::to_string(pid)))
                  .string();
        fs::create_directories(dir);
#if defined(_WIN32)
        _putenv_s("APPDATA", dir.c_str());
#elif defined(__APPLE__)
        if (const char* h = std::getenv("HOME"))
        {
            saved_home = h;
        }
        setenv("HOME", dir.c_str(), 1);
#else
        const std::string cfg = (fs::path(dir) / "config").string();
        const std::string data = (fs::path(dir) / "data").string();
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        setenv("XDG_DATA_HOME", data.c_str(), 1);
#endif
    }

    ~RestoreDirFixture()
    {
#if defined(_WIN32)
        _putenv_s("APPDATA", "");
#elif defined(__APPLE__)
        if (saved_home.empty())
        {
            unsetenv("HOME");
        }
        else
        {
            setenv("HOME", saved_home.c_str(), 1);
        }
#else
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
#endif
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

struct RestoreAsyncShellAccountManager { tesseract::AccountManager am_; };

// A ShellBase double exercising restore_all_accounts_async_ end-to-end via a
// REAL mut_pool_ round trip — unlike the fully synthetic AliveShell /
// ForwardShell doubles used elsewhere, post_to_ui_ here queues into a
// mutex/condvar-guarded vector (not run inline), so the test can prove the
// completion is genuinely deferred through the worker thread rather than
// executed synchronously on the calling thread.
struct RestoreAsyncShell : RestoreAsyncShellAccountManager, ShellBase
{
    RestoreAsyncShell() : ShellBase(am_) {}

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::function<void()>> queue_;
    std::vector<std::string> progress_calls_;

    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(fn));
        cv_.notify_all();
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(fn));
        cv_.notify_all();
    }
    void on_startup_restore_progress_ui_(const std::string& status) override
    {
        progress_calls_.push_back(status);
    }

    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return nullptr;
    }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void refresh_account_ui_after_switch_() override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string&) override { return nullptr; }
    void install_account_notifier_(tesseract::AccountSession&) override {}
    void request_relogin_(const std::string&) override {}
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::restore_all_accounts_async_;
    using ShellBase::RestoreResult;

    // Wait (bounded) for the worker's post_to_ui_alive_ continuation to land
    // in queue_, then run it on this thread, standing in for the UI thread.
    void wait_and_drain(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, timeout, [&] { return !queue_.empty(); });
        auto pending = std::move(queue_);
        queue_.clear();
        lk.unlock();
        for (auto& fn : pending)
            if (fn) fn();
    }
};

} // namespace

TEST_CASE("restore_all_accounts_async_ defers the blocking restore off the "
          "calling thread and reports progress",
          "[shell][restore-async]")
{
    RestoreDirFixture dir_fixture;
    RestoreAsyncShell s;

    bool done_called = false;
    RestoreAsyncShell::RestoreResult result;

    s.restore_all_accounts_async_(
        [&](RestoreAsyncShell::RestoreResult r)
        {
            done_called = true;
            result = std::move(r);
        });

    // The initial progress notification and the function's own return happen
    // synchronously — before the worker thread has had any chance to run.
    REQUIRE(s.progress_calls_.size() == 1);
    CHECK_FALSE(s.progress_calls_[0].empty());
    CHECK_FALSE(done_called);

    // Wait for the real mut_pool_ round trip, then run the queued completion
    // on this thread (standing in for the UI thread).
    s.wait_and_drain(std::chrono::seconds(5));

    REQUIRE(done_called);
    CHECK_FALSE(result.any_accounts);
    CHECK_FALSE(result.any_restore_failed);
    REQUIRE(s.progress_calls_.size() == 2);
    CHECK(s.progress_calls_[1].empty()); // cleared right before `done`
}
