// ShellBase_timeline.cpp: row building, pagination result bookkeeping, read
// receipts / deferred m.fully_read, the main-window timeline handlers
// (reset / insert / update / remove / prepend / append / batch update), typing
// and compose-typing notices, the animation tick gates and the concrete
// thread-view appliers.

#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "views/ThreadView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::MessageRowData;

namespace
{

struct TlShell : tesseract::test::TestShellBase
{
    ~TlShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        delayed.emplace_back(ms, std::move(fn));
    }
    void request_relayout_() override { ++relayouts; }
    void update_typing_bar_(const std::string& t, bool v) override
    {
        typing.emplace_back(t, v);
    }
    void stop_anim_tick_() override { ++anim_stops; }
    void repaint_anim_frame_() override { ++anim_repaints; }
    void stop_inflight_tick_() override { ++inflight_stops; }
    void on_rooms_updated_() override { ++rooms_updated; }
    void prep_row_media_(const tesseract::Event& ev, bool) override
    {
        prepped.push_back(ev.event_id);
    }

    void run_delayed()
    {
        auto pending = std::move(delayed);
        delayed.clear();
        for (auto& [ms, fn] : pending)
            fn();
    }

    std::vector<std::pair<int, std::function<void()>>> delayed;
    std::vector<std::pair<std::string, bool>> typing;
    std::vector<std::string> prepped;
    int relayouts = 0;
    int anim_stops = 0;
    int anim_repaints = 0;
    int inflight_stops = 0;
    int rooms_updated = 0;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::apply_thread_message_insert_;
    using ShellBase::apply_thread_message_remove_;
    using ShellBase::apply_thread_message_update_;
    using ShellBase::apply_thread_messages_;
    using ShellBase::apply_threads_list_;
    using ShellBase::build_rows_;
    using ShellBase::client_;
    using ShellBase::compose_typing_active_;
    using ShellBase::current_room_id_;
    using ShellBase::EventAccountScope;
    using ShellBase::flush_fully_read_;
    using ShellBase::flush_pending_fully_read_now_;
    using ShellBase::forget_thread_receipts_;
    using ShellBase::handle_compose_room_leaving_;
    using ShellBase::handle_compose_text_changed_;
    using ShellBase::handle_message_inserted_ui_;
    using ShellBase::handle_message_removed_ui_;
    using ShellBase::handle_message_updated_ui_;
    using ShellBase::handle_messages_appended_ui_;
    using ShellBase::handle_messages_prepended_ui_;
    using ShellBase::handle_messages_updated_batch_ui_;
    using ShellBase::handle_paginate_result_ui_;
    using ShellBase::handle_room_export_complete_ui_;
    using ShellBase::handle_room_export_progress_ui_;
    using ShellBase::handle_room_media_page_ui_;
    using ShellBase::handle_thread_inserted_ui_;
    using ShellBase::handle_thread_messages_appended_ui_;
    using ShellBase::handle_thread_messages_prepended_ui_;
    using ShellBase::handle_thread_removed_ui_;
    using ShellBase::handle_thread_reset_ui_;
    using ShellBase::handle_thread_updated_ui_;
    using ShellBase::handle_threads_updated_ui_;
    using ShellBase::handle_timeline_reset_ui_;
    using ShellBase::handle_typing_changed_ui_;
    using ShellBase::handle_voice_waveform_ready_ui_;
    using ShellBase::in_room_search_matches_;
    using ShellBase::in_room_search_pending_;
    using ShellBase::in_room_search_request_id_;
    using ShellBase::in_room_search_rerun_on_paginate_;
    using ShellBase::in_room_search_room_id_;
    using ShellBase::in_room_search_paginate_rerun_;
    using ShellBase::inflight_tick_;
    using ShellBase::kFullyReadLingerMs;
    using ShellBase::last_sent_receipt_;
    using ShellBase::other_account_pagination_;
    using ShellBase::last_sent_thread_receipt_;
    using ShellBase::main_room_pane_;
    using ShellBase::maybe_send_read_receipt_;
    using ShellBase::maybe_send_thread_read_receipt_;
    using ShellBase::mark_all_threads_read_;
    using ShellBase::mark_room_read_;
    using ShellBase::media_prepped_event_ids_;
    using ShellBase::my_user_id_;
    using ShellBase::notify_fully_read_moved_;
    using ShellBase::pagination_;
    using ShellBase::pending_fully_read_;
    using ShellBase::pending_paginates_;
    using ShellBase::pending_scroll_room_event_id_;
    using ShellBase::per_account_rooms_;
    using ShellBase::push_paginate_result_;
    using ShellBase::room_is_shown_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::tick_anim_;
};

