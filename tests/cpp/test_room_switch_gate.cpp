#include <catch2/catch_test_macros.hpp>

#include "tesseract/media_source.h"
#include "tk/canvas.h"
#include "views/MessageListView.h" // MessageRowData
#include "views/RoomSwitchGateKeeper.h"

#include <string>

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
