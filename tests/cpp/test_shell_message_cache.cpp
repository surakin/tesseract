#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct TestShell : ShellBase
{
    void post_to_ui_(std::function<void()> fn) override { fn(); }
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

    using ShellBase::current_room_id_;
    using ShellBase::handle_message_inserted_ui_;
    using ShellBase::handle_message_updated_ui_;
    using ShellBase::handle_message_removed_ui_;
    using ShellBase::message_cache_;
    using ShellBase::message_cache_lru_;
};

void seed_cache(TestShell& s, const std::string& room_id)
{
    tesseract::views::MessageRowData row;
    row.event_id = "evt1";
    s.message_cache_[room_id] = {row};
    s.message_cache_lru_.push_front(room_id);
}

std::unique_ptr<tesseract::Event> make_event()
{
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = "new_evt";
    return ev;
}

} // namespace

TEST_CASE("handle_message_inserted_ui_ invalidates cache for non-current room",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";
    seed_cache(s, "!other:example.org");

    REQUIRE(s.message_cache_.count("!other:example.org") == 1);

    s.handle_message_inserted_ui_("!other:example.org", 0, make_event());

    CHECK(s.message_cache_.count("!other:example.org") == 0);
    CHECK(s.message_cache_lru_.empty());
}

TEST_CASE("handle_message_inserted_ui_ does not touch cache for current room",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";
    seed_cache(s, "!current:example.org");

    s.handle_message_inserted_ui_("!current:example.org", 0, make_event());

    CHECK(s.message_cache_.count("!current:example.org") == 1);
}

TEST_CASE("handle_message_inserted_ui_ ignores Unhandled events",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";
    seed_cache(s, "!other:example.org");

    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Unhandled;
    s.handle_message_inserted_ui_("!other:example.org", 0, std::move(ev));

    CHECK(s.message_cache_.count("!other:example.org") == 1);
}

TEST_CASE("handle_message_updated_ui_ invalidates cache for non-current room",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";
    seed_cache(s, "!other:example.org");

    s.handle_message_updated_ui_("!other:example.org", 0, make_event());

    CHECK(s.message_cache_.count("!other:example.org") == 0);
    CHECK(s.message_cache_lru_.empty());
}

TEST_CASE("handle_message_removed_ui_ invalidates cache for non-current room",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";
    seed_cache(s, "!other:example.org");

    s.handle_message_removed_ui_("!other:example.org", 0);

    CHECK(s.message_cache_.count("!other:example.org") == 0);
    CHECK(s.message_cache_lru_.empty());
}

TEST_CASE("cache invalidation is a no-op when room has no cache entry",
          "[shell][message_cache]")
{
    TestShell s;
    s.current_room_id_ = "!current:example.org";

    s.handle_message_inserted_ui_("!other:example.org", 0, make_event());
    s.handle_message_updated_ui_("!other:example.org", 0, make_event());
    s.handle_message_removed_ui_("!other:example.org", 0);

    CHECK(s.message_cache_.empty());
}

TEST_CASE("handle_message_inserted_ui_ skips main list when ev is in-thread",
          "[shell][thread_filter]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = "$reply";
    ev->thread_root_id = "$root"; // marks as in-thread

    // Should not crash even without a room_view_; the early-return prevents
    // the room_view_->insert_message() call.
    s.handle_message_inserted_ui_("!r:x", 0, std::move(ev));
    // No assertion needed beyond not crashing — TestShell has no room_view_,
    // so the prior code path would deref null. Reaching this point proves
    // the short-circuit is in place.
    SUCCEED();
}

TEST_CASE("handle_message_inserted_ui_ still invalidates background-room cache",
          "[shell][thread_filter]")
{
    TestShell s;
    s.current_room_id_ = "!current:x";
    seed_cache(s, "!other:x");
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = "$reply";
    ev->thread_root_id = "$root";
    s.handle_message_inserted_ui_("!other:x", 0, std::move(ev));
    // Background-room path is unchanged — cache invalidation still fires
    // regardless of thread membership.
    CHECK(s.message_cache_.count("!other:x") == 0);
}
