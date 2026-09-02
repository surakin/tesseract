#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "views/IrcFormat.h"

namespace irc = tesseract::views::msgirc;
using irc::PrefixKind;

// ── nick_color ────────────────────────────────────────────────────────────

TEST_CASE("msgirc::nick_color is deterministic per id + theme", "[irc]")
{
    const auto a1 = irc::nick_color("@alice:example.org", tk::ThemeMode::Light);
    const auto a2 = irc::nick_color("@alice:example.org", tk::ThemeMode::Light);
    CHECK(a1.r == a2.r);
    CHECK(a1.g == a2.g);
    CHECK(a1.b == a2.b);
}

TEST_CASE("msgirc::nick_color stays within the per-theme palette", "[irc]")
{
    auto in_palette = [](const tk::Color& c, const tk::Color* pal, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i)
            if (pal[i].r == c.r && pal[i].g == c.g && pal[i].b == c.b)
                return true;
        return false;
    };

    for (const char* id : {"@a:x", "@bob:y", "@carol:z", "@dave:w", "@erin:q",
                           "@frank:r", "@grace:s", "@heidi:t"})
    {
        const auto light = irc::nick_color(id, tk::ThemeMode::Light);
        const auto dark  = irc::nick_color(id, tk::ThemeMode::Dark);
        CHECK(in_palette(light, irc::kNickLight,
                         sizeof(irc::kNickLight) / sizeof(irc::kNickLight[0])));
        CHECK(in_palette(dark, irc::kNickDark,
                         sizeof(irc::kNickDark) / sizeof(irc::kNickDark[0])));
    }
}

TEST_CASE("msgirc::nick_color spreads ids across the palette", "[irc]")
{
    std::set<std::string> seen;
    const char* ids[] = {"@u0:h", "@u1:h", "@u2:h", "@u3:h", "@u4:h",
                         "@u5:h", "@u6:h", "@u7:h", "@u8:h", "@u9:h",
                         "@u10:h", "@u11:h"};
    for (const char* id : ids)
    {
        const auto c = irc::nick_color(id, tk::ThemeMode::Light);
        seen.insert(std::to_string(c.r) + "," + std::to_string(c.g) + "," +
                    std::to_string(c.b));
    }
    // Not a strict guarantee, but a healthy hash over 12 ids should land on
    // well more than one bucket.
    CHECK(seen.size() >= 4);
}

// ── line prefix ───────────────────────────────────────────────────────────

TEST_CASE("msgirc::line_prefix formats each kind", "[irc]")
{
    CHECK(irc::line_prefix("12:34", "alice", PrefixKind::Message) ==
          "[12:34] <alice> ");
    CHECK(irc::line_prefix("12:34", "alice", PrefixKind::Action) ==
          "[12:34] * alice ");
    CHECK(irc::line_prefix("12:34", "RoomBot", PrefixKind::Notice) ==
          "[12:34] -RoomBot- ");
}

TEST_CASE("msgirc::line_prefix drops the bracket when the time is unknown",
          "[irc]")
{
    CHECK(irc::timestamp_part("").empty());
    CHECK(irc::line_prefix("", "alice", PrefixKind::Message) == "<alice> ");
}

TEST_CASE("msgirc::timestamp_part and nick_part compose into line_prefix",
          "[irc]")
{
    const std::string ts   = irc::timestamp_part("09:05");
    const std::string nick = irc::nick_part("bob", PrefixKind::Message);
    CHECK(ts == "[09:05] ");
    CHECK(nick == "<bob> ");
    CHECK(ts + nick == irc::line_prefix("09:05", "bob", PrefixKind::Message));
}
