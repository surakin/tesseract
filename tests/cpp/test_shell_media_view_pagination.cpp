#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MainAppWidget.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/client.h>

#include <functional>
#include <string>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;
using tesseract::views::MainAppWidget;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;

namespace tesseract
{

// Exposes exactly the private RoomPane gallery-pagination state this test
// suite pokes directly. RoomPane is held by composition (not inherited), so
// the `using ShellBase::field;` trick the ShellBase test double below uses
// for its own protected members isn't available here — this friend struct
// (declared as a friend inside RoomPane itself) is the equivalent for a
// composed, non-subclassed collaborator.
struct RoomPaneMediaViewTestAccess
{
    static std::string& media_view_room_id(RoomPane& p)
    {
        return p.media_view_room_id_;
    }
    static std::uint64_t& media_view_pending_request_id(RoomPane& p)
    {
        return p.media_view_pending_request_id_;
    }
    static int& media_view_retries_left(RoomPane& p)
    {
        return p.media_view_retries_left_;
    }
    static bool& media_view_paginate_pending(RoomPane& p)
    {
        return p.media_view_paginate_pending_;
    }
    static std::uint64_t& media_view_known_media_count(RoomPane& p)
    {
        return p.media_view_known_media_count_;
    }
    static RoomView*& room_view(RoomPane& p) { return p.room_view_; }
    static RoomPane::Widgets& widgets(RoomPane& p) { return p.widgets_; }
};

} // namespace tesseract

using tesseract::RoomPaneMediaViewTestAccess;

namespace
{

// A ShellBase test double exposing the shared pagination_/pending_paginates_/
// client_ state a RoomPane's gallery-pagination methods read/write via
// shell_->..., plus handle_media_view_paginate_result_ui_ (the real
// production routing entry point: looks up the owning RoomPane in
// media_view_paginate_owners_ and forwards to it). The pure-virtual surface
// is stubbed to no-ops, mirroring PriorityShell in test_shell_media_priority.cpp.
struct MediaViewShellWithAccountManager
{
    tesseract::AccountManager am_;
};

struct MediaViewShell : MediaViewShellWithAccountManager, ShellBase
{
    MediaViewShell() : ShellBase(am_) {}

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

    // Expose the protected room-media-gallery plumbing under test.
    using ShellBase::client_;
    using ShellBase::handle_media_view_paginate_result_ui_;
    using ShellBase::kMediaViewMaxRenderGap;
    using ShellBase::kMediaViewMaxRetries;
    using ShellBase::kMediaViewMinTotal;
    using ShellBase::kMediaViewPauseFallbackMs;
    using ShellBase::main_app_;
    using ShellBase::pagination_;
};

// Builds a RoomPane bound to `s`, with no widgets attached — enough for the
// on_media_view_load_older_ / request_media_view_pagination_back_ /
// handle_media_view_paginate_result_ui_ retry-budget plumbing, which never
// touches room_view_/widgets_.room_media_view directly.
std::unique_ptr<RoomPane> make_pane(MediaViewShell& s,
                                    const std::string& room_id)
{
    return std::make_unique<RoomPane>(
        RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}},
        room_id);
}

} // namespace

TEST_CASE(
    "on_media_view_load_older_ does not rearm retries while a round is in flight",
    "[shell][media-view]")
{
    // Reproduces the reported bug: opening the gallery in a media-sparse room
    // kicks off the automatic retry/accumulate chain in
    // handle_media_view_paginate_result_. If the user scrolls (impatient,
    // since nothing visible has happened yet) while that chain's own round is
    // still in flight, on_media_view_load_older_ must not top the shared
    // retry budget back up to kMediaViewMaxRetries — doing so lets every such
    // scroll extend the chain well past its intended cap.
    MediaViewShell s;
    tesseract::Client client; // real, session-less — the FFI call is a no-op
    s.client_ = &client;

    const std::string room_id = "!room:example.org";
    auto pane = make_pane(s, room_id);
    RoomPaneMediaViewTestAccess::media_view_room_id(*pane) = room_id;
    RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) = 1;
    s.pagination_[room_id].in_flight = true;

    pane->on_media_view_load_older_(room_id);

    CHECK(RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) == 1);
}

TEST_CASE(
    "on_media_view_load_older_ rearms retries for a genuine new gesture",
    "[shell][media-view]")
{
    // Companion to the above: once the automatic chain has actually finished
    // (no round in flight) and exhausted its budget, a later real scroll
    // gesture must still be able to kick off a fresh batch of retries.
    MediaViewShell s;
    tesseract::Client client;
    s.client_ = &client;

    const std::string room_id = "!room:example.org";
    auto pane = make_pane(s, room_id);
    RoomPaneMediaViewTestAccess::media_view_room_id(*pane) = room_id;
    RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) = 0;

    pane->on_media_view_load_older_(room_id);

    // Rearmed to kMediaViewMaxRetries, then one round fired (decrementing it).
    CHECK(RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) ==
          MediaViewShell::kMediaViewMaxRetries - 1);
    CHECK(s.pagination_[room_id].in_flight);
}

