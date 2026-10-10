#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/ExportHistoryDialog.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::views::ExportHistoryDialog;

namespace
{

struct EhdStage : vt::Stage
{
    std::unique_ptr<ExportHistoryDialog> dlg =
        tk::create_root_widget<ExportHistoryDialog>(&host);
    int layouts = 0;
    EhdStage()
    {
        dlg->on_layout_changed = [this] { ++layouts; };
        host.set_root(dlg.get());
    }
    void run() { relayout_paint(*dlg, {0, 0, 800, 600}); }
    tk::ComboBox* format() { return vt::find_all<tk::ComboBox>(*dlg).at(0); }
    tk::ComboBox* range() { return vt::find_all<tk::ComboBox>(*dlg).at(1); }
    tk::CheckButton* images() { return vt::find_all<tk::CheckButton>(*dlg).at(0); }
    tk::CheckButton* zip() { return vt::find_all<tk::CheckButton>(*dlg).at(1); }
};

} // namespace

TEST_CASE("ExportHistoryDialog is hidden until opened and queries for a resume "
          "checkpoint",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    CHECK_FALSE(s.dlg->visible());
    CHECK_FALSE(s.dlg->access_modal());
    std::string queried;
    s.dlg->on_query_resume = [&](std::string r) { queried = r; };
    s.dlg->open("!r:x", "Room X");
    CHECK(s.dlg->is_open());
    CHECK(s.dlg->access_modal());
    CHECK(queried == "!r:x");
    CHECK(s.layouts == 1);
    CHECK(s.dlg->access_role() == tk::Role::Dialog);
    CHECK(s.dlg->access_name().find("Room X") != std::string::npos);
    s.run();
    s.dlg->open("!r:x", "Room X"); // already open: no extra layout notification
    CHECK(s.layouts == 1);
}

TEST_CASE("ExportHistoryDialog Export builds a request from the options",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    s.dlg->open("!r:x", "Room X");
    s.run();
    ExportHistoryDialog::Request got;
    int n = 0;
    s.dlg->on_export_requested = [&](ExportHistoryDialog::Request r)
    {
        got = std::move(r);
        ++n;
    };

    s.images()->set_checked(true);
    s.images()->on_change(true);
    s.zip()->set_checked(true);
    s.zip()->on_change(true);
    s.range()->on_changed("7d");
    REQUIRE(vt::press(*s.dlg, "Export"));
    REQUIRE(n == 1);
    CHECK(got.room_id == "!r:x");
    CHECK(got.format == ExportHistoryDialog::Format::Html);
    CHECK(got.zip_output);
    CHECK(got.include_images);
    CHECK(got.stop_at_ts_ms > 0);

    // Plain text forces include_images off and disables the checkbox.
    s.format()->on_changed("txt");
    CHECK_FALSE(s.images()->checked());
    CHECK_FALSE(s.images()->enabled());
    s.range()->on_changed("all");
    vt::press(*s.dlg, "Export");
    CHECK(got.format == ExportHistoryDialog::Format::Text);
    CHECK_FALSE(got.include_images);
    CHECK(got.stop_at_ts_ms == 0);
    s.format()->on_changed("html");
    CHECK(s.images()->enabled());
}

TEST_CASE("ExportHistoryDialog resume checkpoint offers resume or start over",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    s.dlg->open("!r:x", "Room X");
    tesseract::RoomExportCheckpoint other;
    other.exists = true;
    other.room_id = "!other:x";
    s.dlg->set_resume_checkpoint(other); // wrong room: ignored
    s.run();
    CHECK(vt::has_name(*s.dlg, "Export"));

    tesseract::RoomExportCheckpoint cp;
    cp.exists = true;
    cp.room_id = "!r:x";
    cp.oldest_event_id = "$old";
    cp.events_written = 42;
    s.dlg->set_resume_checkpoint(cp);
    s.run();
    CHECK(s.dlg->access_description().find("42") != std::string::npos);
    CHECK_FALSE(vt::has_name(*s.dlg, "Export"));

    ExportHistoryDialog::Request got;
    s.dlg->on_export_requested = [&](ExportHistoryDialog::Request r) { got = std::move(r); };
    REQUIRE(vt::press(*s.dlg, "Resume export"));
    CHECK(got.resume_from_event_id == "$old");

    REQUIRE(vt::press(*s.dlg, "Start new export"));
    s.run();
    CHECK(vt::has_name(*s.dlg, "Export"));
    CHECK_FALSE(vt::has_name(*s.dlg, "Resume export"));
}

