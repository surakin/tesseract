// RoomPane.cpp: the callbacks wire_room_view_ installs on RoomView (send /
// reply / edit / delete / react / poll / receipts / pin / DM / moderation /
// knock / thread panel / pagination / focused-subscription / date jump / search)
// and the pane-owned compose-draft cache. Each test fires the same std::function
// the widget would and asserts the pane / shell state it produces. Client has no
// session, so nothing reaches a network.

#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/ComposeBar.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "views/ThreadView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;
using tesseract::views::MessageRowData;

namespace
{

struct RpShell : tesseract::test::TestShellBase
{
    ~RpShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        main_room_pane_.reset();
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void request_repaint_() override { ++repaints; }
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }

    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::string> statuses;
    int repaints = 0;

    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::dm_in_flight_user_ids_;
    using ShellBase::in_room_search_active_rv_;
    using ShellBase::in_room_search_room_id_;
    using ShellBase::known_users_building_;
    using ShellBase::last_sent_receipt_;
    using ShellBase::last_sent_thread_receipt_;
    using ShellBase::main_room_pane_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::next_paginate_id_;
    using ShellBase::pagination_;
    using ShellBase::pending_matrix_link_;
    using ShellBase::pending_moderations_;
    using ShellBase::pending_paginates_;
    using ShellBase::pending_room_actions_;
    using ShellBase::knock_requests_panel_room_id_;
    using ShellBase::rooms_;
    using ShellBase::pending_scroll_room_event_id_;
};

struct RpFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    RpShell s;
    std::unique_ptr<tesseract::views::RoomView> view =
        tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    int relayouts = 0;
    int repaints = 0;

    explicit RpFx(bool with_client = true)
    {
        tesseract::RoomInfo info;
        info.id = "!r:x";
        info.name = "R";
        view->set_room(info);
        s.rooms_ = {info};
        s.mark_room_index_dirty_();
        s.current_room_id_ = "!r:x";
        s.my_user_id_ = "@me:x";
        if (with_client)
        {
            s.client_ = &client;
            s.active_account_ = std::make_shared<tesseract::AccountSession>();
            s.active_account_->user_id = "@me:x";
            s.active_account_->client = std::make_unique<tesseract::Client>();
        }
        s.main_room_pane_ = std::make_unique<RoomPane>(
            RoomPane::Deps{.shell = &s,
                           .repaint = [this] { ++repaints; },
                           .relayout = [this] { ++relayouts; }},
            "!r:x");
        s.main_room_pane_->attach({.room_view = view.get()});
    }
    RoomPane& pane() { return *s.main_room_pane_; }
    tesseract::views::MessageListView* ml() { return view->message_list(); }
};

MessageRowData rp_row(const std::string& id)
{
    MessageRowData m;
    m.event_id = id;
    return m;
}

} // namespace

TEST_CASE("sending: blank text is ignored, real text clears the composer",
          "[roompane][compose]")
{
    RpFx f;
    f.view->set_current_text("   ");
    f.view->on_send("   ");
    CHECK(f.view->compose_bar()->current_text() == "   "); // untouched

    f.view->set_current_text("hello");
    f.view->on_send("hello");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.s.pump();
}

TEST_CASE("replying and editing clear the composer only for real content",
          "[roompane][compose]")
{
    RpFx f;
    f.view->set_current_text("draft");
    f.view->on_send_reply("$e", "");
    CHECK(f.view->compose_bar()->current_text() == "draft");
    f.view->on_send_reply("$e", "answer");
    CHECK(f.view->compose_bar()->current_text().empty());

    f.view->set_current_text("draft");
    f.view->on_send_edit("$e", "", false);
    CHECK(f.view->compose_bar()->current_text() == "draft");
    f.view->on_send_edit("$e", "fixed", false);
    CHECK(f.view->compose_bar()->current_text().empty());
    f.view->set_current_text("draft");
    f.view->on_send_edit("$e", "", true); // a caption edit may be empty
    CHECK(f.view->compose_bar()->current_text().empty());
    f.s.pump();
}

