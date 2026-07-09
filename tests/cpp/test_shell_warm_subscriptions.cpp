#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <string>
#include <unordered_set>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellWarmSubscriptionsWithAccountManager { tesseract::AccountManager am_; };

struct ShellWarmSubscriptionsTestShell : ShellWarmSubscriptionsWithAccountManager, ShellBase
{
    ShellWarmSubscriptionsTestShell() : ShellBase(am_) {}

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
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::select_warm_evictions_;
    using ShellBase::touch_visited_room_;
    using ShellBase::visited_lru_;
    using ShellBase::kWarmRoomsMax;
    using ShellBase::current_room_id_;
    using ShellBase::pagination_;
    using ShellBase::prune_warm_subscriptions_;
    using ShellBase::parse_status_message_;
    using ShellBase::status_message_allows_links_;

    using ShellBase::mark_room_index_dirty_;
    using ShellBase::room_by_id_;
    using ShellBase::rooms_;

    // Replace the room list the way the production code does (wholesale), then
    // mark the index dirty so the next room_by_id_ rebuilds it lazily.
    void set_rooms_for_test(std::vector<tesseract::RoomInfo> rs)
    {
        rooms_ = std::move(rs);
        mark_room_index_dirty_();
    }
};

tesseract::RoomInfo room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    return r;
}

} // namespace

TEST_CASE("status messages are not linkified unless explicitly opted in",
          "[shell][status_links]")
{
    ShellWarmSubscriptionsTestShell s;
    const std::string msg = "failed: see [click here](https://evil.example)";

    // Default (server/error text): no linkification — a single plain segment.
    auto plain = s.parse_status_message_(msg);
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].url.empty());
    CHECK(plain[0].text == msg);

    // Opted in (app-authored text): markdown links are parsed.
    s.status_message_allows_links_ = true;
    auto linked = s.parse_status_message_(msg);
    CHECK(tesseract::status_has_links(linked));
}

TEST_CASE("room_by_id_ returns the matching room or nullptr",
          "[shell][room_index]")
{
    ShellWarmSubscriptionsTestShell s;
    s.set_rooms_for_test({room("!a:x"), room("!b:x"), room("!c:x")});

    const auto* b = s.room_by_id_("!b:x");
    REQUIRE(b != nullptr);
    CHECK(b->id == "!b:x");
    CHECK(s.room_by_id_("!missing:x") == nullptr);
}

TEST_CASE("room_by_id_ reflects a wholesale room-list replacement",
          "[shell][room_index]")
{
    ShellWarmSubscriptionsTestShell s;
    s.set_rooms_for_test({room("!a:x"), room("!b:x")});
    REQUIRE(s.room_by_id_("!a:x") != nullptr);

    // The list is replaced (account switch / rooms update) — the old ids must
    // disappear and the new ones resolve.
    s.set_rooms_for_test({room("!c:x")});
    CHECK(s.room_by_id_("!a:x") == nullptr);
    CHECK(s.room_by_id_("!b:x") == nullptr);
    REQUIRE(s.room_by_id_("!c:x") != nullptr);
    CHECK(s.room_by_id_("!c:x")->id == "!c:x");
}

TEST_CASE("room_by_id_ is empty before any rooms are set", "[shell][room_index]")
{
    ShellWarmSubscriptionsTestShell s;
    CHECK(s.room_by_id_("!anything:x") == nullptr);
}

TEST_CASE("touch_visited_room_ moves a room to the MRU front without duplicates",
          "[shell][warm_subscriptions]")
{
    ShellWarmSubscriptionsTestShell s;
    s.touch_visited_room_("!a:x");
    s.touch_visited_room_("!b:x");
    s.touch_visited_room_("!a:x"); // re-visit A: moves to front, no dup

    REQUIRE(s.visited_lru_.size() == 2);
    CHECK(s.visited_lru_[0] == "!a:x");
    CHECK(s.visited_lru_[1] == "!b:x");
}

TEST_CASE("select_warm_evictions_ keeps newest warm rooms up to the cap",
          "[shell][warm_subscriptions]")
{
    ShellWarmSubscriptionsTestShell s;
    // Front = most recent. Active room E is protected; cap 2 warm.
    s.visited_lru_ = {"!e", "!d", "!c", "!b", "!a"};
    auto evicted = s.select_warm_evictions_({"!e"}, /*warm_cap=*/2);

    // Keep E (protected) + the two newest non-protected (D, C); evict B, A.
    REQUIRE(evicted.size() == 2);
    CHECK(evicted[0] == "!b");
    CHECK(evicted[1] == "!a");
    // visited_lru_ pruned to the retained set, order preserved.
    REQUIRE(s.visited_lru_.size() == 3);
    CHECK(s.visited_lru_[0] == "!e");
    CHECK(s.visited_lru_[1] == "!d");
    CHECK(s.visited_lru_[2] == "!c");
}

TEST_CASE("select_warm_evictions_ never evicts protected rooms even when old",
          "[shell][warm_subscriptions]")
{
    ShellWarmSubscriptionsTestShell s;
    // A is the oldest but is an open tab (protected); cap 1 warm.
    s.visited_lru_ = {"!e", "!d", "!c", "!b", "!a"};
    auto evicted = s.select_warm_evictions_({"!e", "!a"}, /*warm_cap=*/1);

    // Protected: E, A. Newest 1 non-protected: D. Evict C, B.
    REQUIRE(evicted.size() == 2);
    CHECK(evicted[0] == "!c");
    CHECK(evicted[1] == "!b");
    REQUIRE(s.visited_lru_.size() == 3);
    CHECK(s.visited_lru_[0] == "!e");
    CHECK(s.visited_lru_[1] == "!d");
    CHECK(s.visited_lru_[2] == "!a");
}

TEST_CASE("select_warm_evictions_ evicts nothing when within the cap",
          "[shell][warm_subscriptions]")
{
    ShellWarmSubscriptionsTestShell s;
    s.visited_lru_ = {"!c", "!b", "!a"};
    auto evicted = s.select_warm_evictions_({"!c"}, /*warm_cap=*/4);
    CHECK(evicted.empty());
    CHECK(s.visited_lru_.size() == 3);
}

TEST_CASE("prune_warm_subscriptions_ drops pagination_ state for evicted rooms",
          "[shell][warm_subscriptions]")
{
    // An evicted room's SDK timeline is torn down (unsubscribe_room), so its
    // stale pagination_ state (e.g. reached_start=true) must not survive — else
    // a rebuilt timeline on return would skip back-pagination and show truncated
    // history.
    ShellWarmSubscriptionsTestShell s;
    s.current_room_id_ = "!f"; // active room (protected)
    // 6 visited rooms, cap 4 → with !f active, !a is the oldest warm and evicts.
    s.visited_lru_ = {"!f", "!e", "!d", "!c", "!b", "!a"};
    s.pagination_["!a"].reached_start = true; // will be evicted
    s.pagination_["!e"].reached_start = true; // stays warm

    s.prune_warm_subscriptions_();

    CHECK(s.pagination_.count("!a") == 0);     // evicted → state dropped
    CHECK(s.pagination_.count("!e") == 1);     // retained → state kept
    CHECK(s.pagination_["!e"].reached_start);  // unchanged
}
