#include <catch2/catch_test_macros.hpp>

#include "views/settings/RoomMediaSection.h"
#include "tk_test_surface.h"

using tesseract::views::RoomMediaSection;
using Mode = tesseract::MediaPreviewConfig::Mode;

namespace
{

struct TkRoomMediaSectionStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("RoomMediaSection: paints without crashing", "[room_media_section]")
{
    RoomMediaSection s;
    TkRoomMediaSectionStage st;
    st.run(s, {0.0f, 0.0f, 800.0f, 600.0f});
}

TEST_CASE("RoomMediaSection: defaults to \"Use global default\"",
          "[room_media_section]")
{
    RoomMediaSection s;
    REQUIRE(s.override_combo() != nullptr);
    CHECK(s.override_combo()->selected_value() == "global");
}

TEST_CASE("RoomMediaSection: set_override(true, mode) selects the matching option",
          "[room_media_section]")
{
    RoomMediaSection s;

    s.set_override(true, Mode::Off);
    CHECK(s.override_combo()->selected_value() == "off");

    s.set_override(true, Mode::On);
    CHECK(s.override_combo()->selected_value() == "on");
}

TEST_CASE("RoomMediaSection: set_override(true, Private) displays as \"Always\" "
          "since this per-room combo has no Private option",
          "[room_media_section]")
{
    RoomMediaSection s;
    s.set_override(true, Mode::Private);
    CHECK(s.override_combo()->selected_value() == "on");
}

TEST_CASE("RoomMediaSection: set_override(false, ...) always selects "
          "\"Use global default\" regardless of mode",
          "[room_media_section]")
{
    RoomMediaSection s;
    s.set_override(false, Mode::Off);
    CHECK(s.override_combo()->selected_value() == "global");
    s.set_override(false, Mode::Private);
    CHECK(s.override_combo()->selected_value() == "global");
    s.set_override(false, Mode::On);
    CHECK(s.override_combo()->selected_value() == "global");
}

TEST_CASE("RoomMediaSection: picking a concrete option fires on_override_changed",
          "[room_media_section]")
{
    RoomMediaSection s;

    int count = 0;
    std::optional<Mode> picked;
    s.on_override_changed = [&](std::optional<Mode> m)
    {
        ++count;
        picked = m;
    };

    s.override_combo()->on_changed("off");
    REQUIRE(count == 1);
    REQUIRE(picked.has_value());
    CHECK(*picked == Mode::Off);
}

TEST_CASE("RoomMediaSection: picking \"Use global default\" fires "
          "on_override_changed with std::nullopt",
          "[room_media_section]")
{
    RoomMediaSection s;
    s.set_override(true, Mode::Off);

    int count = 0;
    std::optional<Mode> picked = Mode::On; // sentinel, must become nullopt
    s.on_override_changed = [&](std::optional<Mode> m)
    {
        ++count;
        picked = m;
    };

    s.override_combo()->on_changed("global");
    REQUIRE(count == 1);
    CHECK_FALSE(picked.has_value());
}