std::unique_ptr<tesseract::Event> tl_ev(const std::string& id,
                                     const std::string& thread = "",
                                     const std::string& reply_to = "")
{
    auto e = std::make_unique<tesseract::Event>();
    e->event_id = id;
    e->sender = "@a:x";
    e->body = "body " + id;
    e->type = tesseract::EventType::Text;
    e->thread_root_id = thread;
    e->in_reply_to_id = reply_to;
    return e;
}

tesseract::EventList tl_list(std::initializer_list<const char*> ids)
{
    tesseract::EventList l;
    for (auto* i : ids)
        l.push_back(tl_ev(i));
    return l;
}

struct TlMainFixture
{
    TlShell s;
    tesseract::Client client;
    std::unique_ptr<tesseract::views::RoomView> view =
        tk::create_root_widget<tesseract::views::RoomView>(nullptr);

    TlMainFixture()
    {
        tesseract::RoomInfo info;
        info.id = "!r:x";
        info.name = "R";
        view->set_room(info);
        s.client_ = &client;
        s.room_view_ = view.get();
        s.current_room_id_ = "!r:x";
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.my_user_id_ = "@me:x";
        s.main_room_pane_ = std::make_unique<tesseract::RoomPane>(
            tesseract::RoomPane::Deps{
                .shell = &s, .repaint = [] {}, .relayout = [] {}},
            "!r:x");
        s.main_room_pane_->attach({.room_view = view.get()});
    }
    tesseract::views::MessageListView* ml() { return view->message_list(); }
    std::vector<std::string> ids()
    {
        std::vector<std::string> v;
        for (const auto& m : ml()->messages())
            v.push_back(m.event_id);
        return v;
    }
};

} // namespace

TEST_CASE("build_rows_ skips null events and preps media for the tail only",
          "[shell][timeline]")
{
    TlMainFixture f;
    tesseract::EventList snap;
    snap.push_back(tl_ev("$a"));
    snap.push_back(nullptr);
    snap.push_back(tl_ev("$b", "", "$a")); // reply
    auto rows = f.s.build_rows_(snap, "!r:x");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].event_id == "$a");
    CHECK(rows[1].event_id == "$b");
    CHECK(f.s.media_prepped_event_ids_.count("$a") == 1);
    CHECK(f.s.media_prepped_event_ids_.count("$b") == 1);

    // Raw-pointer overload behaves the same.
    auto a = tl_ev("$p");
    std::vector<tesseract::Event*> raw{a.get(), nullptr};
    auto rows2 = f.s.build_rows_(raw, "!r:x");
    REQUIRE(rows2.size() == 1);
    CHECK(rows2[0].event_id == "$p");
}

TEST_CASE("push_paginate_result_ clears in-flight and records history end",
          "[shell][timeline][paginate]")
{
    TlMainFixture f;
    f.s.pagination_["!r:x"].in_flight = true;
    f.view->set_paginating(true);
    f.s.push_paginate_result_("!r:x", true, "");
    CHECK_FALSE(f.s.pagination_["!r:x"].in_flight);
    CHECK(f.s.pagination_["!r:x"].reached_start);
    CHECK_FALSE(f.ml()->paginating());

    // Another account's result must not flip the active view.
    f.view->set_paginating(true);
    f.s.push_paginate_result_("!r:x", false, "@other:x");
    CHECK(f.ml()->paginating());
    CHECK(f.s.other_account_pagination_.at("@other:x").at("!r:x").in_flight ==
          false);
}

TEST_CASE("handle_paginate_result_ui_: unknown id is ignored, backward result "
          "updates state",
          "[shell][timeline][paginate]")
{
    TlMainFixture f;
    f.s.handle_paginate_result_ui_(5, true, true, false, "");
    CHECK(f.s.pagination_.count("!r:x") == 0);

    f.s.pending_paginates_[5] = {"!r:x", true};
    f.s.pagination_["!r:x"].in_flight = true;
    f.view->set_paginating(true);
    f.s.handle_paginate_result_ui_(5, true, true, false, "");
    CHECK(f.s.pending_paginates_.empty());
    CHECK_FALSE(f.s.pagination_["!r:x"].in_flight);
    CHECK(f.s.pagination_["!r:x"].reached_start);
    CHECK_FALSE(f.ml()->paginating());

    // A failed paginate keeps reached_start untouched.
    f.s.pending_paginates_[6] = {"!r:x", true};
    f.s.pagination_["!r:x"].reached_start = false;
    f.s.handle_paginate_result_ui_(6, false, true, false, "boom");
    CHECK_FALSE(f.s.pagination_["!r:x"].reached_start);
}

