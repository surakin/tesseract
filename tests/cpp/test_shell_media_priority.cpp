#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/client.h>

#include <functional>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double exposing the media-request registry plumbing so the
// reverse-map (media key → request_id) lifecycle and the visible-row resolution
// can be exercised without a window, canvas, or live session. The pure-virtual
// surface is stubbed to no-ops.
struct ShellMediaPriorityWithAccountManager
{
    tesseract::AccountManager am_;
};

struct PriorityShell : ShellMediaPriorityWithAccountManager, ShellBase
{
    PriorityShell() : ShellBase(am_) {}

    void post_to_ui_(std::function<void()> fn) override { queue.push_back(std::move(fn)); }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
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
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override { return nullptr; }
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
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}

    std::vector<std::function<void()>> queue;

    // Expose the protected media-request plumbing under test.
    using ShellBase::begin_media_req_;
    using ShellBase::cancel_media_group_;
    using ShellBase::client_;
    using ShellBase::handle_media_ready_ui_;
    using ShellBase::media_key_to_req_;
    using ShellBase::resolve_visible_request_ids_;
};

// A no-op bytes sink for begin_media_req_.
auto noop_bytes = [](std::vector<std::uint8_t>&&) {};

} // namespace

TEST_CASE("begin_media_req_ registers its priority key; resolve finds the id",
          "[shell][media-priority]")
{
    PriorityShell s;
    auto id = s.begin_media_req_(/*group=*/7, noop_bytes, {}, "mxc://a");

    CHECK(s.resolve_visible_request_ids_({"mxc://a"}) ==
          std::vector<std::uint64_t>{id});
    // An unknown key resolves to nothing.
    CHECK(s.resolve_visible_request_ids_({"mxc://nope"}).empty());
}

TEST_CASE("begin_media_req_ with an empty priority key registers nothing",
          "[shell][media-priority]")
{
    PriorityShell s;
    s.begin_media_req_(/*group=*/7, noop_bytes); // no key
    CHECK(s.media_key_to_req_.empty());
    CHECK(s.resolve_visible_request_ids_({"mxc://a"}).empty());
}

TEST_CASE("completing a request drops its reverse-map entry",
          "[shell][media-priority]")
{
    PriorityShell s;
    auto id = s.begin_media_req_(/*group=*/7, noop_bytes, {}, "mxc://a");
    REQUIRE(s.resolve_visible_request_ids_({"mxc://a"}) ==
            std::vector<std::uint64_t>{id});

    s.handle_media_ready_ui_(id, {}); // delivery completes the request

    CHECK(s.media_key_to_req_.empty());
    CHECK(s.resolve_visible_request_ids_({"mxc://a"}).empty());
}

TEST_CASE("cancel_media_group_ drops only the cancelled group's keys",
          "[shell][media-priority]")
{
    PriorityShell s;
    tesseract::Client client; // real, session-less — the FFI call is a no-op
    s.client_ = &client;

    auto id1 = s.begin_media_req_(/*group=*/11, noop_bytes, {}, "mxc://k1");
    auto id2 = s.begin_media_req_(/*group=*/22, noop_bytes, {}, "mxc://k2");

    s.cancel_media_group_(11);

    CHECK(s.resolve_visible_request_ids_({"mxc://k1"}).empty());
    CHECK(s.resolve_visible_request_ids_({"mxc://k2"}) ==
          std::vector<std::uint64_t>{id2});
}

TEST_CASE("resolve_visible_request_ids_ keeps found keys and skips misses",
          "[shell][media-priority]")
{
    PriorityShell s;
    auto id1 = s.begin_media_req_(/*group=*/7, noop_bytes, {}, "mxc://k1");
    auto id3 = s.begin_media_req_(/*group=*/7, noop_bytes, {}, "mxc://k3");

    // Middle key was never requested (e.g. already cached) → skipped.
    auto ids = s.resolve_visible_request_ids_({"mxc://k1", "mxc://k2", "mxc://k3"});
    CHECK(ids == std::vector<std::uint64_t>{id1, id3});
}
