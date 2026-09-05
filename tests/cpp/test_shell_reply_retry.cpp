#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "views/RoomView.h"

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// retry_stale_reply_previews_() re-issues ensure_reply_details_() for any
// still-unresolved reply row whose in_reply_to_id now matches a newly-loaded
// event. This double checks the *selectivity* of that scan without going
// through the real Rust FFI: ensure_reply_details_() early-returns without
// touching reply_details_requested_ when current_room_id_ is empty, so
// flipping current_room_id_ to "" right before the call under test turns
// "was the guard entry erased and a re-fetch attempted" into a directly
// observable "is the entry gone afterward" — a re-fetch that actually ran
// would have found current_room_id_ empty and bailed before re-inserting.
struct ShellReplyRetryWithAccountManager
{
    tesseract::AccountManager am_;
};

struct ReplyRetryShell : ShellReplyRetryWithAccountManager, ShellBase
{
    ReplyRetryShell() : ShellBase(am_) {}

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
    void request_relogin_(const std::string&) override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override
    {
    }
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string&) override
    {
        return nullptr;
    }
    void install_account_notifier_(tesseract::AccountSession&) override {}
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::reply_details_requested_;
    using ShellBase::room_view_;
    using ShellBase::retry_stale_reply_previews_;
};

tesseract::views::MessageRowData
make_reply_row(const std::string& event_id, const std::string& in_reply_to_id,
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
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    s.room_view_ = &view;
    s.current_room_id_ = info.id;

    // A reply row whose quote target ("$missing") isn't loaded yet: the
    // first fetch attempt already ran and failed to resolve it, leaving the
    // dedup guard permanently set the way ensure_reply_details_ would after
    // a lost race with pagination.
    view.insert_message(0, make_reply_row("$reply1", "$missing", ""));
    s.reply_details_requested_.insert("$reply1");
    REQUIRE(s.reply_details_requested_.count("$reply1") == 1);

    // Flip current_room_id_ empty so the re-triggered ensure_reply_details_
    // call is a guaranteed no-op (see comment above) instead of reaching for
    // client_, which stays null in this test.
    s.current_room_id_.clear();
    s.retry_stale_reply_previews_({"$missing"});

    // The guard entry was cleared to allow a retry, and the retry itself
    // bailed out (empty current_room_id_) without re-inserting it — proving
    // retry_stale_reply_previews_ actually re-attempted the fetch rather
    // than leaving the stale guard in place.
    CHECK(s.reply_details_requested_.count("$reply1") == 0);
}

TEST_CASE("retry_stale_reply_previews_ (thread overload) retries an "
          "unresolved reply once its target arrives",
          "[shell][reply_retry]")
{
    // Exercises the general (list, room_id, thread_root, ids) overload used
    // by RoomPane::retry_stale_thread_reply_previews_ for an open thread
    // panel (main window or pop-out) instead of the 1-arg overload's
    // implicit room_view_->message_list()/current_room_id_.
    ReplyRetryShell s;
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    view.insert_message(0, make_reply_row("$reply1", "$missing", ""));
    s.reply_details_requested_.insert("$reply1");
    REQUIRE(s.reply_details_requested_.count("$reply1") == 1);

    // Empty room_id forces the re-triggered ensure_reply_details_ call to be
    // a guaranteed no-op (same early-return as current_room_id_ empty in the
    // test above) instead of reaching for client_, which stays null here.
    s.retry_stale_reply_previews_(view.message_list(), /*room_id=*/"",
                                  "$thread_root_event", {"$missing"});

    CHECK(s.reply_details_requested_.count("$reply1") == 0);
}

TEST_CASE("retry_stale_reply_previews_ leaves unrelated rows untouched",
          "[shell][reply_retry]")
{
    ReplyRetryShell s;
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    s.room_view_ = &view;
    s.current_room_id_ = info.id;

    // Unresolved, but quoting a *different* event than the one that just
    // arrived — must not be retried.
    view.insert_message(0, make_reply_row("$reply_other", "$other", ""));
    s.reply_details_requested_.insert("$reply_other");

    // Already resolved — must not be retried even though its target matches,
    // since re-fetching a resolved reply is pure waste.
    view.insert_message(1, make_reply_row("$reply_resolved", "$missing", "Bob"));
    s.reply_details_requested_.insert("$reply_resolved");

    s.current_room_id_.clear();
    s.retry_stale_reply_previews_({"$missing"});

    CHECK(s.reply_details_requested_.count("$reply_other") == 1);
    CHECK(s.reply_details_requested_.count("$reply_resolved") == 1);
}