TEST_CASE("edit prefill and cancel drive the composer text", "[roompane][compose]")
{
    RpFx f;
    f.view->on_edit_prefill("old text");
    CHECK(f.view->compose_bar()->current_text() == "old text");
    f.view->on_edit_cancelled();
    CHECK(f.view->compose_bar()->current_text().empty());
}

TEST_CASE("a failed edit reports on the status line", "[roompane][compose]")
{
    RpFx f;
    f.view->on_send_reply("$e", "answer");
    f.view->on_send_edit("$e", "fixed", false);
    f.s.pump();
    bool edit_failed = false;
    for (const auto& st : f.s.statuses)
        edit_failed |= st.rfind("Edit failed", 0) == 0;
    CHECK(edit_failed);
}

TEST_CASE("sends do nothing without a client", "[roompane][compose]")
{
    RpFx f(/*with_client=*/false);
    f.view->set_current_text("draft");
    f.view->on_send_reply("$e", "answer");
    f.view->on_send_edit("$e", "x", false);
    f.view->on_delete_requested("$e");
    f.view->on_reaction_toggled("$e", "+", "");
    f.view->on_poll_vote("$e", {"a"});
    f.view->on_poll_end_requested("$e");
    f.view->on_pin_requested("$e");
    f.view->on_unpin_requested("$e");
    f.view->on_copy_event_source_requested("$e");
    f.view->on_scroll_to_original("$e");
    f.view->on_date_jump(1000);
    f.view->on_open_dm("@u:x");
    f.s.pump();
    CHECK(f.s.statuses.empty());
    CHECK(f.s.pending_paginates_.empty());
}

TEST_CASE("event actions run against the pane's room", "[roompane][actions]")
{
    RpFx f;
    f.view->on_delete_requested("");
    f.view->on_delete_requested("$e");
    f.view->on_reaction_toggled("", "+", "");
    f.view->on_reaction_toggled("$e", "+", "");
    f.view->on_reaction_toggled("$e", "mxc://hs/x", "mxc://hs/x");
    f.view->on_poll_vote("$e", {"a", "b"});
    f.view->on_poll_end_requested("$e");
    f.view->on_pin_requested("$e");
    f.view->on_unpin_requested("$e");
    f.view->on_pin_requested("");
    f.view->on_copy_event_source_requested("");
    f.s.pump();
    SUCCEED();
}

TEST_CASE("read receipts from the visible range are deduplicated", "[roompane][receipts]")
{
    RpFx f;
    f.view->on_receipt_needed("$a");
    f.view->on_receipt_needed("$a");
    CHECK(f.s.last_sent_receipt_.at("!r:x") == "$a");
    f.view->on_receipt_needed("$b");
    CHECK(f.s.last_sent_receipt_.at("!r:x") == "$b");
    f.view->on_thread_receipt_needed("$t1"); // no thread open: ignored
    CHECK(f.s.last_sent_thread_receipt_.empty());
    f.s.pump();
}

TEST_CASE("scrolling near the top paginates once and honours guards",
          "[roompane][paginate]")
{
    RpFx f;
    f.view->on_near_top();
    CHECK(f.s.pagination_.at("!r:x").in_flight);
    f.view->on_near_top(); // already in flight
    f.s.pump();            // sessionless result clears the flag
    CHECK_FALSE(f.s.pagination_.at("!r:x").in_flight);

    f.s.pagination_["!r:x"].reached_start = true;
    f.view->on_near_top();
    CHECK_FALSE(f.s.pagination_.at("!r:x").in_flight);
}

TEST_CASE("forward history only loads inside a focused timeline",
          "[roompane][paginate]")
{
    RpFx f;
    f.view->on_near_bottom();
    CHECK(f.s.pending_paginates_.empty());

    f.view->on_scroll_to_original("$old"); // enters a focused timeline
    REQUIRE(f.s.pagination_.at("!r:x").is_focused);
    CHECK(f.s.pagination_.at("!r:x").focus_event_id == "$old");

    f.view->on_near_bottom();
    CHECK(f.s.pagination_.at("!r:x").fwd_in_flight);
    REQUIRE(f.s.pending_paginates_.size() == 1);
    CHECK_FALSE(f.s.pending_paginates_.begin()->second.second); // forward
    f.view->on_near_bottom(); // already in flight
    CHECK(f.s.pending_paginates_.size() == 1);

    f.view->on_return_to_live();
    CHECK_FALSE(f.s.pagination_.at("!r:x").is_focused);
    CHECK(f.s.pagination_.at("!r:x").returning_to_live);
    CHECK(f.s.pagination_.at("!r:x").in_flight);
    f.s.pump(); // subscribes, then asks for a backward page
    CHECK_FALSE(f.s.pending_paginates_.empty());
}

