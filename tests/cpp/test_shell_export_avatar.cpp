// ShellBase_settings.cpp: the history-export controller wiring (dialog requests
// into the controller, controller status back into the dialog / status line),
// picking a room avatar for yourself, staging a room-settings avatar upload,
// and building the settings / export controllers.

#include <catch2/catch_test_macros.hpp>

#include "app/HistoryExportController.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk_test_surface.h"
#include "views/ExportHistoryDialog.h"
#include "views/MainAppWidget.h"
#include "views/RoomSettingsView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct EaShell : tesseract::test::TestShellBase
{
    ~EaShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        history_export_controller_.reset();
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void on_restore_status_ui_() override { ++restores; }
    void navigate_to_room_(const std::string& id) override { navigated.push_back(id); }
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) override
    {
        picker = std::move(cb);
    }
    void bind_settings_controller_() override { ++binds; }
    void request_repaint_() override { ++repaints; }
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }

    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::string> statuses, navigated;
    std::function<void(std::vector<uint8_t>, std::string)> picker;
    int restores = 0, binds = 0, repaints = 0;

    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::ensure_history_export_controller_;
    using ShellBase::ensure_settings_controller_;
    using ShellBase::history_export_controller_;
    using ShellBase::main_app_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::on_persistent_status_activate_;
    using ShellBase::pick_and_set_room_avatar_;
    using ShellBase::rooms_;
    using ShellBase::settings_controller_;
    using ShellBase::stage_room_settings_avatar_upload_;
    using ShellBase::status_msg_gen_;
    using ShellBase::status_override_active_;
};

struct EaFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    EaShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<tesseract::views::MainAppWidget> app =
        tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    EaFx()
    {
        s.main_app_ = app.get();
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.client_ = s.active_account_->client.get();
        s.my_user_id_ = "@me:x";
        tesseract::RoomInfo info;
        info.id = "!r:x";
        info.name = "Room";
        s.rooms_ = {info};
        s.mark_room_index_dirty_();
    }
};

} // namespace

TEST_CASE("the settings controller is rebuilt and bound on every call",
          "[shell][export_avatar]")
{
    EaFx f;
    f.s.ensure_settings_controller_();
    REQUIRE(f.s.settings_controller_ != nullptr);
    CHECK(f.s.binds == 1);
    f.s.ensure_settings_controller_();
    CHECK(f.s.binds == 2);
}

TEST_CASE("the export controller reports status through the line and the dialog",
          "[shell][export_avatar][export]")
{
    EaFx f;
    f.s.ensure_history_export_controller_();
    auto& c = *f.s.history_export_controller_;
    REQUIRE(c.on_started);

    c.on_started("!r:x", "/tmp/out");
    f.s.pump();
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back().find("Exporting history") != std::string::npos);
    REQUIRE(f.s.on_persistent_status_activate_);

    // Clicking the persistent status jumps to the exporting room.
    f.s.on_persistent_status_activate_();
    CHECK(f.s.navigated == std::vector<std::string>{"!r:x"});

    tesseract::RoomExportProgress p;
    p.events_written = 42;
    c.on_progress(p);
    f.s.pump();
    CHECK(f.s.statuses.back().find("42") != std::string::npos);

    f.s.status_override_active_ = true;
    c.on_finished(true, false, "/tmp/out.html", 42, "");
    CHECK_FALSE(f.s.on_persistent_status_activate_);
    CHECK_FALSE(f.s.status_override_active_);
    CHECK(f.s.restores == 1);

    c.on_finished(false, true, "", 0, "cancelled"); // nothing left to restore
    CHECK(f.s.restores == 1);

    tesseract::RoomExportCheckpoint cp;
    cp.room_id = "!r:x";
    c.on_resume_available(cp);
}

TEST_CASE("dialog requests are forwarded to the controller", "[shell][export_avatar][export]")
{
    EaFx f;
    f.s.ensure_history_export_controller_();
    auto* dlg = f.app->export_history_dialog();
    REQUIRE(dlg != nullptr);
    REQUIRE(dlg->on_query_resume);
    dlg->on_query_resume("!r:x");
    dlg->on_cancel_requested();
    dlg->on_stop_requested();

    tesseract::views::ExportHistoryDialog::Request req;
    req.room_id = "!r:x";
    req.format = tesseract::views::ExportHistoryDialog::Format::Html;
    req.include_images = true;
    dlg->on_export_requested(req);
    req.room_id = "!unnamed:x"; // unknown room: id doubles as display name
    req.format = tesseract::views::ExportHistoryDialog::Format::Text;
    dlg->on_export_requested(req);

    dlg->on_go_to_other_export("!r:x");
    CHECK(f.s.navigated == std::vector<std::string>{"!r:x"});
    f.s.pump();
}

TEST_CASE("the export controller needs no dialog to be built", "[shell][export_avatar][export]")
{
    EaShell s;
    s.ensure_history_export_controller_();
    CHECK(s.history_export_controller_ != nullptr);
}

TEST_CASE("picking a room avatar uploads it and reports the outcome",
          "[shell][export_avatar][avatar]")
{
    EaFx f;
    f.s.pick_and_set_room_avatar_("!r:x", nullptr);
    REQUIRE(f.s.picker);

    f.s.picker({}, ""); // cancelled: nothing happens
    f.s.pump();
    CHECK(f.s.statuses.empty());

    f.s.picker({1, 2, 3}, "image/png");
    f.s.pump(); // the upload fails offline
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back().rfind("Failed to upload avatar:", 0) == 0);
}

TEST_CASE("a picked avatar is dropped if the account logged out meanwhile",
          "[shell][export_avatar][avatar]")
{
    EaFx f;
    f.s.pick_and_set_room_avatar_("!r:x", nullptr);
    REQUIRE(f.s.picker);
    f.s.active_account_.reset(); // the session (and its client) go away
    f.s.client_ = nullptr;
    f.s.picker({1, 2, 3}, "image/png");
    f.s.pump();
    CHECK(f.s.statuses.empty());

    EaShell none;
    none.pick_and_set_room_avatar_("!r:x", nullptr); // no client: no picker
    CHECK_FALSE(none.picker);
}

TEST_CASE("staging a room-settings avatar marks busy, previews, and honours a "
          "closed dialog",
          "[shell][export_avatar][avatar]")
{
    EaFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    auto* settings = rv->room_settings_view();
    REQUIRE(settings != nullptr);

    f.s.stage_room_settings_avatar_upload_("!r:x", nullptr, nullptr); // no target
    CHECK_FALSE(f.s.picker);

    settings->open(info);
    f.s.stage_room_settings_avatar_upload_("!r:x", settings, nullptr);
    REQUIRE(f.s.picker);

    f.s.picker({}, ""); // cancelled: busy cleared and repainted
    CHECK(f.s.repaints >= 1);

    f.s.stage_room_settings_avatar_upload_("!r:x", settings, nullptr);
    f.s.picker({1, 2, 3}, "image/png");
    f.s.pump();

    // A dialog reopened meanwhile ignores the stale preview.
    f.s.stage_room_settings_avatar_upload_("!r:x", settings, nullptr);
    auto cb = f.s.picker;
    settings->open(info); // bumps the generation
    cb({1, 2, 3}, "image/png");
    f.s.pump();

    // Logged out between pick and callback.
    f.s.stage_room_settings_avatar_upload_("!r:x", settings, nullptr);
    cb = f.s.picker;
    f.s.active_account_.reset();
    f.s.client_ = nullptr;
    cb({1}, "image/png");
    f.s.pump();
}