TEST_CASE("handle_paginate_result_ui_: forward result records the live end",
          "[shell][timeline][paginate]")
{
    TlMainFixture f;
    f.s.pending_paginates_[7] = {"!r:x", false};
    f.s.pagination_["!r:x"].fwd_in_flight = true;
    f.s.handle_paginate_result_ui_(7, true, false, false, "");
    CHECK_FALSE(f.s.pagination_["!r:x"].fwd_in_flight);
    CHECK_FALSE(f.s.pagination_["!r:x"].reached_end);

    f.s.pending_paginates_[8] = {"!r:x", false};
    f.s.handle_paginate_result_ui_(8, true, false, true, "");
    // Reaching the live end snaps back to the live timeline.
    CHECK(f.s.pagination_["!r:x"].returning_to_live);
    CHECK_FALSE(f.s.pagination_["!r:x"].reached_end);
}

TEST_CASE("handle_paginate_result_ui_ re-runs an in-room search that drove it",
          "[shell][timeline][paginate]")
{
    TlMainFixture f;
    f.view->open_room_search();
    REQUIRE(f.view->room_search_open());
    f.view->room_search_bar()->set_query("needle");
    f.s.in_room_search_room_id_ = "!r:x";
    f.s.in_room_search_rerun_on_paginate_ = true;
    f.s.in_room_search_matches_.resize(2);
    f.s.pending_paginates_[9] = {"!r:x", true};
    f.s.handle_paginate_result_ui_(9, true, false, false, "");
    CHECK_FALSE(f.s.in_room_search_rerun_on_paginate_);
    CHECK(f.s.in_room_search_paginate_rerun_);
    CHECK(f.s.in_room_search_request_id_ == 1);
    REQUIRE(f.s.in_room_search_pending_.size() == 1);
    CHECK(f.s.in_room_search_pending_.at(1) == "needle");
}

TEST_CASE("read receipts are deduplicated and arm one deferred fully-read",
          "[shell][timeline][receipt]")
{
    TlMainFixture f;
    f.s.maybe_send_read_receipt_("", "$e");
    f.s.maybe_send_read_receipt_("!r:x", "");
    CHECK(f.s.last_sent_receipt_.empty());

    f.s.maybe_send_read_receipt_("!r:x", "$e1");
    CHECK(f.s.last_sent_receipt_.at("!r:x") == "$e1");
    REQUIRE(f.s.pending_fully_read_.size() == 1);
    CHECK(f.s.delayed.size() == 1);
    CHECK(f.s.delayed[0].first == TlShell::kFullyReadLingerMs);

    f.s.maybe_send_read_receipt_("!r:x", "$e1"); // duplicate: nothing
    CHECK(f.s.delayed.size() == 1);

    // A later receipt advances the target without re-arming the timer.
    f.s.maybe_send_read_receipt_("!r:x", "$e2");
    CHECK(f.s.delayed.size() == 1);
    CHECK(f.s.pending_fully_read_.begin()->second.event_id == "$e2");

    f.s.run_delayed(); // the linger timer flushes the marker
    CHECK(f.s.pending_fully_read_.empty());

    // Unknown key: no-op.
    f.s.flush_fully_read_("nope");
}

TEST_CASE("flush_pending_fully_read_now_ flushes only the named account",
          "[shell][timeline][receipt]")
{
    TlMainFixture f;
    auto other = std::make_shared<tesseract::AccountSession>();
    other->user_id = "@other:x";
    f.s.maybe_send_read_receipt_("!r:x", "$e1");
    f.s.maybe_send_read_receipt_("!r2:x", "$e2", other);
    REQUIRE(f.s.pending_fully_read_.size() == 2);

    f.s.flush_pending_fully_read_now_("@other:x");
    REQUIRE(f.s.pending_fully_read_.size() == 1);
    CHECK(f.s.pending_fully_read_.begin()->first.rfind("@me:x", 0) == 0);

    f.s.flush_pending_fully_read_now_();
    CHECK(f.s.pending_fully_read_.empty());
}

