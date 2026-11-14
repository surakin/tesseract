#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "ffi_convert.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/types.h>

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomView;

// ── FFI conversion ──────────────────────────────────────────────────────────

TEST_CASE("make_event turns m.room.tombstone into RoomTombstoneStateEvent", "[ffi][room_upgrade]")
{
    tesseract_ffi::TimelineEvent in{};
    in.msg_type = "m.room.tombstone";
    in.sender = "@alice:server";
    in.sender_name = "Alice";
    in.body = "moving on";
    in.replacement_room_id = "!new:server";

    auto ev = tesseract::make_event(in);
    REQUIRE(ev);
    CHECK(ev->type == tesseract::EventType::RoomTombstone);
    auto* t = dynamic_cast<tesseract::RoomTombstoneStateEvent*>(ev.get());
    REQUIRE(t != nullptr);
    CHECK(t->replacement_room_id == "!new:server");
    CHECK(t->body == "moving on");
    CHECK(t->sender_name == "Alice");
}

TEST_CASE("from_ffi(RoomInfo) propagates the room-upgrade fields", "[ffi][room_upgrade]")
{
    tesseract_ffi::RoomInfo in{};
    in.id = "!old:server";
    in.predecessor_room_id = "!older:server";
    in.predecessor_via.push_back("a.org");
    in.predecessor_via.push_back("b.org");
    in.successor_room_id = "!new:server";
    in.successor_reason = "bigger room";
    in.successor_via.push_back("c.org");
    in.successor_joined = true;

    auto out = tesseract::from_ffi(in);
    CHECK(out.predecessor_room_id == "!older:server");
    CHECK(out.predecessor_via == std::vector<std::string>{"a.org", "b.org"});
    CHECK(out.successor_room_id == "!new:server");
    CHECK(out.successor_reason == "bigger room");
    CHECK(out.successor_via == std::vector<std::string>{"c.org"});
    CHECK(out.successor_joined);

    // Two infos differing only in an upgrade field are not equal, so the room
    // list notices the change.
    auto changed = out;
    changed.successor_joined = false;
    CHECK(out != changed);
}

// ── MessageListView: "View older messages" ──────────────────────────────────

namespace
{

struct UpgradeStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 300);
    void run(tk::Widget& root, tk::Rect bounds) const
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        root.paint(pc);
    }
};

std::vector<MessageRowData> start_then_text()
{
    std::vector<MessageRowData> rows;
    MessageRowData start;
    start.kind = MessageRowData::Kind::TimelineStart;
    rows.push_back(start);
    MessageRowData t;
    t.kind = MessageRowData::Kind::Text;
    t.event_id = "$a";
    t.sender_name = "Alice";
    t.body = "hello";
    t.timestamp_ms = 1700000000000ULL;
    rows.push_back(t);
    return rows;
}

tk::Point center_of(const tk::Rect& r)
{
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

} // namespace

TEST_CASE("MessageListView offers View older messages on the start row only "
          "when the room has a predecessor",
          "[message_list][room_upgrade]")
{
    UpgradeStage st;
    MessageListView v;
    v.set_messages(start_then_text(), /*room_switch=*/true);
    v.scroll_to_top();
    st.run(v, {0, 0, 400, 300});
    REQUIRE(v.predecessor_link() != nullptr);
    CHECK_FALSE(v.predecessor_link()->own_visible());

    v.set_predecessor_available(true);
    st.run(v, {0, 0, 400, 300});
    CHECK(v.predecessor_link()->own_visible());

    v.set_predecessor_available(false);
    st.run(v, {0, 0, 400, 300});
    CHECK_FALSE(v.predecessor_link()->own_visible());
}

TEST_CASE("clicking View older messages fires on_open_predecessor", "[message_list][room_upgrade]")
{
    UpgradeStage st;
    MessageListView v;
    int fired = 0;
    v.on_open_predecessor = [&]
    {
        ++fired;
    };
    v.set_messages(start_then_text(), /*room_switch=*/true);
    v.set_predecessor_available(true);
    v.scroll_to_top();
    st.run(v, {0, 0, 400, 300});
    REQUIRE(v.predecessor_link()->own_visible());

    const tk::Point p = center_of(v.predecessor_link()->bounds());
    tk::Widget* hit = v.dispatch_pointer_down(p);
    REQUIRE(hit != nullptr);
    hit->on_pointer_up(p, true);
    CHECK(fired == 1);
}

