#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <string>
#include <unordered_set>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct WithAccountManager { tesseract::AccountManager am_; };

struct TestShell : WithAccountManager, ShellBase
{
    TestShell() : ShellBase(am_) {}

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

    using ShellBase::rebuild_room_index_;
    using ShellBase::room_by_id_;
    using ShellBase::rooms_;

    // Replace the room list the way the production code does (wholesale), then
    // refresh the id→index map.
    void set_rooms_for_test(std::vector<tesseract::RoomInfo> rs)
    {
        rooms_ = std::move(rs);
        rebuild_room_index_();
    }
};

tesseract::RoomInfo room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    return r;
}

} // namespace

TEST_CASE("room_by_id_ returns the matching room or nullptr",
          "[shell][room_index]")
{
    TestShell s;
    s.set_rooms_for_test({room("!a:x"), room("!b:x"), room("!c:x")});

    const auto* b = s.room_by_id_("!b:x");
    REQUIRE(b != nullptr);
    CHECK(b->id == "!b:x");
    CHECK(s.room_by_id_("!missing:x") == nullptr);
}

TEST_CASE("room_by_id_ reflects a wholesale room-list replacement",
          "[shell][room_index]")
{
    TestShell s;
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
    TestShell s;
    CHECK(s.room_by_id_("!anything:x") == nullptr);
}

TEST_CASE("touch_visited_room_ moves a room to the MRU front without duplicates",
          "[shell][warm_subscriptions]")
{
    TestShell s;
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
    TestShell s;
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
    TestShell s;
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
    TestShell s;
    s.visited_lru_ = {"!c", "!b", "!a"};
    auto evicted = s.select_warm_evictions_({"!c"}, /*warm_cap=*/4);
    CHECK(evicted.empty());
    CHECK(s.visited_lru_.size() == 3);
}