TEST_CASE("room_is_shown_ matches the active room for the active account only",
          "[shell][timeline][receipt]")
{
    TlMainFixture f;
    CHECK(f.s.room_is_shown_("@me:x", "!r:x"));
    CHECK_FALSE(f.s.room_is_shown_("@other:x", "!r:x"));
    CHECK_FALSE(f.s.room_is_shown_("@me:x", "!elsewhere:x"));
    CHECK_FALSE(f.s.room_is_shown_("@me:x", ""));
    f.s.notify_fully_read_moved_("@me:x", "!r:x"); // forwards to the pane
    f.s.notify_fully_read_moved_("@x:x", "!r:x");
}

TEST_CASE("thread read receipts dedupe per thread and can be forgotten",
          "[shell][timeline][receipt]")
{
    TlMainFixture f;
    f.s.maybe_send_thread_read_receipt_("", "$t", "$e");
    f.s.maybe_send_thread_read_receipt_("!r:x", "", "$e");
    f.s.maybe_send_thread_read_receipt_("!r:x", "$t", "");
    CHECK(f.s.last_sent_thread_receipt_.empty());

    f.s.maybe_send_thread_read_receipt_("!r:x", "$t1", "$e1");
    f.s.maybe_send_thread_read_receipt_("!r:x", "$t2", "$e2");
    f.s.maybe_send_thread_read_receipt_("!r2:x", "$t1", "$e3");
    f.s.maybe_send_thread_read_receipt_("!r:x", "$t1", "$e1");
    CHECK(f.s.last_sent_thread_receipt_.size() == 3);

    f.s.forget_thread_receipts_("!r:x");
    CHECK(f.s.last_sent_thread_receipt_.size() == 1);
    CHECK(f.s.last_sent_thread_receipt_.count("!r2:x\x1F$t1") == 1);

    f.s.mark_all_threads_read_("");
    f.s.mark_all_threads_read_("!r:x");
}

TEST_CASE("mark_room_read_ zeroes counters and drops the deferred marker",
          "[shell][timeline][receipt]")
{
    TlMainFixture f;
    tesseract::RoomInfo r;
    r.id = "!other:x";
    r.notification_count = 4;
    r.highlight_count = 2;
    f.s.rooms_ = {r};
    f.s.per_account_rooms_["@me:x"] = {r};
    f.s.pending_fully_read_["@me:x\x1F!other:x"] = {};

    f.s.mark_room_read_("");
    CHECK(f.s.rooms_updated == 0);
    f.s.mark_room_read_("!other:x");
    CHECK(f.s.rooms_[0].notification_count == 0);
    CHECK(f.s.rooms_[0].highlight_count == 0);
    CHECK(f.s.per_account_rooms_["@me:x"][0].notification_count == 0);
    CHECK(f.s.rooms_updated == 1);
    // Not the shown room: the pending fully-read move is discarded.
    CHECK(f.s.pending_fully_read_.empty());
}

TEST_CASE("voice waveform only lands in the shown room", "[shell][timeline]")
{
    TlMainFixture f;
    f.s.handle_voice_waveform_ready_ui_("!other:x", "$e", {1, 2, 3});
    f.s.handle_voice_waveform_ready_ui_("!r:x", "$e", {1, 2, 3});
    SUCCEED();
}

TEST_CASE("tick_anim_ stops with nothing animated and runs while paginating",
          "[shell][timeline][anim]")
{
    TlMainFixture f;
    CHECK_FALSE(f.s.tick_anim_());
    CHECK(f.s.anim_stops == 1);
    CHECK(f.s.anim_repaints == 0);

    f.view->set_paginating(true);
    CHECK(f.s.tick_anim_());
    CHECK(f.s.anim_repaints == 1);

    CHECK_FALSE(f.s.inflight_tick_()); // no in-flight sends
    CHECK(f.s.inflight_stops == 1);
}

TEST_CASE("timeline reset populates the shown room and ignores others",
          "[shell][timeline][reset]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!other:x", tl_list({"$x"}));
    CHECK(f.ids().empty());

    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a", "$b", "$c"}));
    CHECK(f.ids() == std::vector<std::string>{"$a", "$b", "$c"});

    // The pending permalink scroll is re-armed after a reset.
    f.s.pending_scroll_room_event_id_ = "$b";
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a", "$b", "$c", "$d"}));
    CHECK(f.ids().size() == 4);
}

