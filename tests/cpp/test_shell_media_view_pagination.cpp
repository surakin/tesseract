#include <catch2/catch_test_macros.hpp>

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

using tesseract::ShellBase;
using tesseract::views::MainAppWidget;
using tesseract::views::MessageRowData;

namespace
{

// A ShellBase test double exposing the room-media-gallery pagination
// plumbing so the retry-budget bookkeeping in on_media_view_load_older_ can
// be exercised without a window, canvas, or live session. The pure-virtual
// surface is stubbed to no-ops, mirroring PriorityShell in
// test_shell_media_priority.cpp.
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
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override { return nullptr; }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
#endif
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

    // Expose the protected room-media-gallery pagination plumbing under test.
    using ShellBase::client_;
    using ShellBase::close_room_media_view_;
    using ShellBase::handle_media_view_paginate_result_ui_;
    using ShellBase::kMediaViewMaxRenderGap;
    using ShellBase::kMediaViewMaxRetries;
    using ShellBase::kMediaViewMinTotal;
    using ShellBase::kMediaViewPauseFallbackMs;
    using ShellBase::main_app_;
    using ShellBase::maybe_resume_media_view_pagination_ui_;
    using ShellBase::media_view_known_media_count_;
    using ShellBase::media_view_paginate_pending_;
    using ShellBase::media_view_pending_request_id_;
    using ShellBase::media_view_retries_left_;
    using ShellBase::media_view_room_id_;
    using ShellBase::on_media_view_load_older_;
    using ShellBase::open_room_media_view_;
    using ShellBase::pagination_;
    using ShellBase::request_media_view_pagination_back_;
    using ShellBase::room_view_;
};

} // namespace

TEST_CASE(
    "on_media_view_load_older_ does not rearm retries while a round is in flight",
    "[shell][media-view]")
{
    // Reproduces the reported bug: opening the gallery in a media-sparse room
    // kicks off the automatic retry/accumulate chain in
    // handle_paginate_result_ui_. If the user scrolls (impatient, since
    // nothing visible has happened yet) while that chain's own round is still
    // in flight, on_media_view_load_older_ must not top the shared retry
    // budget back up to kMediaViewMaxRetries — doing so lets every such
    // scroll extend the chain well past its intended cap.
    MediaViewShell s;
    tesseract::Client client; // real, session-less — the FFI call is a no-op
    s.client_ = &client;

    const std::string room_id = "!room:example.org";
    s.media_view_room_id_ = room_id;
    s.media_view_retries_left_ = 1;
    s.pagination_[room_id].in_flight = true;

    s.on_media_view_load_older_(room_id);

    CHECK(s.media_view_retries_left_ == 1);
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
    s.media_view_room_id_ = room_id;
    s.media_view_retries_left_ = 0;

    s.on_media_view_load_older_(room_id);

    // Rearmed to kMediaViewMaxRetries, then one round fired (decrementing it).
    CHECK(s.media_view_retries_left_ == MediaViewShell::kMediaViewMaxRetries - 1);
    CHECK(s.pagination_[room_id].in_flight);
}

TEST_CASE("on_media_view_load_older_ ignores a stale room", "[shell][media-view]")
{
    MediaViewShell s;
    tesseract::Client client;
    s.client_ = &client;

    s.media_view_room_id_ = "!current:example.org";
    s.media_view_retries_left_ = 0;

    s.on_media_view_load_older_("!stale:example.org");

    CHECK(s.media_view_retries_left_ == 0);
}

namespace
{

// Sets up a MediaViewShell with an open gallery for room_id, ready for
// request_media_view_pagination_back_ / handle_media_view_paginate_result_ui_
// to be exercised directly.
struct GalleryFixture
{
    MediaViewShell s;
    tesseract::Client client; // real, session-less — the FFI call is a no-op
    MainAppWidget app;
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    std::string room_id = "!room:example.org";

    tk::LayoutCtx layout_ctx() { return {surface->factory(), tk::Theme::light()}; }

    GalleryFixture()
    {
        s.client_ = &client;
        s.main_app_ = &app;
        auto lc = layout_ctx();
        app.measure(lc, {800, 600});
        app.arrange(lc, {0, 0, 800, 600});
        app.room_media_view()->open(room_id, "Test Room");
        s.media_view_room_id_ = room_id;
        s.media_view_retries_left_ = MediaViewShell::kMediaViewMaxRetries;
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

    f.s.request_media_view_pagination_back_(f.room_id); // first round
    int rounds = 0;
    while (f.s.media_view_pending_request_id_ != 0 && rounds < 20)
    {
        ++rounds;
        auto req_id = f.s.media_view_pending_request_id_;
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

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);
    // The server reports the true start of history on this very first round.
    f.s.handle_media_view_paginate_result_ui_(req_id, true, true, 0, "");

    CHECK(f.s.media_view_pending_request_id_ == 0);
    CHECK(f.s.pagination_[f.room_id].reached_start);
}

TEST_CASE(
    "gallery pagination stops once the authoritative media_count reaches the "
    "threshold",
    "[shell][media-view]")
{
    GalleryFixture f;

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);

