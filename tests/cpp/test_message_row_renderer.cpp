#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "views/MessageRowGeometry.h"

using Catch::Approx;
namespace mb = tesseract::views::msgbubble;

// ── shaping_width ──────────────────────────────────────────────────────────

TEST_CASE("msgbubble::shaping_width caps at kMaxBubbleW minus padding", "[bubble]")
{
    // Wide row, own message: no avatar column subtracted.
    CHECK(mb::shaping_width(1400.0f, true) ==
          Approx(mb::kMaxBubbleW - 2.0f * mb::kBubblePadX)); // 500
    // Wide row, other user: avatar column does not matter once capped.
    CHECK(mb::shaping_width(1400.0f, false) ==
          Approx(mb::kMaxBubbleW - 2.0f * mb::kBubblePadX)); // 500
}

TEST_CASE("msgbubble::shaping_width tracks a narrow row", "[bubble]")
{
    // 300 wide, own: avail = 300 - 24 = 276 → content = 276 - 20 = 256.
    CHECK(mb::shaping_width(300.0f, true) == Approx(256.0f));
    // 300 wide, other: avail = 300 - 24 - 40 = 236 → content = 236 - 20 = 216.
    CHECK(mb::shaping_width(300.0f, false) == Approx(216.0f));
}

TEST_CASE("msgbubble::shaping_width never goes below kBubbleMinW", "[bubble]")
{
    CHECK(mb::shaping_width(20.0f, true) == Approx(mb::kBubbleMinW));
    CHECK(mb::shaping_width(0.0f, false) == Approx(mb::kBubbleMinW));
}

TEST_CASE("msgbubble::shaping_width subtracts the receipt reserve for other users",
          "[bubble]")
{
    // 300 wide, other user, no reserve: avail = 300-24-40 = 236 -> content = 216.
    CHECK(mb::shaping_width(300.0f, false, 0.0f) == Approx(216.0f));
    // Same row with a 40px receipt cluster reserved: avail = 196 -> content = 176.
    CHECK(mb::shaping_width(300.0f, false, 40.0f) == Approx(176.0f));
    // Own messages never reserve — their receipts sit to the left of the
    // bubble, not overlapping its content.
    CHECK(mb::shaping_width(300.0f, true, 40.0f) == Approx(256.0f));
}

// ── layout: own messages ──────────────────────────────────────────────────

TEST_CASE("msgbubble::layout hugs a short own message", "[bubble]")
{
    const mb::Box b = mb::layout(900.0f, /*is_own=*/true, /*is_cont=*/false, 60.0f);

    CHECK(b.content_w == Approx(60.0f));
    CHECK(b.bubble_w == Approx(60.0f + 2.0f * mb::kBubblePadX)); // 80
    CHECK(b.bubble_x == Approx(900.0f - mb::kEdgePadX - b.bubble_w)); // flush right
    CHECK(b.content_x == Approx(b.bubble_x + mb::kBubblePadX));
    CHECK(b.chip_x == Approx(b.content_x));
    CHECK(b.header_h == Approx(0.0f));
    CHECK_FALSE(b.draw_avatar);
    CHECK_FALSE(b.draw_sender);
    CHECK(b.furniture_right_x == Approx(b.bubble_x - mb::kFurnitureGap));
    CHECK(b.furniture_center_in_row);
    CHECK_FALSE(b.reserve_receipt_width);
}

TEST_CASE("msgbubble::layout hugs an own message narrower than kBubbleMinW",
          "[bubble]")
{
    // A 2-character message ("ok") shapes to well under 48px. The bubble must
    // hug it exactly — no minimum width — so it stays flush to the right edge
    // with no dead space beside the text.
    const mb::Box b = mb::layout(900.0f, /*is_own=*/true, /*is_cont=*/false, 18.0f);

    CHECK(b.content_w == Approx(18.0f));
    CHECK(b.content_w < mb::kBubbleMinW);
    CHECK(b.bubble_w == Approx(18.0f + 2.0f * mb::kBubblePadX));
    CHECK(b.bubble_x == Approx(900.0f - mb::kEdgePadX - b.bubble_w)); // flush right
}

TEST_CASE("msgbubble::layout caps a long own message at kMaxBubbleW", "[bubble]")
{
    const mb::Box b = mb::layout(1400.0f, true, false, 5000.0f);
    CHECK(b.content_w == Approx(500.0f));
    CHECK(b.bubble_w == Approx(520.0f));
    CHECK(b.bubble_x == Approx(1400.0f - mb::kEdgePadX - 520.0f));
}

TEST_CASE("msgbubble::layout keeps a narrow own bubble on-screen", "[bubble]")
{
    const mb::Box b = mb::layout(300.0f, true, false, 9999.0f);
    CHECK(b.content_w == Approx(256.0f));
    CHECK(b.bubble_x >= Approx(mb::kEdgePadX));
}

TEST_CASE("msgbubble::layout still bubbles a continuation own message", "[bubble]")
{
    const mb::Box b = mb::layout(900.0f, true, /*is_cont=*/true, 60.0f);
    CHECK(b.header_h == Approx(0.0f));
    CHECK_FALSE(b.draw_avatar);
    CHECK(b.bubble_w == Approx(80.0f));
}

// ── layout: other users ──────────────────────────────────────────────────

TEST_CASE("msgbubble::layout keeps the left avatar column for other users", "[bubble]")
{
    const mb::Box b = mb::layout(900.0f, /*is_own=*/false, false, 120.0f);

    CHECK(b.bubble_x == Approx(mb::kEdgePadX + mb::kAvatarSize + mb::kAvatarGap)); // 52
    CHECK(b.content_x == Approx(b.bubble_x + mb::kBubblePadX));
    CHECK(b.content_w == Approx(120.0f));
    CHECK(b.header_h == Approx(mb::kAvatarSize));
    CHECK(b.draw_avatar);
    CHECK(b.draw_sender);
    CHECK(b.furniture_right_x == Approx(900.0f - mb::kEdgePadX)); // unchanged from classic
    CHECK(b.reserve_receipt_width);
}

TEST_CASE("msgbubble::layout drops avatar/sender on an other-user continuation", "[bubble]")
{
    const mb::Box b = mb::layout(900.0f, false, /*is_cont=*/true, 120.0f);
    CHECK(b.header_h == Approx(0.0f));
    CHECK_FALSE(b.draw_avatar);
    CHECK_FALSE(b.draw_sender);
}

// ── determinism ──────────────────────────────────────────────────────────

TEST_CASE("msgbubble::layout is a pure function of its inputs", "[bubble]")
{
    const mb::Box a = mb::layout(760.0f, true, false, 240.0f);
    const mb::Box b = mb::layout(760.0f, true, false, 240.0f);
    CHECK(a.content_x == Approx(b.content_x));
    CHECK(a.content_w == Approx(b.content_w));
    CHECK(a.bubble_x == Approx(b.bubble_x));
    CHECK(a.bubble_w == Approx(b.bubble_w));
    CHECK(a.furniture_right_x == Approx(b.furniture_right_x));
}

TEST_CASE("msgbubble::layout with unknown natural width fills the bubble", "[bubble]")
{
    const mb::Box b = mb::layout(900.0f, true, false, -1.0f);
    CHECK(b.content_w == Approx(mb::shaping_width(900.0f, true)));
}