TEST_CASE("on_media_view_load_older_ ignores a stale room", "[shell][media-view]")
{
    MediaViewShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto pane = make_pane(s, "!current:example.org");
    RoomPaneMediaViewTestAccess::media_view_room_id(*pane) = "!current:example.org";
    RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) = 0;

    pane->on_media_view_load_older_("!stale:example.org");

    CHECK(RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) == 0);
}

namespace
{

// Sets up a MediaViewShell + RoomPane with an open gallery for room_id,
// ready for request_media_view_pagination_back_ /
// handle_media_view_paginate_result_ui_ to be exercised directly.
struct GalleryFixture
{
    MediaViewShell s;
    tesseract::Client client; // real, session-less — the FFI call is a no-op
    std::unique_ptr<MainAppWidget> app = tk::create_root_widget<MainAppWidget>(nullptr);
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    std::string room_id = "!room:example.org";
    std::unique_ptr<RoomPane> pane;

    tk::LayoutCtx layout_ctx() { return {surface->factory(), tk::Theme::light()}; }

    GalleryFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        auto lc = layout_ctx();
        app->measure(lc, {800, 600});
        app->arrange(lc, {0, 0, 800, 600});
        app->room_media_view()->open(room_id, "Test Room");

        pane = make_pane(s, room_id);
        RoomPaneMediaViewTestAccess::widgets(*pane).room_media_view =
            app->room_media_view();
        RoomPaneMediaViewTestAccess::media_view_room_id(*pane) = room_id;
        RoomPaneMediaViewTestAccess::media_view_retries_left(*pane) =
            MediaViewShell::kMediaViewMaxRetries;
    }
};

} // namespace

TEST_CASE(
    "gallery pagination keeps requesting past the old 4-round cap while "
    "media_count stays below the threshold and history isn't reached",
    "[shell][media-view]")
{
    // Reproduces the reported bug: a media-sparse-relative-to-volume room
    // (lots of media overall, but none in the next few hundred raw events)
    // needed many manual scroll gestures to make any progress, because each
    // gesture was hard-capped at 4 rounds regardless of the authoritative
    // media count.
    GalleryFixture f;

    f.pane->request_media_view_pagination_back_(); // first round
    int rounds = 0;
    while (RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) != 0 &&
           rounds < 20)
    {
        ++rounds;
        auto req_id =
            RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
        // ok, no reached_start, media_count stays at 0 every round.
        f.s.handle_media_view_paginate_result_ui_(req_id, true, false, 0, "");
    }

    CHECK(rounds > 4);
}

TEST_CASE(
    "gallery pagination stops once reached_start is reported",
    "[shell][media-view]")
{
    GalleryFixture f;

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);
    // The server reports the true start of history on this very first round.
    f.s.handle_media_view_paginate_result_ui_(req_id, true, true, 0, "");

    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);
    CHECK(f.s.pagination_[f.room_id].reached_start);
}

TEST_CASE(
    "gallery pagination stops once the authoritative media_count reaches the "
    "threshold",
    "[shell][media-view]")
{
    GalleryFixture f;

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);

    // Reported directly from the SDK's timeline (see
    // paginate_media_view_back_async) — no widget content manipulation
    // needed; this is exactly the value the round would carry regardless of
    // whether the separate diff-streaming task has rendered anything yet.
    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal, "");

    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);
}

TEST_CASE(
    "gallery pagination keeps going when media_count is just below the "
    "threshold",
    "[shell][media-view]")
{
    GalleryFixture f;

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);

    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal - 1, "");

    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) != 0);
}