TEST_CASE("insert / update / remove keep the main list in step",
          "[shell][timeline][edit]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a", "$c"}));
    f.s.handle_message_inserted_ui_("!r:x", 1, tl_ev("$b"));
    CHECK(f.ids() == std::vector<std::string>{"$a", "$b", "$c"});

    auto edited = tl_ev("$b");
    edited->body = "edited";
    f.s.handle_message_updated_ui_("!r:x", 1, std::move(edited));
    CHECK(f.ml()->messages()[1].body == "edited");

    f.s.handle_message_removed_ui_("!r:x", 0);
    CHECK(f.ids() == std::vector<std::string>{"$b", "$c"});

    // Other rooms, null events and in-thread replies never touch the list.
    f.s.handle_message_removed_ui_("!other:x", 0);
    f.s.handle_message_updated_ui_("!other:x", 0, tl_ev("$z"));
    f.s.handle_message_updated_ui_("!r:x", 0, nullptr);
    f.s.handle_message_updated_ui_("!r:x", 0, tl_ev("$t", "$root"));
    auto unhandled = tl_ev("$u");
    unhandled->type = tesseract::EventType::Unhandled;
    f.s.handle_message_updated_ui_("!r:x", 0, std::move(unhandled));
    CHECK(f.ids() == std::vector<std::string>{"$b", "$c"});
    CHECK(f.ml()->messages()[0].body == "edited");
}

TEST_CASE("reply updates resolve the quote and mark the reply ready",
          "[shell][timeline][edit]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a"}));
    auto reply = tl_ev("$a", "", "$orig");
    reply->in_reply_to_sender_name = "Alice";
    f.s.handle_message_updated_ui_("!r:x", 0, std::move(reply));
    CHECK(f.ids() == std::vector<std::string>{"$a"});
}

TEST_CASE("prepend and append batches add rows in order", "[shell][timeline][batch]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$m"}));
    f.s.handle_messages_prepended_ui_("!r:x", tl_list({"$o1", "$o2"}));
    CHECK(f.ids() == std::vector<std::string>{"$o1", "$o2", "$m"});

    f.s.handle_messages_appended_ui_("!r:x", tl_list({"$n1", "$n2"}));
    CHECK(f.ids() ==
          std::vector<std::string>{"$o1", "$o2", "$m", "$n1", "$n2"});

    // In-thread batches and unrelated rooms are ignored.
    tesseract::EventList thread;
    thread.push_back(tl_ev("$t", "$root"));
    f.s.handle_messages_appended_ui_("!r:x", std::move(thread));
    f.s.handle_messages_prepended_ui_("!other:x", tl_list({"$zz"}));
    CHECK(f.ids().size() == 5);
}

TEST_CASE("batch update rewrites only the named indices", "[shell][timeline][batch]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a", "$b", "$c"}));
    auto b = tl_ev("$b");
    b->body = "B2";
    auto c = tl_ev("$c");
    c->body = "C2";
    tesseract::EventList evs;
    evs.push_back(std::move(b));
    evs.push_back(std::move(c));
    f.s.handle_messages_updated_batch_ui_("!r:x", {1, 2}, std::move(evs));
    CHECK(f.ml()->messages()[0].body == "body $a");
    CHECK(f.ml()->messages()[1].body == "B2");
    CHECK(f.ml()->messages()[2].body == "C2");
}

TEST_CASE("another account's room events do not touch the active main list",
          "[shell][timeline][edit]")
{
    TlMainFixture f;
    f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$a"}));
    {
        TlShell::EventAccountScope scope(f.s, "@other:x");
        f.s.handle_timeline_reset_ui_("!r:x", tl_list({"$o1", "$o2"}));
        f.s.handle_messages_appended_ui_("!r:x", tl_list({"$o3"}));
        f.s.handle_message_removed_ui_("!r:x", 0);
    }
    CHECK(f.ids() == std::vector<std::string>{"$a"});
}