    // Reported directly from the SDK's timeline (see
    // paginate_media_view_back_async) — no widget content manipulation
    // needed; this is exactly the value the round would carry regardless of
    // whether the separate diff-streaming task has rendered anything yet.
    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal, "");

    CHECK(f.s.media_view_pending_request_id_ == 0);
}

TEST_CASE(
    "gallery pagination keeps going when media_count is just below the "
    "threshold",
    "[shell][media-view]")
{
    GalleryFixture f;

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);

    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal - 1, "");

    CHECK(f.s.media_view_pending_request_id_ != 0);
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

    MainAppWidget app;
    s.main_app_ = &app;
    auto surface = TestSurface::create(800, 600);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    app.measure(lc, {800, 600});
    app.arrange(lc, {0, 0, 800, 600}); // RoomMediaView stays invisible/unarranged

    tesseract::views::RoomView view;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);
    MessageRowData image_row;
    image_row.kind     = MessageRowData::Kind::Image;
    image_row.event_id = "$only-event";
    view.set_messages({image_row}, /*room_switch=*/true);
    s.room_view_ = &view;

    s.open_room_media_view_(info.id);

    // Only one media event is known, well below kMediaViewMinTotal — pagination
    // must have been kicked off to look for more, not silently skipped.
    CHECK(s.media_view_pending_request_id_ != 0);
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
    f.app.room_media_view()->arrange(lc, {0, 0, 800, 600});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMinTotal);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);

    // More than the old fixed floor, but still fewer than the real capacity.
    f.s.handle_media_view_paginate_result_ui_(
        req_id, true, false, MediaViewShell::kMediaViewMinTotal + 1, "");

    // Must still be going — six-plus is not "the viewport is full" once the
    // widget's real geometry says otherwise.
    CHECK(f.s.media_view_pending_request_id_ != 0);
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
    f.app.room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);

    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");

    // Deferred, not fired: no new request_id, but a fallback timer queued.
    CHECK(f.s.media_view_pending_request_id_ == 0);
    CHECK(f.s.media_view_paginate_pending_);
    CHECK(f.s.media_view_known_media_count_ == big_count);
    CHECK_FALSE(f.s.queue.empty());
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ui_ fires once the render gap has "
    "closed enough",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app.room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(f.s.media_view_paginate_pending_);
    REQUIRE(f.s.media_view_pending_request_id_ == 0);

    // Simulate the diff-streaming task delivering enough rows to close the
    // gap (item_count() now within kMediaViewMaxRenderGap of big_count).
    f.app.room_media_view()->set_media(
        make_image_rows(static_cast<int>(big_count - 1)));
    REQUIRE(f.app.room_media_view()->item_count() ==
            static_cast<std::size_t>(big_count - 1));

    f.s.maybe_resume_media_view_pagination_ui_(/*force=*/false);

    CHECK_FALSE(f.s.media_view_paginate_pending_);
    CHECK(f.s.media_view_pending_request_id_ != 0);
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ui_ is a no-op while the render gap "
    "is still too large",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app.room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(f.s.media_view_paginate_pending_);

    // No rows rendered at all yet — gap is still the full big_count.
    f.s.maybe_resume_media_view_pagination_ui_(/*force=*/false);

    CHECK(f.s.media_view_paginate_pending_);
    CHECK(f.s.media_view_pending_request_id_ == 0);
}

TEST_CASE(
    "maybe_resume_media_view_pagination_ui_ force=true fires regardless of "
    "the render gap (fallback-timer path)",
    "[shell][media-view]")
{
    GalleryFixture f;
    // Arrange a tall viewport so estimated_capacity() comfortably exceeds
    // big_count below — otherwise need_more would be false before the gap
    // logic is even reached (target defaults to the small kMediaViewMinTotal
    // floor while unarranged).
    auto tall_lc = f.layout_ctx();
    f.app.room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(f.s.media_view_paginate_pending_);

    f.s.maybe_resume_media_view_pagination_ui_(/*force=*/true);

    CHECK_FALSE(f.s.media_view_paginate_pending_);
    CHECK(f.s.media_view_pending_request_id_ != 0);
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
    f.app.room_media_view()->arrange(tall_lc, {0, 0, 800, 3000});
    REQUIRE(f.app.room_media_view()->estimated_capacity() >
            MediaViewShell::kMediaViewMaxRenderGap + 50);

    f.s.request_media_view_pagination_back_(f.room_id);
    auto req_id = f.s.media_view_pending_request_id_;
    REQUIRE(req_id != 0);
    const std::uint64_t big_count = MediaViewShell::kMediaViewMaxRenderGap + 50;
    f.s.handle_media_view_paginate_result_ui_(req_id, true, false, big_count, "");
    REQUIRE(f.s.media_view_paginate_pending_);
    REQUIRE_FALSE(f.s.queue.empty());
    auto stale_fallback = f.s.queue.back();

    f.s.close_room_media_view_();
    CHECK_FALSE(f.s.media_view_paginate_pending_);

    // The fallback timer's captured room_id no longer matches
    // media_view_room_id_ (cleared by close), so invoking the stale closure
    // must not re-fire pagination for a gallery that's no longer open.
    stale_fallback();
    CHECK(f.s.media_view_pending_request_id_ == 0);
    CHECK_FALSE(f.s.media_view_paginate_pending_);
}
