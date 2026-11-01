#pragma once

// Shared ShellBase test double. Supplies every pure virtual with a benign
// default so a test only overrides what it cares about; adding a hook to
// ShellBase means editing this one file instead of every test.
//
// Defaults: post_to_ui_* run inline when called on the constructing (test main)
// thread; posts from worker threads (pools, SDK callbacks) are queued and run
// on the main thread by drain_workers(), so "UI-thread" continuations never
// race the test body. Everything else is a no-op. A test that wants manually
// stepped UI work overrides post_to_ui_ in its own subclass (an override that
// runs fn inline reintroduces the cross-thread race).

#include "app/AccountManager.h"
#include "app/ShellBase.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tesseract::test
{

// Base-class-first holder: AccountManager must outlive ShellBase, and ShellBase
// takes a reference to it in its constructor.
struct AccountManagerHolder
{
    tesseract::AccountManager am_;
};

struct TestShellBase : AccountManagerHolder, tesseract::ShellBase
{
    TestShellBase() : ShellBase(am_)
    {
    }
    // Several windows sharing one AccountManager (multi-window tests); `shared`
    // must outlive this shell.
    explicit TestShellBase(tesseract::AccountManager& shared) : ShellBase(shared)
    {
    }

    // Workers post back into derived state; let them finish before it goes away.
    // Anything still queued is discarded, not run: the derived parts of the
    // shell are already destroyed by now, and the real app likewise drops posts
    // once the shell is gone (post_to_ui_alive_).
    ~TestShellBase() override
    {
        wait_pools_idle();
    }

    // Waits for the worker pools, then runs the UI continuations they queued on
    // the calling (UI) thread, repeating until both are quiet (a continuation
    // may start more work). Fixtures that own widgets the workers post into call
    // this before those widgets are destroyed (their members go before the
    // shell's destructor). Bounded so a bug cannot hang the suite.
    void drain_workers()
    {
        for (int i = 0; i < 1000; ++i)
        {
            wait_pools_idle();
            std::vector<std::function<void()>> batch;
            {
                std::lock_guard<std::mutex> lk(ui_queue_mu_);
                batch.swap(ui_queue_);
            }
            if (batch.empty())
            {
                return;
            }
            for (auto& fn : batch)
            {
                fn();
            }
        }
    }
    void settle()
    {
        drain_workers();
    }

    void post_to_ui_(std::function<void()> fn) override
    {
        post_(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        post_(std::move(fn));
    }
    void request_relayout_() override
    {
    }
    void request_repaint_() override
    {
    }
    void on_rooms_updated_() override
    {
    }
    void on_media_bytes_ready_(const tk::CacheKey&, MediaKind, std::vector<uint8_t>) override
    {
    }
    void on_tab_state_changed_ui_() override
    {
    }
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override
    {
        return 1000;
    }
    void start_anim_tick_() override
    {
    }
    void repaint_pickers_() override
    {
    }
    void navigate_to_room_(const std::string&) override
    {
    }
    void pick_image_file_(std::function<void(std::vector<uint8_t>, std::string)>) override
    {
    }
    void show_encryption_setup_overlay_(tesseract::views::EncryptionSetupOverlay::Mode) override
    {
    }
    void raise_and_activate_() override
    {
    }
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return nullptr;
    }
    tesseract::CallWindowBase* create_call_window_() override
    {
        return nullptr;
    }
    bool is_ctrl_held_() const override
    {
        return false;
    }
    void switch_active_account_(const std::string&) override
    {
    }
    void refresh_account_ui_after_switch_() override
    {
    }
    void bind_settings_controller_() override
    {
    }
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override
    {
    }
    std::unique_ptr<tesseract::IEventHandler> make_account_bridge_(const std::string&) override
    {
        return nullptr;
    }
    void install_account_notifier_(tesseract::AccountSession&) override
    {
    }
    void request_relogin_(const std::string&) override
    {
    }

    // Waits for the pools without running what they queued. For derived
    // destructors: the queued continuations may touch fixture state that is
    // already gone, so they are discarded rather than run.
    void wait_pools_idle()
    {
        const auto t = std::chrono::seconds(5);
        pool_.wait_idle(t);
        mut_pool_.wait_idle(t);
        media_prefetch_pool_.wait_idle(t);
    }

private:
    void post_(std::function<void()> fn)
    {
        if (std::this_thread::get_id() == ui_thread_)
        {
            fn();
            return;
        }
        std::lock_guard<std::mutex> lk(ui_queue_mu_);
        ui_queue_.push_back(std::move(fn));
    }

    const std::thread::id ui_thread_ = std::this_thread::get_id();
    std::mutex ui_queue_mu_;
    std::vector<std::function<void()>> ui_queue_;
};

} // namespace tesseract::test
