#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "views/RoomView.h"

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;

namespace tesseract
{

// Exposes exactly the private RoomPane state this test suite pokes directly,
// mirroring RoomPaneStickerTestAccess in test_shell_send_sticker.cpp (RoomPane
// is held by composition, not inherited, so the `using ShellBase::field;`
// trick the ShellBase test double below uses for its own protected members
// isn't available here).
struct RoomPaneReplyRetryTestAccess
{
    static views::RoomView*& room_view(RoomPane& p)
    {
        return p.room_view_;
    }
};

} // namespace tesseract

using tesseract::RoomPaneReplyRetryTestAccess;

namespace
{

// retry_stale_reply_previews_() re-issues ensure_reply_details_() for any
// still-unresolved reply row whose in_reply_to_id now matches a newly-loaded
// event. This double checks the *selectivity* of that scan without going
// through the real Rust FFI: ensure_reply_details_() early-returns without
// touching reply_details_requested_ when room_id is empty, so passing an
// empty room_id right before the call under test turns "was the guard entry
// erased and a re-fetch attempted" into a directly observable "is the entry
// gone afterward" — a re-fetch that actually ran would have found room_id
// empty and bailed before re-inserting.
struct ReplyRetryShell : tesseract::test::TestShellBase
{

    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override
    {
    }
    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }

    using ShellBase::reply_details_requested_;
    using ShellBase::reply_details_key_;
};

// Entries are keyed by (account, event id). These panes have no session, so
// their account is empty.
std::string key(const std::string& event_id)
{
    return ReplyRetryShell::reply_details_key_("", event_id);
}

std::unique_ptr<RoomPane> make_pane(ReplyRetryShell& s, const std::string& room_id)
{
    return std::make_unique<RoomPane>(
        RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}}, room_id);
}

tesseract::views::MessageRowData make_reply_row(const std::string& event_id,
                                                const std::string& in_reply_to_id,
                                                const std::string& in_reply_to_sender_name)
{
    tesseract::views::MessageRowData row;
    row.event_id = event_id;
    row.sender = "@alice:example.org";
    row.body = "hi";
    row.in_reply_to_id = in_reply_to_id;
    row.in_reply_to_sender_name = in_reply_to_sender_name;
    return row;
}

} // namespace

TEST_CASE("retry_stale_reply_previews_ retries an unresolved reply once its "
          "target arrives",
          "[shell][reply_retry]")
{
    ReplyRetryShell s;
    auto pane = make_pane(s, "!room:example.org");
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    RoomPaneReplyRetryTestAccess::room_view(*pane) = &view;

    // A reply row whose quote target ("$missing") isn't loaded yet: the
    // first fetch attempt already ran and failed to resolve it, leaving the
    // dedup guard permanently set the way ensure_reply_details_ would after
    // a lost race with pagination.
    view.insert_message(0, make_reply_row("$reply1", "$missing", ""));
    s.reply_details_requested_.insert(key("$reply1"));
    REQUIRE(s.reply_details_requested_.count(key("$reply1")) == 1);

    // Retrying against an empty room_id makes the re-triggered
    // ensure_reply_details_ call a guaranteed no-op (see comment above)
    // instead of reaching for client_, which stays null in this test.
    pane->retry_stale_reply_previews_(view.message_list(), /*room_id=*/"",
                                      /*thread_root=*/"", {"$missing"});

    // The guard entry was cleared to allow a retry, and the retry itself
    // bailed out (empty room_id) without re-inserting it — proving
    // retry_stale_reply_previews_ actually re-attempted the fetch rather
    // than leaving the stale guard in place.
    CHECK(s.reply_details_requested_.count(key("$reply1")) == 0);
}

TEST_CASE("retry_stale_reply_previews_ (thread overload) retries an "
          "unresolved reply once its target arrives",
          "[shell][reply_retry]")
{
    // Exercises the general (list, room_id, thread_root, ids) overload used
    // by RoomPane::retry_stale_thread_reply_previews_ for an open thread
    // panel (main window or pop-out) instead of the 1-arg overload's
    // implicit room_view_->message_list()/room_id_.
    ReplyRetryShell s;
    auto pane = make_pane(s, "!room:example.org");
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    view.insert_message(0, make_reply_row("$reply1", "$missing", ""));
    s.reply_details_requested_.insert(key("$reply1"));
    REQUIRE(s.reply_details_requested_.count(key("$reply1")) == 1);

    // Empty room_id forces the re-triggered ensure_reply_details_ call to be
    // a guaranteed no-op (same early-return as the empty-room_id case above)
    // instead of reaching for client_, which stays null here.
    pane->retry_stale_reply_previews_(view.message_list(), /*room_id=*/"", "$thread_root_event",
                                      {"$missing"});

    CHECK(s.reply_details_requested_.count(key("$reply1")) == 0);
}

TEST_CASE("retry_stale_reply_previews_ leaves unrelated rows untouched", "[shell][reply_retry]")
{
    ReplyRetryShell s;
    auto pane = make_pane(s, "!room:example.org");
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    RoomPaneReplyRetryTestAccess::room_view(*pane) = &view;

    // Unresolved, but quoting a *different* event than the one that just
    // arrived — must not be retried.
    view.insert_message(0, make_reply_row("$reply_other", "$other", ""));
    s.reply_details_requested_.insert(key("$reply_other"));

    // Already resolved — must not be retried even though its target matches,
    // since re-fetching a resolved reply is pure waste.
    view.insert_message(1, make_reply_row("$reply_resolved", "$missing", "Bob"));
    s.reply_details_requested_.insert(key("$reply_resolved"));

    pane->retry_stale_reply_previews_(view.message_list(), /*room_id=*/"",
                                      /*thread_root=*/"", {"$missing"});

    CHECK(s.reply_details_requested_.count(key("$reply_other")) == 1);
    CHECK(s.reply_details_requested_.count(key("$reply_resolved")) == 1);
}
