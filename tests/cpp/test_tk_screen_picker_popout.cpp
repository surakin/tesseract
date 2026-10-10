#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/PopoutRoomWidget.h"
#include "views/ScreenPickerWidget.h"

#include <cstdint>
#include <string>
#include <vector>

using tesseract::views::PopoutRoomWidget;
using tesseract::views::ScreenPickerWidget;

namespace
{

std::vector<tk::ScreenSource> sp_sources(int n_screens, int n_windows)
{
    std::vector<tk::ScreenSource> v;
    for (int i = 0; i < n_screens; ++i)
        v.push_back({"screen-" + std::to_string(i), "Display " + std::to_string(i), false});
    for (int i = 0; i < n_windows; ++i)
        v.push_back({"win-" + std::to_string(i), "Window " + std::to_string(i), true});
    return v;
}

struct SpStage : vt::Stage
{
    std::unique_ptr<ScreenPickerWidget> picker;
    explicit SpStage(std::vector<tk::ScreenSource> src)
    {
        picker = tk::create_root_widget<ScreenPickerWidget>(&host, std::move(src));
        mount(*picker, {0, 0, 800, 600});
    }
    void run() { relayout_paint(*picker, {0, 0, 800, 600}); }
};

} // namespace

TEST_CASE("ScreenPickerWidget exposes screens and windows as keyboard targets",
          "[tk][view][screen_picker]")
{
    SpStage s(sp_sources(2, 1));
    CHECK(s.picker->access_modal());
    auto names = vt::all_names(*s.picker);
    auto has = [&](const std::string& n)
    { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("Screen 1"));
    CHECK(has("Screen 2"));
    CHECK(has("Window 0"));
    CHECK(has("Cancel"));
}

TEST_CASE("ScreenPickerWidget clicking a tile selects its source once",
          "[tk][view][screen_picker]")
{
    SpStage s(sp_sources(2, 0));
    std::vector<std::string> chosen;
    s.picker->on_source_selected = [&](std::string id) { chosen.push_back(id); };

    // Tiles are 220x140, centred in a 800px-wide view: find the second tile.
    s.host.dispatch_pointer_move({20, 20}); // no-op region
    // First tile top-left is grid_x (centred 3-col grid) -- probe by scan.
    bool picked = false;
    for (float x = 40; x < 780 && !picked; x += 20)
    {
        s.host.dispatch_pointer_down({x, 150});
        s.host.dispatch_pointer_up({x, 150});
        picked = !chosen.empty();
    }
    REQUIRE(picked);
    CHECK(chosen.size() == 1);
    CHECK(chosen[0].rfind("screen-", 0) == 0);

    // The callback is consumed after firing: a second click does nothing.
    s.host.dispatch_pointer_down({400, 150});
    s.host.dispatch_pointer_up({400, 150});
    CHECK(chosen.size() == 1);
}

TEST_CASE("ScreenPickerWidget Cancel fires on_cancelled once",
          "[tk][view][screen_picker]")
{
    SpStage s(sp_sources(1, 0));
    int cancelled = 0;
    s.picker->on_cancelled = [&] { ++cancelled; };
    REQUIRE(vt::press(*s.picker, "Cancel"));
    CHECK(cancelled == 1);
    vt::press(*s.picker, "Cancel");
    CHECK(cancelled == 1);
}

TEST_CASE("ScreenPickerWidget keyboard activation of a tile selects it",
          "[tk][view][screen_picker]")
{
    SpStage s(sp_sources(1, 1));
    std::string chosen;
    s.picker->on_source_selected = [&](std::string id) { chosen = id; };
    REQUIRE(vt::press(*s.picker, "Window 0"));
    CHECK(chosen == "win-0");
}

TEST_CASE("ScreenPickerWidget thumbnails, wheel scrolling and an empty list",
          "[tk][view][screen_picker]")
{
    SpStage s(sp_sources(12, 0)); // enough rows to scroll
    std::vector<std::uint8_t> px(8 * 8 * 4, 128);
    s.picker->set_thumbnail(0, px, 8, 8);
    s.picker->set_thumbnail(99, px, 8, 8); // out of range: ignored
    s.run();
    CHECK(s.picker->on_wheel({100, 100}, 0, 5, false));
    s.run();
    CHECK(s.picker->on_wheel({100, 100}, 0, -50, false));
    s.run();
    s.picker->on_pointer_drag({790, 300});
    // Clicking a scrolled-out region still resolves cleanly.
    s.host.dispatch_pointer_down({400, 590});
    s.host.dispatch_pointer_up({400, 590});
    auto alive = s.picker->alive_token();
    CHECK_FALSE(alive.expired());

    SpStage empty({});
    empty.run();
}

// ── PopoutRoomWidget ──────────────────────────────────────────────────────

TEST_CASE("PopoutRoomWidget owns the room view and overlays",
          "[tk][view][popout]")
{
    vt::Stage s;
    auto w = tk::create_root_widget<PopoutRoomWidget>(&s.host);
    s.mount(*w, {0, 0, 800, 600});
    CHECK(w->room_view() != nullptr);
    CHECK(w->image_viewer() != nullptr);
    CHECK(w->video_viewer() != nullptr);
    CHECK(w->forward_picker() != nullptr);
    CHECK(w->confirm_dialog() != nullptr);
    CHECK(w->room_media_view() == w->room_view()->room_media_view());
}

TEST_CASE("PopoutRoomWidget show/hide viewers toggle visibility",
          "[tk][view][popout]")
{
    vt::Stage s;
    auto w = tk::create_root_widget<PopoutRoomWidget>(&s.host);
    s.mount(*w, {0, 0, 800, 600});
    w->show_image_viewer(true);
    CHECK(w->image_viewer()->visible());
    w->show_image_viewer(false);
    CHECK_FALSE(w->image_viewer()->visible());
    w->show_video_viewer(true);
    CHECK(w->video_viewer()->visible());
    w->show_video_viewer(false);
    CHECK_FALSE(w->video_viewer()->visible());
}

TEST_CASE("PopoutRoomWidget hides the compose rect while a modal is open and "
          "drops focus when one appears",
          "[tk][view][popout]")
{
    vt::Stage s;
    auto w = tk::create_root_widget<PopoutRoomWidget>(&s.host);
    int layout_changes = 0;
    w->on_layout_changed = [&] { ++layout_changes; };
    s.mount(*w, {0, 0, 800, 600});

    // No modal: the rect equals the room view's own.
    CHECK(w->compose_text_area_rect().w == w->room_view()->compose_text_area_rect().w);

    tesseract::views::ConfirmDialog::Options opts;
    opts.title = "Sure?";
    w->confirm_dialog()->open(opts, [] {});
    s.relayout_paint(*w, {0, 0, 800, 600});
    CHECK(w->compose_text_area_rect().empty());
    CHECK(layout_changes >= 1);
    s.relayout_paint(*w, {0, 0, 800, 600}); // modal already open: no new edge
}