TEST_CASE("return to live clears the pending permalink scroll for the shown room",
          "[roompane][paginate]")
{
    RpFx f;
    f.s.pending_scroll_room_event_id_ = "$stale";
    f.ml()->set_pending_scroll_event_id("$stale");
    f.view->on_return_to_live();
    CHECK(f.s.pending_scroll_room_event_id_.empty());
}

TEST_CASE("jumping to the first unread uses the focused-timeline path",
          "[roompane][unread]")
{
    RpFx f;
    f.view->on_jump_to_unread("");
    CHECK(f.s.pagination_.count("!r:x") == 0);
    f.view->on_jump_to_unread("$fully_read");
    CHECK(f.ml()->highlighted_event() == "$fully_read");
    CHECK(f.s.pagination_.at("!r:x").is_focused);
    f.s.pump();
}

TEST_CASE("a date jump failure is reported", "[roompane][date]")
{
    RpFx f;
    f.view->on_date_jump(1'700'000'000'000ull);
    f.s.pump();
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back().rfind("Jump to date failed", 0) == 0);

    f.view->on_date_picker_opened();
    f.s.pump();
}

TEST_CASE("the thread panel opens, switches and closes through RoomView",
          "[roompane][thread]")
{
    using TP = tesseract::ThreadPanelController::ThreadPanel;
    RpFx f;
    CHECK(f.pane().thread_panel() == TP::Closed);
    f.view->on_threads_button_clicked();
    CHECK(f.pane().thread_panel() == TP::List);
    f.view->on_thread_open_requested("$root");
    CHECK(f.pane().thread_panel() == TP::Open);
    CHECK(f.pane().thread_root() == "$root");
    // The thread's root isn't loaded: the pane asks for its context.
    CHECK(f.s.pagination_.at("!r:x").is_focused);

    f.view->on_thread_close_requested();
    CHECK(f.pane().thread_panel() != TP::Open);
    for (int i = 0; i < 3 && f.pane().thread_panel() != TP::Closed; ++i)
        f.view->on_threads_button_clicked();
    CHECK(f.pane().thread_panel() == TP::Closed);
    f.view->on_mark_all_read();
    f.s.pump();
}

TEST_CASE("thread sends need an open thread and clear the composer",
          "[roompane][thread]")
{
    RpFx f;
    f.view->set_current_text("reply");
    f.view->on_thread_send("reply", "");
    CHECK(f.view->compose_bar()->current_text() == "reply"); // no thread open

    f.view->on_thread_open_requested("$root");
    f.view->on_thread_send("reply", "");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.view->set_current_text("reply2");
    f.view->on_thread_send_reply("", "reply2", ""); // no target
    CHECK(f.view->compose_bar()->current_text() == "reply2");
    f.view->on_thread_send_reply("$t1", "reply2", "");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.view->set_current_text("/poll");
    f.view->on_thread_send("/poll", "");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.s.pump();
}

TEST_CASE("opening a DM: existing, in flight, and failed creation", "[roompane][dm]")
{
    RpFx f;
    f.view->on_open_dm("");
    auto dm = tesseract::RoomInfo{};
    dm.id = "!dm";
    dm.is_direct = true;
    dm.dm_counterpart_user_id = "@known:x";
    f.s.rooms_.push_back(dm);
    f.s.mark_room_index_dirty_();
    CHECK(f.view->on_has_dm("@known:x"));
    CHECK_FALSE(f.view->on_has_dm("@other:x"));
    f.view->on_open_dm("@known:x"); // existing: tries to open its window

    f.view->on_open_dm("@new:x");
    CHECK(f.s.dm_in_flight_user_ids_.count("@new:x") == 1);
    f.view->on_open_dm("@new:x"); // in flight
    f.s.pump();
    CHECK(f.s.dm_in_flight_user_ids_.empty()); // creation failed offline
}

