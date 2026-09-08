#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;

namespace
{

// A ShellBase test double exposing pagination_ — the shared, room-id-keyed
// MSC3030 state RoomPane::on_timeline_reset now reads to re-assert the
// focused-timeline/return-to-live scroll restore for ANY pane (main window
// or pop-out), not just the main window. Mirrors PriorityShell in
// test_shell_media_priority.cpp.
struct PopoutTimelineResetShellWithAccountManager
{
    tesseract::AccountManager am_;
};

struct PopoutTimelineResetShell : PopoutTimelineResetShellWithAccountManager,
                                  ShellBase
{
    PopoutTimelineResetShell() : ShellBase(am_) {}

    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void post_to_ui_after_(int, std::function<void()> fn) override { fn(); }
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

    using ShellBase::current_room_id_;
    using ShellBase::pagination_;
};

// Mirrors TkListsStage in test_tk_lists.cpp — drives a real measure/arrange/
// paint pass so row_offsets_/content_height()/scroll_y() reflect genuine
// layout instead of default-constructed zeros.
struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

MessageRowData make_text_row(const std::string& id)
{
    MessageRowData m{};
    m.kind = MessageRowData::Kind::Text;
    m.event_id = id;
    m.sender_name = "Alice";
    m.body = "hi";
    m.timestamp_ms = 1700000000000ull;
    return m;
}

std::vector<MessageRowData> make_rows(int count)
{
    std::vector<MessageRowData> rows;
    rows.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        rows.push_back(make_text_row("$row" + std::to_string(i)));
    return rows;
}

} // namespace

TEST_CASE(
    "RoomPane::on_timeline_reset re-asserts a pop-out's OWN focused-timeline "
    "scroll state, not just the main window's",
    "[shell][popout][timeline-reset]")
{
    // Regression coverage for the MSC3030 completion-side gap: previously
    // only ShellBase::handle_timeline_reset_ui_'s main-window-only branch
    // read pagination_[room_id] and drove begin_focused_gate/
    // set_historical_mode/scroll_to_event_id — a pop-out (routed instead
    // through RoomPane::on_timeline_reset via dispatch_timeline_reset_secondary_)
    // never got any of it, even though its own MSC3030 initiation (moved onto
    // RoomPane earlier) already writes into this same shared, room-id-keyed
    // pagination_ map.
    PopoutTimelineResetShell s;
    const std::string popout_room = "!popout:example.org";
    // Deliberately NOT current_room_id_ — proves this now works for a pane
    // that isn't the main window's, the actual point of this fix.
    REQUIRE(s.current_room_id_.empty());

    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = popout_room;
    view.set_room(info);

    Stage stage;
    const tk::Rect bounds{0, 0, 400, 600};

    RoomPane pane(
        RoomPane::Deps{
            .shell = &s,
            .repaint = [] {},
            .relayout = [&] { stage.run(*view.message_list(), bounds); },
        },
        popout_room);
    pane.attach(RoomPane::Widgets{.room_view = &view});

    // Enough rows to overflow the 600px viewport, with the target near the
    // TOP: a fresh room display's natural default already pins near the
    // bottom (newest messages), so a target row that would already be
    // visible there (e.g. one near the end) can't distinguish "scroll_to_
    // event_id actually ran" from "the default happened to already show it".
    // A near-top target forces a real upward scroll only scroll_to_event_id
    // would produce.
    auto rows = make_rows(40);
    const std::string target_id = rows[2].event_id;

    auto& pstate = s.pagination_[popout_room];
    pstate.is_focused = true;
    pstate.focus_event_id = target_id;

    pane.on_timeline_reset(std::move(rows));

    MessageListView* list = view.message_list();
    REQUIRE(list != nullptr);
    CHECK(list->historical_mode_for_test());
    // Scrolled up toward the near-top target, away from the natural
    // bottom-pinned default for a fresh (non-focused) room display.
    const float max_scroll = std::max(0.0f, list->content_height() - bounds.h);
    REQUIRE(max_scroll > 0.0f); // sanity: content genuinely overflows
    CHECK(list->scroll_y() < max_scroll);
}

TEST_CASE(
    "RoomPane::on_timeline_reset scrolls a pop-out to the bottom and clears "
    "returning_to_live once consumed",
    "[shell][popout][timeline-reset]")
{
    PopoutTimelineResetShell s;
    const std::string popout_room = "!popout:example.org";

    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = popout_room;
    view.set_room(info);

    Stage stage;
    const tk::Rect bounds{0, 0, 400, 600};

    RoomPane pane(
        RoomPane::Deps{
            .shell = &s,
            .repaint = [] {},
            .relayout = [&] { stage.run(*view.message_list(), bounds); },
        },
        popout_room);
    pane.attach(RoomPane::Widgets{.room_view = &view});

    auto& pstate = s.pagination_[popout_room];
    pstate.returning_to_live = true;

    pane.on_timeline_reset(make_rows(40));

    MessageListView* list = view.message_list();
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->historical_mode_for_test());
    const float max_scroll = std::max(0.0f, list->content_height() - bounds.h);
    REQUIRE(max_scroll > 0.0f);
    CHECK(list->scroll_y() == max_scroll);

    // Consumed: a later reset must not re-trigger the bottom snap from a
    // stale flag.
    CHECK_FALSE(s.pagination_[popout_room].returning_to_live);
}

TEST_CASE(
    "RoomPane::on_timeline_reset leaves historical mode off for an ordinary "
    "(non-focused, non-returning) reset",
    "[shell][popout][timeline-reset]")
{
    PopoutTimelineResetShell s;
    const std::string popout_room = "!popout:example.org";

    auto view_owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = popout_room;
    view.set_room(info);

    Stage stage;
    const tk::Rect bounds{0, 0, 400, 600};

    RoomPane pane(
        RoomPane::Deps{
            .shell = &s,
            .repaint = [] {},
            .relayout = [&] { stage.run(*view.message_list(), bounds); },
        },
        popout_room);
    pane.attach(RoomPane::Widgets{.room_view = &view});

    pane.on_timeline_reset(make_rows(10));

    MessageListView* list = view.message_list();
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->historical_mode_for_test());
}
