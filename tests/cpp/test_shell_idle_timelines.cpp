#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellIdleTimelinesWithAccountManager { tesseract::AccountManager am_; };

struct ShellIdleTimelinesTestShell : ShellIdleTimelinesWithAccountManager, ShellBase
{
    ShellIdleTimelinesTestShell() : ShellBase(am_) {}

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
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::select_idle_room_evictions_;
    using ShellBase::select_idle_thread_evictions_;
    using ShellBase::sweep_idle_timelines_;
    using ShellBase::kIdleTimelineTtl;
    using ShellBase::room_last_active_;
    using ShellBase::thread_last_active_;
    using ShellBase::current_room_id_;
    using ShellBase::pagination_;
    using ShellBase::last_sent_receipt_;
};

} // namespace

TEST_CASE("select_idle_room_evictions_ evicts rooms idle past the ttl",
          "[shell][idle_timelines]")
{
    ShellIdleTimelinesTestShell s;
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::minutes{30};

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_active{
        {"!stale", now - std::chrono::minutes{31}},
        {"!fresh", now - std::chrono::minutes{5}},
    };
    auto evicted =
        s.select_idle_room_evictions_(last_active, {}, now, ttl);

    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0] == "!stale");
}

TEST_CASE("select_idle_room_evictions_ never evicts a currently-visible room",
          "[shell][idle_timelines]")
{
    ShellIdleTimelinesTestShell s;
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::minutes{30};

    // Even a very stale timestamp must not evict a room that's on-screen —
    // in production sweep_idle_timelines_ self-refreshes visible rooms every
    // tick, but the pure partition function must enforce this regardless of
    // what the caller passes in.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_active{
        {"!visible", now - std::chrono::hours{5}},
    };
    auto evicted =
        s.select_idle_room_evictions_(last_active, {"!visible"}, now, ttl);

    CHECK(evicted.empty());
}

TEST_CASE("select_idle_room_evictions_ evicts nothing within the ttl window",
          "[shell][idle_timelines]")
{
    ShellIdleTimelinesTestShell s;
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::minutes{30};

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_active{
        {"!a", now - std::chrono::minutes{29}},
    };
    auto evicted =
        s.select_idle_room_evictions_(last_active, {}, now, ttl);

    CHECK(evicted.empty());
}

TEST_CASE("select_idle_thread_evictions_ mirrors the room-level partition",
          "[shell][idle_timelines]")
{
    ShellIdleTimelinesTestShell s;
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::minutes{30};

    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        last_active{
            {{"!room", "$stale_root"}, now - std::chrono::minutes{45}},
            {{"!room", "$visible_root"}, now - std::chrono::hours{2}},
        };
    std::set<std::pair<std::string, std::string>> visible{{"!room", "$visible_root"}};

    auto evicted = s.select_idle_thread_evictions_(last_active, visible, now, ttl);

    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0] == std::make_pair(std::string("!room"), std::string("$stale_root")));
}

TEST_CASE("sweep_idle_timelines_ drops bookkeeping for evicted rooms and "
          "self-refreshes the active room",
          "[shell][idle_timelines]")
{
    ShellIdleTimelinesTestShell s;
    s.current_room_id_ = "!active";
    const auto now = std::chrono::steady_clock::now();

    // !active has a stale timestamp (as if it hadn't been touched in a
    // while), but being current_room_id_ it must be self-refreshed, not
    // evicted. !stale is genuinely idle and must be torn down.
    s.room_last_active_["!active"] = now - std::chrono::hours{2};
    s.room_last_active_["!stale"] = now - std::chrono::hours{2};
    s.pagination_["!stale"].reached_start = true;
    s.last_sent_receipt_["!stale"] = "$evt";
    s.pagination_["!active"].reached_start = true;

    s.sweep_idle_timelines_();

    CHECK(s.pagination_.count("!stale") == 0);
    CHECK(s.last_sent_receipt_.count("!stale") == 0);
    CHECK(s.room_last_active_.count("!stale") == 0);

    CHECK(s.pagination_.count("!active") == 1);
    REQUIRE(s.room_last_active_.count("!active") == 1);
    CHECK(s.room_last_active_["!active"] >= now);
}