TEST_CASE("member moderation and knock actions register shell requests",
          "[roompane][moderation]")
{
    RpFx f;
    f.view->on_kick_member("!r:x", "@u:x", "U", "spam");
    f.view->on_ban_member("!r:x", "@u:x", "U", "");
    f.view->on_unban_member("!r:x", "@u:x", "U");
    CHECK(f.s.pending_moderations_.size() == 3);

    f.s.knock_requests_panel_room_id_ = "!r:x";
    f.view->on_knock_requests_closed();
    CHECK(f.s.knock_requests_panel_room_id_.empty());
    f.view->on_knock_requests_opened("!r:x");
    CHECK(f.s.knock_requests_panel_room_id_ == "!r:x");
    f.view->on_decline_and_ban_knock_request("!r:x", "@k:x", "no");
    f.view->on_knock_requests_closed();
}

TEST_CASE("room-info callbacks reach the shell", "[roompane][roominfo]")
{
    RpFx f;
    f.view->on_notification_mode_changed("!r:x", "mute");
    f.view->on_favourite_changed("!r:x", true);
    f.view->on_low_priority_changed("!r:x", true);
    f.view->on_save_topic("!r:x", "new topic");
    f.view->on_leave_room("!r:x");
    f.view->on_ignore_user("@u:x");
    f.view->on_fetch_notification_mode("!r:x");
    f.view->on_fetch_room_members("!r:x");
    f.view->on_room_info_opened("!r:x");
    f.view->on_room_settings_opened("!r:x");
    f.view->on_invite_dialog_opened("!r:x");
    CHECK(f.s.known_users_building_);
    f.s.pump();
    SUCCEED();
}

TEST_CASE("room-info callbacks degrade gracefully without a client",
          "[roompane][roominfo]")
{
    RpFx f(/*with_client=*/false);
    f.view->on_save_topic("!r:x", "t");
    f.view->on_leave_room("!r:x");
    f.view->on_ignore_user("@u:x");
    f.view->on_fetch_notification_mode("!r:x");
    f.view->on_fetch_room_members("!r:x");
    f.view->on_room_info_opened("!r:x");
    f.view->on_room_settings_opened("!r:x");
    SUCCEED();
}

TEST_CASE("sending media clears the composer and needs a room and client",
          "[roompane][media]")
{
    RpFx f;
    f.view->set_current_text("caption");
    // Animated images skip the host re-encode.
    f.view->on_send_image({1, 2, 3}, "image/gif", "a.gif", "cap", 10, 10, true, "");
    CHECK(f.view->compose_bar()->current_text().empty());

    f.view->set_current_text("caption");
    f.view->on_send_video({1}, "video/mp4", "v.mp4", "cap", 10, 10, {2}, 5, 5, 1000, "");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.view->set_current_text("caption");
    f.view->on_send_audio({1}, "audio/ogg", "a.ogg", "cap", 1000, "");
    CHECK(f.view->compose_bar()->current_text().empty());
    f.view->set_current_text("caption");
    f.view->on_send_file({1}, "text/plain", "a.txt", "cap", "");
    CHECK(f.view->compose_bar()->current_text().empty());

    RpFx none(/*with_client=*/false);
    none.view->set_current_text("caption");
    none.view->on_send_file({1}, "text/plain", "a.txt", "cap", "");
    none.view->on_send_audio({1}, "audio/ogg", "a.ogg", "cap", 1000, "");
    none.view->on_send_video({1}, "video/mp4", "v.mp4", "cap", 1, 1, {}, 0, 0, 0, "");
    none.view->on_send_image({1}, "image/gif", "a.gif", "", 1, 1, true, "");
    CHECK(none.view->compose_bar()->current_text() == "caption");
}

TEST_CASE("sticker picks send through the client and drop the reply",
          "[roompane][sticker]")
{
    RpFx f;
    tesseract::ImagePackImage img;
    img.shortcode = "wave";
    img.url = "mxc://hs/wave";
    f.view->on_sticker_picked(img);
    f.view->on_thread_open_requested("$root");
    f.view->on_sticker_picked(img); // thread form
    RpFx none(/*with_client=*/false);
    none.view->on_sticker_picked(img);
    SUCCEED();
}

