#include <catch2/catch_test_macros.hpp>
#include "app/HistoryExportController.h"
#include <functional>
#include <string>
#include <vector>

using tesseract::HistoryExportController;

TEST_CASE("HistoryExportController: build_labels has the exact count the Rust side expects",
          "[history_export][controller]")
{
    // Must track history_export::labels::ExportLabel::COUNT (32) on the
    // Rust side exactly — a mismatch silently misattributes prose to the
    // wrong slot rather than failing loudly, so both sides pin the count.
    CHECK(HistoryExportController::build_labels().size() == 32);
}

TEST_CASE("HistoryExportController: build_labels has no empty entries",
          "[history_export][controller]")
{
    // tk::tr() falls back to the English source string when no .po entry
    // exists for the current locale, but it should never return an empty
    // string for a non-empty source literal.
    for (const auto& label : HistoryExportController::build_labels())
        CHECK_FALSE(label.empty());
}

TEST_CASE("HistoryExportController: extension_for maps format to file extension",
          "[history_export][controller]")
{
    CHECK(HistoryExportController::extension_for(HistoryExportController::Format::Text) == "txt");
    CHECK(HistoryExportController::extension_for(HistoryExportController::Format::Html) == "html");
}

TEST_CASE("HistoryExportController: suggested_folder_name sanitizes illegal characters",
          "[history_export][controller]")
{
    HistoryExportController::Request req;
    req.room_display_name = "Team: Alpha/Beta?";
    const auto name = HistoryExportController::suggested_folder_name(req);
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find(':') == std::string::npos);
    CHECK(name.find('?') == std::string::npos);
    CHECK(name == "Team_ Alpha_Beta_-history");
}

TEST_CASE("HistoryExportController: suggested_folder_name falls back when the room has no name",
          "[history_export][controller]")
{
    HistoryExportController::Request req;
    req.room_display_name = "";
    CHECK(HistoryExportController::suggested_folder_name(req) == "room-history");
}

TEST_CASE("HistoryExportController: begin() is a no-op without a save-folder dialog wired",
          "[history_export][controller]")
{
    int post_count = 0;
    auto post_inline = [&](std::function<void()> fn) { ++post_count; fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    // show_save_folder_dialog left unset.
    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);

    CHECK_FALSE(ctrl.active());
    CHECK(post_count == 0);
}

TEST_CASE("HistoryExportController: begin() marks itself active until handle_complete",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    req.room_display_name = "Team Alpha";

    bool started = false;
    ctrl.on_started = [&](std::string room_id, std::string path)
    {
        started = true;
        CHECK(room_id == "!room:example.org");
        CHECK(path == "/tmp/export");
    };

    ctrl.begin(req);
    CHECK(started);
    CHECK(ctrl.active());
    CHECK(ctrl.active_room_id() == "!room:example.org");
}

TEST_CASE("HistoryExportController: a second begin() while active is a no-op",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    int dialog_calls = 0;
    ctrl.show_save_folder_dialog =
        [&](std::string, std::function<void(std::string)> cb) { ++dialog_calls; cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);
    CHECK(dialog_calls == 1);

    HistoryExportController::Request other;
    other.room_id = "!other:example.org";
    ctrl.begin(other);
    CHECK(dialog_calls == 1); // second call never even opened the dialog
    CHECK(ctrl.active_room_id() == "!room:example.org");
}

TEST_CASE("HistoryExportController: handle_complete clears active state and fires on_finished",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);
    REQUIRE(ctrl.active());

    bool finished = false;
    ctrl.on_finished = [&](bool ok, bool cancelled, std::string out_path,
                           std::uint64_t events_written, std::string error)
    {
        finished = true;
        CHECK(ok);
        CHECK_FALSE(cancelled);
        CHECK(out_path == "/tmp/export/history.html");
        CHECK(events_written == 42);
        CHECK(error.empty());
    };

    // request_id 1 is the first export this controller ever started.
    ctrl.handle_complete(1, true, false, true, "/tmp/export/history.html", 42, 1234, "");

    CHECK(finished);
    CHECK_FALSE(ctrl.active());
    CHECK(ctrl.active_room_id().empty());
}

TEST_CASE("HistoryExportController: handle_complete for a stale request_id is ignored",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);
    REQUIRE(ctrl.active());

    int finished_count = 0;
    ctrl.on_finished = [&](bool, bool, std::string, std::uint64_t, std::string) { ++finished_count; };

    // A completion for a request that isn't the currently-active one
    // (e.g. a very late callback from an export this controller no longer
    // considers current) must not clear active state or fire on_finished.
    ctrl.handle_complete(999, true, false, true, "/tmp/stale", 1, 1, "");

    CHECK(finished_count == 0);
    CHECK(ctrl.active());
}

TEST_CASE("HistoryExportController: handle_progress updates last_progress and fires on_progress",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);

    int progress_calls = 0;
    ctrl.on_progress = [&](const tesseract::RoomExportProgress&) { ++progress_calls; };

    tesseract::RoomExportProgress p;
    p.request_id = 1;
    p.events_written = 123;
    ctrl.handle_progress(p);

    CHECK(progress_calls == 1);
    CHECK(ctrl.last_progress().events_written == 123);
}

TEST_CASE("HistoryExportController: stop() without an active export is a no-op",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.stop(); // must not crash with no active export and a null client
    CHECK_FALSE(ctrl.active());
}

TEST_CASE("HistoryExportController: stop() while active does not itself clear active state",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);
    REQUIRE(ctrl.active());

    // stop() is a null-guarded pass-through to Client::stop_room_export
    // (client_ is null in this test), same shape as cancel() — active
    // state only clears later, when the matching handle_complete arrives.
    ctrl.stop();
    CHECK(ctrl.active());
}

TEST_CASE("HistoryExportController: handle_progress for a stale request_id is ignored",
          "[history_export][controller]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    HistoryExportController ctrl(nullptr, post_inline, run_inline);
    ctrl.show_save_folder_dialog =
        [](std::string, std::function<void(std::string)> cb) { cb("/tmp/export"); };

    HistoryExportController::Request req;
    req.room_id = "!room:example.org";
    ctrl.begin(req);

    int progress_calls = 0;
    ctrl.on_progress = [&](const tesseract::RoomExportProgress&) { ++progress_calls; };

    tesseract::RoomExportProgress p;
    p.request_id = 999;
    ctrl.handle_progress(p);

    CHECK(progress_calls == 0);
}
