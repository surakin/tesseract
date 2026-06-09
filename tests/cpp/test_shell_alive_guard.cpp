#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double that queues UI-thread callbacks into an EXTERNAL
// vector (owned by the test, not the shell) so the queued continuations can be
// drained *after* the shell is destroyed. This exercises the liveness guard:
// a continuation enqueued via post_to_ui_alive_ must no-op once ~ShellBase has
// run, rather than dereferencing the freed shell.
struct WithAccountManager { tesseract::AccountManager am_; };

struct AliveShell : WithAccountManager, ShellBase
{
    explicit AliveShell(std::vector<std::function<void()>>* queue)
        : ShellBase(am_), queue_(queue)
    {
    }

    std::vector<std::function<void()>>* queue_;

    void post_to_ui_(std::function<void()> fn) override
    {
        queue_->push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue_->push_back(std::move(fn));
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
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    // Expose the guarded post helper to the test.
    using ShellBase::post_to_ui_alive_;
};

} // namespace

TEST_CASE("post_to_ui_alive_ continuation no-ops after the shell is destroyed",
          "[shell][alive]")
{
    std::vector<std::function<void()>> queue;
    bool ran = false;

    {
        AliveShell s(&queue);
        // Enqueue a continuation that captures the shell and would touch it.
        s.post_to_ui_alive_([&s, &ran]
                            {
                                // Touch the (would-be freed) shell to model a
                                // real continuation dereferencing `this`.
                                (void)&s;
                                ran = true;
                            });

        // The guard wraps the fn; it is queued, not yet run.
        REQUIRE(queue.size() == 1);
        REQUIRE_FALSE(ran);
    } // ~AliveShell runs here: sets *alive_ = false.

    // Drain the queue AFTER destruction. The guarded continuation must detect
    // the dead token and skip the body — without the guard this would
    // dereference the freed shell (use-after-free) and set `ran`.
    for (auto& fn : queue)
        if (fn) fn();

    CHECK_FALSE(ran);
}

TEST_CASE("post_to_ui_alive_ continuation runs while the shell is alive",
          "[shell][alive]")
{
    std::vector<std::function<void()>> queue;
    bool ran = false;

    AliveShell s(&queue);
    s.post_to_ui_alive_([&ran] { ran = true; });

    REQUIRE(queue.size() == 1);
    REQUIRE_FALSE(ran);

    // Drain while the shell is still alive: the body runs.
    for (auto& fn : queue)
        if (fn) fn();

    CHECK(ran);
}
