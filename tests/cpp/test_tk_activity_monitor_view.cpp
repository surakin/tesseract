#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/ActivityMonitorView.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ActivityEntry;
using tesseract::ActivityState;
using tesseract::views::ActivityMonitorView;

namespace
{

ActivityEntry make_entry(const std::string& name, const std::string& group,
                         ActivityState state = ActivityState::Idle,
                         const std::string& kind = "periodic")
{
    ActivityEntry e;
    e.name             = name;
    e.group            = group;
    e.kind             = kind;
    e.state            = state;
    e.run_count        = 3;
    e.last_started_ms  = 1'000;
    e.last_finished_ms = 4'000;
    return e;
}

} // namespace

TEST_CASE("ActivityMonitorView::format_duration picks the coarsest unit",
          "[activity_monitor]")
{
    CHECK(ActivityMonitorView::format_duration(-5) == "0s");
    CHECK(ActivityMonitorView::format_duration(999) == "0s");
    CHECK(ActivityMonitorView::format_duration(59'000) == "59s");
    CHECK(ActivityMonitorView::format_duration(60'000) == "1m");
    CHECK(ActivityMonitorView::format_duration(3'600'000) == "1h");
    CHECK(ActivityMonitorView::format_duration(2 * 86'400'000LL) == "2d");
}

TEST_CASE("ActivityMonitorView lays out summary, group headers and job rows",
          "[activity_monitor]")
{
    ActivityMonitorView v;
    CHECK(v.count() == 0);

    v.set_snapshot({make_entry("a", "Sync"), make_entry("b", "Sync", ActivityState::Running),
                    make_entry("c", "Workers", ActivityState::Error)},
                   10'000);
    // summary + 2 group headers + 3 jobs
    CHECK(v.count() == 6);
    CHECK(v.job_count() == 3);
}

TEST_CASE("ActivityMonitorView rows are unselectable and keyed by group/name",
          "[activity_monitor]")
{
    ActivityMonitorView v;
    v.set_snapshot({make_entry("a", "Sync")}, 10'000);
    for (std::size_t i = 0; i < v.count(); ++i)
        CHECK_FALSE(v.is_selectable(i));
    CHECK(v.row_key(0) == "summary");
    CHECK(v.row_key(1) == "group:Sync");
    CHECK(v.row_key(2) == "job:Sync/a");
}

TEST_CASE("ActivityMonitorView accessibility exposes only job rows",
          "[activity_monitor]")
{
    ActivityMonitorView v;
    v.set_snapshot({make_entry("presence-poll", "Sync", ActivityState::Running, "loop")},
                   10'000);
    CHECK(v.access_role_for_row(0) == tk::Role::None);
    CHECK(v.access_role_for_row(1) == tk::Role::None);
    CHECK(v.access_role_for_row(2) == tk::Role::ListItem);
    CHECK(v.access_name_for_row(2).find("presence-poll") != std::string::npos);
    CHECK(v.access_name_for_row(99).empty());
}

TEST_CASE("ActivityMonitorView paints every row kind without crashing",
          "[activity_monitor]")
{
    auto surface = TestSurface::create(640, 400);
    ActivityMonitorView v;
    v.set_snapshot({make_entry("a", "Sync", ActivityState::Running, "loop"),
                    make_entry("b", "Sync", ActivityState::Error),
                    make_entry("c", "Workers", ActivityState::Idle, "pool"),
                    make_entry("never", "Workers")},
                   10'000);
    auto lc = tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    v.measure(lc, {640, 400});
    v.arrange(lc, {0, 0, 640, 400});
    const tk::Theme theme = tk::Theme::light();
    tk::PaintCtx pc{surface->canvas(), surface->factory(), theme};
    v.paint(pc);
    for (std::size_t i = 0; i < v.count(); ++i)
        v.paint_row(i, pc, {0, static_cast<float>(i) * 52.0f, 640, 52.0f}, false, i % 2 == 0);
    SUCCEED();
}