TEST_CASE("the View older messages button is hidden while the start row is "
          "scrolled out of view",
          "[message_list][room_upgrade]")
{
    UpgradeStage st;
    MessageListView v;
    auto rows = start_then_text();
    for (int i = 0; i < 60; ++i)
    {
        MessageRowData t = rows.back();
        t.event_id = "$m" + std::to_string(i);
        rows.push_back(t);
    }
    v.set_messages(std::move(rows), /*room_switch=*/true); // lands at the bottom
    v.set_predecessor_available(true);
    st.run(v, {0, 0, 400, 300});
    CHECK_FALSE(v.predecessor_link()->own_visible());
}

// ── RoomView: replaced room ─────────────────────────────────────────────────

TEST_CASE("RoomView shows the replaced strip and disables the composer for a "
          "tombstoned room",
          "[view][room][room_upgrade]")
{
    UpgradeStage st;
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;

    tesseract::RoomInfo info;
    info.id = "!old:example.org";
    info.name = "Old";
    info.successor_room_id = "!new:example.org";
    info.successor_via = {"example.org"};
    view.set_room(info);
    st.run(view, {0, 0, 800, 600});

    REQUIRE(view.replaced_banner() != nullptr);
    CHECK(view.replaced_banner()->own_visible());
    CHECK_FALSE(view.compose_bar()->enabled());
    CHECK(view.replaced_banner()->action_text() == "Join the new room");

    std::string opened;
    std::vector<std::string> via;
    view.on_open_room_version = [&](const std::string& id, const std::vector<std::string>& v)
    {
        opened = id;
        via = v;
    };
    view.replaced_banner()->on_open();
    CHECK(opened == "!new:example.org");
    CHECK(via == std::vector<std::string>{"example.org"});

    // Once the successor is joined the button follows the upgrade.
    info.successor_joined = true;
    view.set_room(info);
    CHECK(view.replaced_banner()->action_text() == "Go to the new room");
}

TEST_CASE("RoomView leaves the composer enabled and the strip hidden for an "
          "ordinary room, and clears both when the room closes",
          "[view][room][room_upgrade]")
{
    UpgradeStage st;
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;

    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    info.name = "Plain";
    view.set_room(info);
    CHECK_FALSE(view.replaced_banner()->own_visible());
    CHECK(view.compose_bar()->enabled());

    // An upgraded room, then back to an ordinary one: the strip goes away and
    // the composer comes back.
    tesseract::RoomInfo old = info;
    old.id = "!old:example.org";
    old.successor_room_id = "!new:example.org";
    view.set_room(old);
    REQUIRE(view.replaced_banner()->own_visible());
    view.set_room(info);
    CHECK_FALSE(view.replaced_banner()->own_visible());
    CHECK(view.compose_bar()->enabled());

    view.set_room(old);
    view.clear_room();
    CHECK_FALSE(view.replaced_banner()->own_visible());
}

TEST_CASE("RoomView opens the predecessor with its via servers", "[view][room][room_upgrade]")
{
    auto owner = tk::create_root_widget<RoomView>(nullptr);
    RoomView& view = *owner;

    tesseract::RoomInfo info;
    info.id = "!new:example.org";
    info.name = "New";
    info.predecessor_room_id = "!old:example.org";
    info.predecessor_via = {"a.org", "b.org"};
    view.set_room(info);

    std::string opened;
    std::vector<std::string> via;
    view.on_open_room_version = [&](const std::string& id, const std::vector<std::string>& v)
    {
        opened = id;
        via = v;
    };
    REQUIRE(view.message_list()->on_open_predecessor);
    view.message_list()->on_open_predecessor();
    CHECK(opened == "!old:example.org");
    CHECK(via == std::vector<std::string>{"a.org", "b.org"});
}

// ── ShellBase: hidden (upgraded) rooms ──────────────────────────────────────

namespace
{

struct UpgradeTestShell : tesseract::test::TestShellBase
{

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
    void apply_window_title_ui_(const std::string&) override
    {
    }

    using ShellBase::hidden_rooms_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::room_by_id_;
    using ShellBase::rooms_;
};

} // namespace

TEST_CASE("room_by_id_ falls back to a hidden upgraded room", "[shell][room_upgrade]")
{
    UpgradeTestShell s;
    tesseract::RoomInfo listed;
    listed.id = "!new:x";
    listed.name = "New";
    s.rooms_.push_back(listed);
    s.mark_room_index_dirty_();

    tesseract::RoomInfo hidden;
    hidden.id = "!old:x";
    hidden.name = "Old";
    hidden.successor_room_id = "!new:x";
    s.hidden_rooms_[hidden.id] = hidden;

    REQUIRE(s.room_by_id_("!new:x") != nullptr);
    CHECK(s.room_by_id_("!new:x")->name == "New");
    const auto* old = s.room_by_id_("!old:x");
    REQUIRE(old != nullptr);
    CHECK(old->successor_room_id == "!new:x");
    CHECK(s.room_by_id_("!unknown:x") == nullptr);
}