TEST_CASE("thread events only reach the thread view for the open thread",
          "[shell][timeline][thread]")
{
    TlMainFixture f;
    // No thread open: main_room_pane_->thread_root() is empty, so a thread
    // event for any root is dropped.
    f.s.handle_thread_inserted_ui_("!r:x", "$root", 0, tl_ev("$t1", "$root"));
    f.s.handle_thread_updated_ui_("!r:x", "$root", 0, tl_ev("$t1", "$root"));
    f.s.handle_thread_removed_ui_("!r:x", "$root", 0);
    f.s.handle_thread_messages_prepended_ui_("!r:x", "$root", tl_list({"$t1"}));
    f.s.handle_thread_messages_appended_ui_("!r:x", "$root", tl_list({"$t1"}));
    f.s.handle_thread_reset_ui_("!r:x", "$root", tl_list({"$t1"}));
    f.s.handle_thread_inserted_ui_("!r:x", "$root", 0, nullptr);
    f.s.handle_thread_updated_ui_("!r:x", "$root", 0, nullptr);
    const bool empty = f.view->thread_view() == nullptr ||
                       f.view->thread_view()->message_list()->messages().empty();
    CHECK(empty);
}

TEST_CASE("concrete thread appliers tolerate a missing thread view",
          "[shell][timeline][thread]")
{
    TlShell s;
    s.apply_thread_messages_("$r", {}, true);
    s.apply_thread_message_insert_("$r", 0, MessageRowData{});
    s.apply_thread_message_update_("$r", 0, MessageRowData{});
    s.apply_thread_message_remove_("$r", 0);
    s.apply_threads_list_({});
    CHECK(s.relayouts == 0);
}

TEST_CASE("apply_threads_list_ drives the header threads button",
          "[shell][timeline][thread]")
{
    TlMainFixture f;
    tesseract::ThreadInfo t;
    t.root_event_id = "$root";
    f.s.apply_threads_list_({t});
    f.s.apply_threads_list_({});
    SUCCEED();
}

TEST_CASE("threads-updated needs a client and tolerates a closed panel",
          "[shell][timeline][thread]")
{
    TlShell s;
    s.handle_threads_updated_ui_("!r:x"); // no client: no-op

    TlMainFixture f;
    f.s.handle_threads_updated_ui_("!other:x");
    f.s.handle_threads_updated_ui_("!r:x");
    SUCCEED();
}

TEST_CASE("typing notices format once and reach the typing bar only for the "
          "shown room",
          "[shell][timeline][typing]")
{
    TlMainFixture f;
    f.s.handle_typing_changed_ui_("!r:x", {"Alice"});
    REQUIRE(f.s.typing.size() == 1);
    CHECK(f.s.typing[0].second);
    CHECK(f.s.typing[0].first.find("Alice") != std::string::npos);

    f.s.handle_typing_changed_ui_("!r:x", {});
    REQUIRE(f.s.typing.size() == 2);
    CHECK_FALSE(f.s.typing[1].second);
    CHECK(f.s.typing[1].first.empty());

    f.s.handle_typing_changed_ui_("!other:x", {"Bob"});
    CHECK(f.s.typing.size() == 2);
}

TEST_CASE("compose typing state only changes on empty/non-empty edges",
          "[shell][timeline][typing]")
{
    TlMainFixture f;
    f.s.handle_compose_text_changed_("");
    CHECK_FALSE(f.s.compose_typing_active_);
    f.s.handle_compose_text_changed_("h");
    CHECK(f.s.compose_typing_active_);
    f.s.handle_compose_text_changed_("hi"); // still typing: no edge
    CHECK(f.s.compose_typing_active_);

    f.s.handle_compose_room_leaving_("");
    CHECK(f.s.compose_typing_active_);
    f.s.handle_compose_room_leaving_("!r:x");
    CHECK_FALSE(f.s.compose_typing_active_);
    f.s.handle_compose_room_leaving_("!r:x"); // already idle
    CHECK_FALSE(f.s.compose_typing_active_);

    // No current room: the state flips but no notice is queued.
    f.s.current_room_id_.clear();
    f.s.handle_compose_text_changed_("x");
    CHECK(f.s.compose_typing_active_);
}

TEST_CASE("export and gallery completions without a controller/owner are "
          "ignored",
          "[shell][timeline]")
{
    TlShell s;
    s.handle_room_export_progress_ui_({});
    s.handle_room_export_complete_ui_(1, true, false, false, "", 0, 0, "");
    s.handle_room_media_page_ui_(1, {}, true, 0);
    SUCCEED();
}
