#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <functional>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double that queues UI-thread callbacks (rather than running
// them inline) and counts synchronous relayout passes, so a test can observe
// that schedule_relayout_ coalesces a burst of requests into a single pass.
struct CoalesceShell : ShellBase
{
    std::vector<std::function<void()>> queue;
    int relayout_calls = 0;

    void post_to_ui_(std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
    void request_relayout_() override
    {
        ++relayout_calls;
    }
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
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::schedule_relayout_;

    void drain()
    {
        auto pending = std::move(queue);
        queue.clear();
        for (auto& fn : pending)
        {
            if (fn)
                fn();
        }
    }
};

} // namespace

TEST_CASE("schedule_relayout_ coalesces a burst into one relayout pass",
          "[shell][relayout]")
{
    CoalesceShell s;

    // A burst of per-message relayout requests within one event-loop batch.
    s.schedule_relayout_();
    s.schedule_relayout_();
    s.schedule_relayout_();

    // Only one flush is queued, and no synchronous relayout has run yet.
    CHECK(s.queue.size() == 1);
    CHECK(s.relayout_calls == 0);

    // Draining the queue runs exactly one relayout pass.
    s.drain();
    CHECK(s.relayout_calls == 1);
}

TEST_CASE("schedule_relayout_ re-arms after the flush runs",
          "[shell][relayout]")
{
    CoalesceShell s;

    s.schedule_relayout_();
    REQUIRE(s.queue.size() == 1);
    s.drain();
    REQUIRE(s.relayout_calls == 1);

    // A later request (next batch) schedules a fresh flush.
    s.schedule_relayout_();
    CHECK(s.queue.size() == 1);
    s.drain();
    CHECK(s.relayout_calls == 2);
}
