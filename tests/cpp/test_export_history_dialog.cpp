#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "views/ExportHistoryDialog.h"
#include "tesseract/types.h"

#include <cctype>

using tesseract::views::ExportHistoryDialog;

namespace
{

tesseract::RoomExportProgress gathering_progress(std::uint64_t created_ms, std::uint64_t newest_ms,
                                                  std::uint64_t oldest_ms, bool reached_start = false)
{
    tesseract::RoomExportProgress p;
    p.room_created_ts_ms = created_ms;
    p.newest_ts_ms = newest_ms;
    p.oldest_ts_ms = oldest_ms;
    p.reached_start = reached_start;
    return p;
}

} // namespace

TEST_CASE("ExportHistoryDialog: compute_progress_fraction is indeterminate with no room_created_ts_ms",
          "[history_export][dialog]")
{
    tesseract::RoomExportProgress p;
    CHECK_FALSE(ExportHistoryDialog::compute_progress_fraction(p).has_value());
}

TEST_CASE("ExportHistoryDialog: compute_progress_fraction is indeterminate while gathering, even with a known room_created_ts_ms",
          "[history_export][dialog]")
{
    // No time-ratio estimate is attempted during gathering at all — real
    // message density can be wildly uneven across a room's calendar-time
    // span (measured directly: some 100-event batches under an hour,
    // others over a week), so no fraction is claimed regardless of
    // whether room_created_ts_ms is known.
    const auto p = gathering_progress(/*created*/ 100, /*newest*/ 1100, /*oldest*/ 600);
    CHECK_FALSE(ExportHistoryDialog::compute_progress_fraction(p).has_value());
}

TEST_CASE("ExportHistoryDialog: compute_progress_fraction snaps to the gathering band max on reached_start",
          "[history_export][dialog]")
{
    // Even though the time-ratio estimate would only be ~10% (oldest_ts_ms
    // barely below newest_ts_ms), reached_start is authoritative and wins.
    const auto p = gathering_progress(/*created*/ 100, /*newest*/ 1100, /*oldest*/ 1000, /*reached_start*/ true);
    const auto fraction = ExportHistoryDialog::compute_progress_fraction(p);
    REQUIRE(fraction.has_value());
    CHECK(*fraction == Catch::Approx(0.85f));
}

TEST_CASE("ExportHistoryDialog: compute_progress_fraction reflects real assembly progress while finalizing",
          "[history_export][dialog]")
{
    tesseract::RoomExportProgress p;
    p.finalizing = true;
    p.assembly_done = 5;
    p.assembly_total = 10;
    const auto fraction = ExportHistoryDialog::compute_progress_fraction(p);
    REQUIRE(fraction.has_value());
    // Halfway through assembly: 0.85 + 0.5 * (1 - 0.85) = 0.925.
    CHECK(*fraction == Catch::Approx(0.925f));
}

TEST_CASE("ExportHistoryDialog: compute_progress_fraction reaches exactly 1.0 when assembly completes",
          "[history_export][dialog]")
{
    tesseract::RoomExportProgress p;
    p.finalizing = true;
    p.assembly_done = 10;
    p.assembly_total = 10;
    const auto fraction = ExportHistoryDialog::compute_progress_fraction(p);
    REQUIRE(fraction.has_value());
    CHECK(*fraction == Catch::Approx(1.0f));
}

TEST_CASE("ExportHistoryDialog: compute_progress_fraction is indeterminate when finalizing with no known total",
          "[history_export][dialog]")
{
    tesseract::RoomExportProgress p;
    p.finalizing = true;
    p.assembly_done = 0;
    p.assembly_total = 0;
    CHECK_FALSE(ExportHistoryDialog::compute_progress_fraction(p).has_value());
}

TEST_CASE("ExportHistoryDialog: select_progress_display_ts uses the oldest message while gathering",
          "[history_export][dialog]")
{
    // Not yet reached_start: show the true oldest message written so far,
    // even if it's later than room creation.
    const auto p = gathering_progress(/*created*/ 100, /*newest*/ 1000, /*oldest*/ 400);
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 400);
}

TEST_CASE("ExportHistoryDialog: select_progress_display_ts snaps to room creation once reached_start",
          "[history_export][dialog]")
{
    // A room can sit quiet for a while after creation before the first
    // message — so the true oldest message (400) can be well after room
    // creation (100). Once gathering is authoritatively done, the display
    // should read the same as "Exporting back to {room creation}" instead
    // of stopping short of it.
    const auto p = gathering_progress(/*created*/ 100, /*newest*/ 1000, /*oldest*/ 400, /*reached_start*/ true);
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 100);
}

TEST_CASE("ExportHistoryDialog: select_progress_display_ts falls back to oldest_ts_ms if room_created_ts_ms is unknown",
          "[history_export][dialog]")
{
    auto p = gathering_progress(/*created*/ 0, /*newest*/ 1000, /*oldest*/ 400, /*reached_start*/ true);
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 400);
}

TEST_CASE("ExportHistoryDialog: select_progress_display_ts stays snapped to room creation while finalizing",
          "[history_export][dialog]")
{
    // reached_start is still true in every progress tick fired once
    // finalizing begins (mod.rs's assembly-section emit_progress calls) —
    // the date must stay correct there too, not just on the single
    // (easily-superseded) tick where reached_start first became true.
    auto p = gathering_progress(/*created*/ 100, /*newest*/ 1000, /*oldest*/ 400, /*reached_start*/ true);
    p.finalizing = true;
    p.assembly_done = 3;
    p.assembly_total = 10;
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 100);
}

TEST_CASE("ExportHistoryDialog: format_short_date renders YYYY-MM-DD",
          "[history_export][dialog]")
{
    // format_short_date renders in *local* time (unlike the export
    // document's own UTC timestamps — see the function's doc comment), so
    // asserting an exact date here would be timezone-dependent and flaky
    // across machines/CI. Check the shape instead: 10 chars, dashes at the
    // right positions, digits everywhere else.
    const std::string date = ExportHistoryDialog::format_short_date(1'700'000'000'000ULL);
    REQUIRE(date.size() == 10);
    CHECK(date[4] == '-');
    CHECK(date[7] == '-');
    for (std::size_t i = 0; i < date.size(); ++i)
    {
        if (i == 4 || i == 7) continue;
        CHECK(std::isdigit(static_cast<unsigned char>(date[i])));
    }
}