TEST_CASE("visible rows and avatars trigger lazy media", "[roompane][media]")
{
    RpFx f;
    f.view->on_visible_avatars_changed({"mxc://hs/a1", "mxc://hs/a2"});
    f.view->on_visible_range_changed({}); // nothing laid out: no range
    f.view->on_member_pronoun_needed("@u:x");
    f.s.pump();
    SUCCEED();
}

TEST_CASE("in-room search queries mark this pane as the search target",
          "[roompane][search]")
{
    RpFx f;
    f.view->on_room_search_query("needle");
    CHECK(f.s.in_room_search_active_rv_ == f.view.get());
    CHECK(f.s.in_room_search_room_id_ == "!r:x");
    f.view->on_room_search_navigate(1);
    f.view->on_room_search_paginate_toggled(false);
    f.view->on_room_search_closed();
    CHECK(f.relayouts >= 1);
}

TEST_CASE("matrix links clicked in the timeline go through the shell",
          "[roompane][links]")
{
    RpFx f;
    f.s.rooms_.clear();
    f.s.mark_room_index_dirty_();
    f.view->on_link_clicked("https://matrix.to/#/!other:x"); // no rooms yet: held
    CHECK(f.s.pending_matrix_link_ == "https://matrix.to/#/!other:x");
}

TEST_CASE("identity warnings can be resolved", "[roompane][identity]")
{
    RpFx f;
    tesseract::IdentityWarning w;
    w.user_id = "@u:x";
    w.kind = tesseract::IdentityWarning::Kind::VerificationBroken;
    f.view->on_resolve_identity_warning(w);
    f.s.pump();
    CHECK_FALSE(f.s.statuses.empty()); // the failure is surfaced
    f.s.statuses.clear();
    w.kind = tesseract::IdentityWarning::Kind::Changed;
    f.view->on_resolve_identity_warning(w);
    f.s.pump();
    CHECK_FALSE(f.s.statuses.empty());
}

TEST_CASE("compose drafts are kept per room and replayed on return",
          "[roompane][draft]")
{
    RpFx f;
    f.view->set_current_text("half a thought");
    f.pane().save_compose_draft_("!r:x");
    f.view->set_current_text("");
    f.pane().apply_compose_draft_("!r:x");
    CHECK(f.view->compose_bar()->current_text() == "half a thought");

    // An empty composer drops the stored draft.
    f.view->set_current_text("");
    f.pane().save_compose_draft_("!r:x");
    f.view->set_current_text("x");
    f.pane().apply_compose_draft_("!r:x");
    CHECK(f.view->compose_bar()->current_text() == "x"); // nothing replayed

    f.pane().save_compose_draft_(""); // no room: ignored
    f.pane().apply_compose_draft_("!unknown:x");
    f.pane().clear_compose_drafts_();
}

TEST_CASE("unsent text is restored into an empty composer only", "[roompane][draft]")
{
    RpFx f;
    CHECK_FALSE(f.pane().restore_unsent_text_(""));
    CHECK(f.pane().restore_unsent_text_("lost message"));
    CHECK(f.view->compose_bar()->current_text() == "lost message");
    CHECK_FALSE(f.pane().restore_unsent_text_("another")); // composer in use

    f.pane().stash_unsent_draft_("!r:x", "stashed");
    f.pane().stash_unsent_draft_("!r:x", "second"); // first wins
    f.view->set_current_text("");
    f.pane().apply_compose_draft_("!r:x");
    CHECK(f.view->compose_bar()->current_text() == "stashed");
    f.pane().stash_unsent_draft_("", "x");
    f.pane().stash_unsent_draft_("!r:x", "");
}

TEST_CASE("retargeting resets per-room caches and refreshes send busy",
          "[roompane][retarget]")
{
    RpFx f;
    f.pane().retarget("!r:x"); // unchanged: nothing
    f.pane().retarget("!other:x");
    CHECK(f.pane().room_id() == "!other:x");
}