TEST_CASE("ExportHistoryDialog progress: gathering, finalizing, stop/cancel, "
          "silence watchdog",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    s.dlg->open("!r:x", "Room X");
    int stops = 0, cancels = 0;
    s.dlg->on_stop_requested = [&] { ++stops; };
    s.dlg->on_cancel_requested = [&] { ++cancels; };

    tesseract::RoomExportProgress p;
    p.room_id = "!r:x";
    p.events_written = 10;
    p.oldest_ts_ms = 1700000000000ull;
    p.room_created_ts_ms = 1600000000000ull;
    s.dlg->show_progress(p); // Options -> InProgress
    s.run();
    CHECK(s.dlg->access_name().find("Room X") != std::string::npos);
    CHECK(s.dlg->access_description().find("Exporting back to") != std::string::npos);
    REQUIRE(vt::press(*s.dlg, "Stop & save"));
    REQUIRE(vt::press(*s.dlg, "Cancel"));
    CHECK(stops == 1);
    CHECK(cancels == 1);

    // The silence watchdog flips the bar to indeterminate when no newer tick.
    REQUIRE_FALSE(s.host.pending_delays_.empty());
    s.host.fire_all_delays();
    s.run();

    p.reached_start = true;
    p.events_written = 0;
    s.dlg->show_progress(p);
    p.events_written = 55;
    p.finalizing = true;
    p.assembly_done = 3;
    p.assembly_total = 10;
    s.dlg->show_progress(p);
    s.run();
    // Finalizing hides Stop/Cancel.
    CHECK_FALSE(vt::has_name(*s.dlg, "Stop & save"));
    p.assembly_total = 0;
    s.dlg->show_progress(p);
    p.oldest_ts_ms = 0;
    p.finalizing = false;
    p.room_created_ts_ms = 0;
    s.dlg->show_progress(p);
    s.run();
    s.host.fire_all_delays(); // stale watchdogs are ignored / last one fires
}

TEST_CASE("ExportHistoryDialog completion states and the minimum progress "
          "duration",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    ExportHistoryDialog::Request unused;
    s.dlg->open_in_progress("!r:x", "Room X", {});
    s.run();

    // Finishing straight away is deferred so the progress view doesn't flash.
    s.dlg->show_finished(true, false, "/tmp/out", 5, "");
    CHECK(s.dlg->access_name().find("Room X") != std::string::npos);
    s.host.fire_all_delays();
    CHECK(s.dlg->access_name() == "Export complete");
    CHECK(s.dlg->access_description() == "/tmp/out");
    s.run();
    REQUIRE(vt::press(*s.dlg, "Close"));
    CHECK_FALSE(s.dlg->is_open());

    // A new export started meanwhile makes the stale completion a no-op.
    s.dlg->open_in_progress("!r:x", "Room X", {});
    s.dlg->show_finished(false, false, "", 0, "disk full");
    s.dlg->open_in_progress("!r:x", "Room X", {});
    s.host.fire_all_delays();
    CHECK(s.dlg->access_name().find("Room X") != std::string::npos);

    // Outside InProgress, completion applies immediately.
    s.dlg->open("!r:x", "Room X");
    s.dlg->show_finished(false, false, "", 0, "disk full");
    CHECK(s.dlg->access_name() == "Export failed");
    CHECK(s.dlg->access_description() == "disk full");
    s.dlg->show_finished(false, true, "", 0, "");
    CHECK(s.dlg->access_name() == "Export cancelled");
    s.run();
}

TEST_CASE("ExportHistoryDialog busy-elsewhere navigates to the other export",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    std::string go;
    s.dlg->on_go_to_other_export = [&](std::string id) { go = id; };
    s.dlg->open_busy_elsewhere("!busy:x", "Busy Room", {});
    s.run();
    CHECK(s.dlg->access_name() == "Export in progress");
    CHECK(s.dlg->access_description().find("Busy Room") != std::string::npos);
    REQUIRE(vt::press(*s.dlg, "Go to that export"));
    CHECK(go == "!busy:x");
}

TEST_CASE("ExportHistoryDialog backdrop click closes, card click does not",
          "[tk][view][export_dialog]")
{
    EhdStage s;
    s.dlg->open("!r:x", "Room X");
    s.run();
    CHECK_FALSE(s.dlg->on_pointer_down({400, 300})); // inside the card
    CHECK(s.dlg->on_pointer_down({5, 5}));
    s.dlg->on_pointer_up({5, 5}, false);
    CHECK(s.dlg->is_open());
    s.dlg->on_pointer_down({5, 5});
    s.dlg->on_pointer_up({5, 5}, true);
    CHECK_FALSE(s.dlg->is_open());
    CHECK_FALSE(s.dlg->on_pointer_down({5, 5})); // closed: ignores input
}

TEST_CASE("ExportHistoryDialog static helpers", "[tk][view][export_dialog]")
{
    constexpr std::uint64_t kDay = 24ull * 60 * 60 * 1000;
    const std::uint64_t now = 400 * kDay;
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("all", now) == 0);
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("bogus", now) == 0);
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("24h", now) == now - kDay);
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("30d", now) == now - 30 * kDay);
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("365d", now) == now - 365 * kDay);
    CHECK(ExportHistoryDialog::resolve_stop_at_ts_ms("365d", kDay) == 0);

    tesseract::RoomExportProgress p;
    p.oldest_ts_ms = 5;
    p.room_created_ts_ms = 9;
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 5);
    p.reached_start = true;
    CHECK(ExportHistoryDialog::select_progress_display_ts(p) == 9);
    CHECK(ExportHistoryDialog::format_short_date(86400ull * 1000 * 365).size() == 10);
}
