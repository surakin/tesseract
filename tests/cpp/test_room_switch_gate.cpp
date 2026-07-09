#include <catch2/catch_test_macros.hpp>

#include "tesseract/media_source.h"
#include "tk/canvas.h"
#include "views/MessageListView.h" // MessageRowData
#include "views/RoomSwitchGateKeeper.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::views::MessageRowData;
using tesseract::views::RoomSwitchGateKeeper;
using tesseract::views::UrlPreviewData;

namespace
{

// A gate whose image provider always reports "not decoded yet" (nullptr) and
// whose preview provider always reports "no preview". This isolates the
// height-stability rule: with these providers, the ONLY reason the gate would
// keep blocking is a genuinely unknown height.
RoomSwitchGateKeeper make_undecoded_gate()
{
    RoomSwitchGateKeeper g;
    g.set_providers(
        [](const std::string&) -> const tk::Image* { return nullptr; },
        [](const std::string&) -> const UrlPreviewData* { return nullptr; });
    return g;
}

MessageRowData image_row_with_dims(int w, int h)
{
    MessageRowData m;
    m.kind = MessageRowData::Kind::Image;
    m.event_id = "$img";
    m.source = tesseract::MediaSource::plain("mxc://example.org/img");
    m.media_w = w;
    m.media_h = h;
    return m;
}

} // namespace

TEST_CASE("gate does not wait on image decode when intrinsic dimensions are known",
          "[room_switch_gate]")
{
    // The measure path reserves the media box from media_w/media_h, so the
    // row's height is final before the image decodes. The gate must therefore
    // treat the row as satisfied and not hold the whole list invisible waiting
    // for the decode.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    CHECK(g.dep_satisfied(image_row_with_dims(400, 300)));
}

TEST_CASE("gate still waits on an image whose dimensions are unknown",
          "[room_switch_gate]")
{
    // No intrinsic dimensions → the reserved height is a placeholder that will
    // change on decode, so waiting (to avoid a reflow) is still correct.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    CHECK_FALSE(g.dep_satisfied(image_row_with_dims(0, 0)));
}

TEST_CASE("gate does not wait on a video thumbnail when dimensions are known",
          "[room_switch_gate]")
{
    // Video height is derived from media_w/media_h (the decoded thumbnail never
    // affects it), so a known-dimension video must not gate the list.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    MessageRowData m;
    m.kind = MessageRowData::Kind::Video;
    m.event_id = "$vid";
    m.thumbnail = tesseract::MediaSource::plain("mxc://example.org/thumb");
    m.media_w = 1280;
    m.media_h = 720;
    CHECK(g.dep_satisfied(m));
}

TEST_CASE("reset_within_switch is a no-op when no gate is active",
          "[room_switch_gate]")
{
    // An ordinary backfill/pagination reset on an already-revealed room must
    // not start gating the list — that would freeze scroll-up pagination
    // behind an invisible list.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    g.reset_within_switch();
    CHECK_FALSE(g.active());
}

TEST_CASE("reset_within_switch preserves focused mode across the re-arm",
          "[room_switch_gate]")
{
    // ShellBase only calls begin_focused_gate() on the room_switch==true
    // branch, so a same-room reset re-arm must remember focused mode itself
    // or a permalink/thread-focus open loses its jump-to-event target.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    std::string revealed_focus_id;
    bool        revealed_to_bottom = false;
    g.set_scroll_callbacks(
        [&](const std::string& id) { revealed_focus_id = id; },
        [&]() { revealed_to_bottom = true; });

    g.begin_room_switch();
    g.set_focus_event("$focus");

    // Simulate a same-room reset landing before the original gate resolved.
    g.reset_within_switch();

    // Empty scan: nothing pending, so the re-armed gate is immediately
    // revealable — but it must still remember it's in focused mode.
    g.evaluate([](const std::function<void(const MessageRowData&)>&) {});
    REQUIRE_FALSE(g.blocking());
    g.try_reveal();

    CHECK(revealed_focus_id == "$focus");
    CHECK_FALSE(revealed_to_bottom);
}

TEST_CASE("reset_within_switch's fresh gate ignores the superseded gate's timeout",
          "[room_switch_gate]")
{
    // The gate armed by the ORIGINAL switch schedules a 400ms timeout
    // fallback. If a same-room reset re-arms the gate, that stale timeout
    // must not be able to force an early reveal of the new gate.
    RoomSwitchGateKeeper g = make_undecoded_gate();
    std::vector<std::function<void()>> timeouts;
    g.set_post_delayed([&](int, std::function<void()> cb)
                        { timeouts.push_back(std::move(cb)); });
    auto alive = std::make_shared<bool>(true);
    g.set_alive(alive);
    g.set_repaint([] {});

    auto scan_unresolved =
        [](const std::function<void(const MessageRowData&)>& visit)
    { visit(image_row_with_dims(0, 0)); };

    g.begin_room_switch();
    g.evaluate(scan_unresolved);
    REQUIRE(g.blocking()); // unresolved image dep holds the gate

    g.reset_within_switch(); // same-room reset races the still-pending gate
    g.evaluate(scan_unresolved); // next paint re-scans the fresh gate
    REQUIRE(g.blocking());
    REQUIRE(timeouts.size() == 2);

    // The FIRST (superseded) gate's timeout must not reveal the new gate.
    timeouts[0]();
    CHECK(g.active());
    CHECK(g.blocking());

    // The SECOND (current) gate's timeout does force a reveal.
    timeouts[1]();
    CHECK_FALSE(g.blocking());
}