TEST_CASE(
    "open_room_media_view_ kicks off pagination on first-ever open even "
    "though the never-arranged gallery widget has zero bounds",
    "[shell][media-view]")
{
    // Regression test: RoomMediaView starts invisible (tk::Widget::arrange's
    // default child recursion skips invisible children), so the first time
    // the gallery is ever opened in a session, rmv->open() makes it visible
    // but it has not yet received an arrange() pass of its own — its bounds_
    // is still the default-constructed {0,0,0,0}. A kickoff check based on
    // content_fills_viewport() (content_height() >= bounds().h) is trivially
    // true against a zero-height viewport regardless of how little content
    // exists, so it must not be used here — only item_count() is reliable at
    // this point in the sequence.
    MediaViewShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto app_owner = tk::create_root_widget<MainAppWidget>(nullptr);
    MainAppWidget& app = *app_owner;
    s.main_app_ = &app;
    auto surface = TestSurface::create(800, 600);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    app.measure(lc, {800, 600});
    app.arrange(lc, {0, 0, 800, 600}); // RoomMediaView stays invisible/unarranged

    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);
    MessageRowData image_row;
    image_row.kind     = MessageRowData::Kind::Image;
    image_row.event_id = "$only-event";
    view.set_messages({image_row}, /*room_switch=*/true);

    auto pane = make_pane(s, info.id);
    RoomPaneMediaViewTestAccess::room_view(*pane) = &view;
    RoomPaneMediaViewTestAccess::widgets(*pane).room_media_view =
        app.room_media_view();

    pane->open_room_media_view_();

    // Only one media event is known, well below kMediaViewMinTotal — pagination
    // must have been kicked off to look for more, not silently skipped.
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*pane) != 0);
    CHECK(s.pagination_[info.id].in_flight);
}

TEST_CASE(
    "gallery pagination keeps going past kMediaViewMinTotal once the "
    "widget's real (larger) capacity is known",
    "[shell][media-view]")
{
    // Reproduces the "found six, stopped" bug: once RoomMediaView has been
    // arranged with its real viewport, the retry loop's target must be
    // estimated_capacity() (which a normal-sized window comfortably exceeds
    // kMediaViewMinTotal for), not the small fixed floor.
    GalleryFixture f;
    auto lc = f.layout_ctx();
    f.app->room_media_view()->arrange(lc, {0, 0, 800, 600});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMinTotal);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);

    // More than the old fixed floor, but still fewer than the real capacity.
    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal + 1, "");

    // Must still be going — six-plus is not "the viewport is full" once the
    // widget's real geometry says otherwise.
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) != 0);
}

namespace
{

std::vector<MessageRowData> make_image_rows(int count)
{
    std::vector<MessageRowData> rows;
    for (int i = 0; i < count; ++i)
    {
        MessageRowData row;
        row.kind     = MessageRowData::Kind::Image;
        row.event_id = "$ev" + std::to_string(i);
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

TEST_CASE(
    "gallery pagination defers the next round when rendering is far behind "
    "the authoritative count",
    "[shell][media-view]")
{
    // Reproduces the "huge bunch" bug: the widget has rendered nothing yet,
    // but the round reports a media_count far beyond kMediaViewMaxRenderGap
    // ahead of it — firing immediately would let dozens more rounds queue
    // raw events for the slow diff-streaming task before it renders any of
    // this round's finds.
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app->room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);

    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");

    // Deferred, not fired: no new request_id, but a fallback timer queued.
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);
    CHECK(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    CHECK(RoomPaneMediaViewTestAccess::media_view_known_media_count(*f.pane) ==
          big_count);
    CHECK_FALSE(f.s.queue.empty());
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ fires once the render gap has "
    "closed enough",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app->room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    REQUIRE(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);

    // Simulate the diff-streaming task delivering enough rows to close the
    // gap (item_count() now within kMediaViewMaxRenderGap of big_count).
    f.app->room_media_view()->set_media(
        make_image_rows(static_cast<int>(big_count - 1)));
    REQUIRE(f.app->room_media_view()->item_count() ==
            static_cast<std::size_t>(big_count - 1));

    f.pane->maybe_resume_media_view_pagination_(/*force=*/false);

    CHECK_FALSE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) != 0);
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ is a no-op while the render gap "
    "is still too large",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app->room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));

    // No rows rendered at all yet — gap is still the full big_count.
    f.pane->maybe_resume_media_view_pagination_(/*force=*/false);

    CHECK(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ force=true fires regardless of "
    "the render gap (fallback-timer path)",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app->room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));

    f.pane->maybe_resume_media_view_pagination_(/*force=*/true);

    CHECK_FALSE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) != 0);
}

TEST_CASE(
    "closing the gallery clears a pending deferred round; a stale fallback "
    "closure is a no-op afterward",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app->room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app->room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.pane->request_media_view_pagination_back_();
    auto req_id =
        RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane);
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
    REQUIRE_FALSE(f.s.queue.empty());
    auto stale_fallback = f.s.queue.back();

    f.pane->close_room_media_view_();
    CHECK_FALSE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));

    // The fallback timer's captured target_room no longer matches
    // media_view_room_id_ (cleared by close), so invoking the stale closure
    // must not re-fire pagination for a gallery that's no longer open.
    stale_fallback();
    CHECK(RoomPaneMediaViewTestAccess::media_view_pending_request_id(*f.pane) == 0);
    CHECK_FALSE(RoomPaneMediaViewTestAccess::media_view_paginate_pending(*f.pane));
}
