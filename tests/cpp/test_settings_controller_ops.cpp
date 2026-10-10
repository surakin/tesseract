// SettingsController operations beyond the device-list basics already covered
// in test_settings_controller.cpp: avatar, display name, notification
// settings, image packs and room-key import/export. Run with inline executors
// against (a) no client and (b) a session-less Client, so every result is the
// deterministic "not logged in"/failure path.

#include <catch2/catch_test_macros.hpp>

#include "app/SettingsController.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::SettingsController;

namespace
{

struct ScRig
{
    int posts = 0;
    int runs = 0;
    bool inline_run = true;
    std::vector<std::function<void()>> deferred;
    std::function<void(std::vector<uint8_t>, std::string)> picker_cb;
    bool picker_called = false;
    std::unique_ptr<SettingsController> ctl;

    explicit ScRig(tesseract::Client* client, bool defer_run = false)
        : inline_run(!defer_run)
    {
        ctl = std::make_unique<SettingsController>(
            client,
            [this](std::function<void()> fn)
            {
                ++posts;
                fn();
            },
            [this](std::function<void()> fn)
            {
                ++runs;
                if (inline_run)
                {
                    fn();
                }
                else
                {
                    deferred.push_back(std::move(fn));
                }
            },
            [this](std::function<void(std::vector<uint8_t>, std::string)> cb)
            {
                picker_called = true;
                picker_cb = std::move(cb);
            },
            [](const std::vector<uint8_t>&) { return std::shared_ptr<tk::Image>(); });
    }
};

} // namespace

TEST_CASE("SettingsController: avatar upload without a client reports an error",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    bool ok = true;
    int results = 0;
    r.ctl->on_avatar_result = [&](bool o, std::string)
    {
        ok = o;
        ++results;
    };
    r.ctl->upload_avatar();
    CHECK(results == 1);
    CHECK_FALSE(ok);
    CHECK_FALSE(r.picker_called);
    // The in-flight flag was released: a second attempt reports again.
    r.ctl->upload_avatar();
    CHECK(results == 2);
}

TEST_CASE("SettingsController: avatar upload flow with a picker result",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    int results = 0, previews = 0, changed = 0;
    bool last_ok = true;
    r.ctl->on_avatar_result = [&](bool o, std::string)
    {
        last_ok = o;
        ++results;
    };
    r.ctl->on_avatar_preview = [&](std::shared_ptr<tk::Image>) { ++previews; };
    r.ctl->on_avatar_changed = [&](std::string) { ++changed; };

    r.ctl->upload_avatar();
    REQUIRE(r.picker_called);

    // A second upload while the picker is open is ignored.
    r.picker_called = false;
    r.ctl->upload_avatar();
    CHECK_FALSE(r.picker_called);

    r.picker_cb({1, 2, 3}, "image/png");
    CHECK(previews == 1);
    CHECK(results == 1);
    CHECK_FALSE(last_ok); // no session: the upload fails
    CHECK(changed == 0);
}

TEST_CASE("SettingsController: cancelling the avatar picker clears the busy flag",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    r.ctl->upload_avatar();
    REQUIRE(r.picker_called);
    r.picker_cb({}, ""); // cancel
    r.picker_called = false;
    r.ctl->upload_avatar();
    CHECK(r.picker_called);
}

TEST_CASE("SettingsController: dropping the picker callback unblocks uploads",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    r.ctl->upload_avatar();
    REQUIRE(r.picker_called);
    r.picker_cb = nullptr; // dialog destroyed without being invoked
    r.picker_called = false;
    r.ctl->upload_avatar();
    CHECK(r.picker_called);
}

TEST_CASE("SettingsController: avatar picked after the client changed is dropped",
          "[settings][controller][ops]")
{
    tesseract::Client a, b;
    ScRig r(&a);
    int results = 0;
    r.ctl->on_avatar_result = [&](bool, std::string) { ++results; };
    r.ctl->upload_avatar();
    REQUIRE(r.picker_called);
    r.ctl->set_client(&b);
    r.picker_cb({1}, "image/png");
    CHECK(results == 0);
}

TEST_CASE("SettingsController: remove_avatar", "[settings][controller][ops]")
{
    {
        ScRig r(nullptr);
        int results = 0;
        r.ctl->on_avatar_result = [&](bool ok, std::string)
        {
            CHECK_FALSE(ok);
            ++results;
        };
        r.ctl->remove_avatar();
        CHECK(results == 1);
    }
    {
        tesseract::Client client;
        ScRig r(&client);
        int results = 0, changed = 0;
        r.ctl->on_avatar_result = [&](bool ok, std::string)
        {
            CHECK_FALSE(ok);
            ++results;
        };
        r.ctl->on_avatar_changed = [&](std::string) { ++changed; };
        r.ctl->remove_avatar();
        CHECK(results == 1);
        CHECK(changed == 0);
    }
}

TEST_CASE("SettingsController: display name with a session-less client fails",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    int results = 0, renamed = 0;
    r.ctl->on_name_result = [&](bool ok, std::string)
    {
        CHECK_FALSE(ok);
        ++results;
    };
    r.ctl->on_name_changed = [&](std::string) { ++renamed; };
    r.ctl->set_display_name("Alice");
    CHECK(results == 1);
    CHECK(renamed == 0);
}

TEST_CASE("SettingsController: results for a replaced client are dropped",
          "[settings][controller][ops]")
{
    tesseract::Client a, b;
    ScRig r(&a, /*defer_run=*/true);
    int results = 0;
    r.ctl->on_name_result = [&](bool, std::string) { ++results; };
    r.ctl->on_mentions_toggle_result = [&](bool, bool) { ++results; };
    r.ctl->set_display_name("x");
    r.ctl->set_mentions_enabled(true);
    REQUIRE(r.deferred.size() == 2);
    r.ctl->set_client(&b);
    for (auto& fn : r.deferred)
    {
        fn();
    }
    CHECK(results == 0);
}

TEST_CASE("SettingsController: mentions settings default when logged out",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    bool called = false;
    r.ctl->on_mentions_settings_loaded = [&](bool m, bool rm, bool all, bool kw,
                                             std::vector<std::string> words)
    {
        called = true;
        CHECK(m);
        CHECK(rm);
        CHECK(all);
        CHECK_FALSE(kw);
        CHECK(words.empty());
    };
    r.ctl->load_mentions_settings();
    CHECK(called);
}

TEST_CASE("SettingsController: notification toggles do nothing without a client",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    int results = 0;
    auto bump2 = [&](bool, bool) { ++results; };
    auto bump_s = [&](bool, std::string) { ++results; };
    r.ctl->on_mentions_toggle_result = bump2;
    r.ctl->on_room_mentions_toggle_result = bump2;
    r.ctl->on_notify_all_messages_result = bump2;
    r.ctl->on_notify_on_keywords_result = bump2;
    r.ctl->on_keyword_add_result = bump_s;
    r.ctl->on_keyword_remove_result = bump_s;
    r.ctl->set_mentions_enabled(true);
    r.ctl->set_room_mentions_enabled(true);
    r.ctl->set_notify_all_messages(true);
    r.ctl->set_notify_on_keywords(true);
    r.ctl->add_notification_keyword("kw");
    r.ctl->remove_notification_keyword("kw");
    r.ctl->set_pack_subscribed("!r:x", "key", true);
    CHECK(results == 0);
    CHECK(r.runs == 0);
}

TEST_CASE("SettingsController: notification toggles report failure on a "
          "session-less client",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    std::vector<std::string> log;
    r.ctl->on_mentions_toggle_result = [&](bool ok, bool en)
    { log.push_back(std::string("m") + (ok ? "1" : "0") + (en ? "1" : "0")); };
    r.ctl->on_room_mentions_toggle_result = [&](bool ok, bool en)
    { log.push_back(std::string("r") + (ok ? "1" : "0") + (en ? "1" : "0")); };
    r.ctl->on_notify_all_messages_result = [&](bool ok, bool en)
    { log.push_back(std::string("a") + (ok ? "1" : "0") + (en ? "1" : "0")); };
    r.ctl->on_notify_on_keywords_result = [&](bool ok, bool en)
    { log.push_back(std::string("k") + (ok ? "1" : "0") + (en ? "1" : "0")); };
    r.ctl->on_keyword_add_result = [&](bool ok, std::string kw)
    { log.push_back("add:" + std::string(ok ? "1" : "0") + kw); };
    r.ctl->on_keyword_remove_result = [&](bool ok, std::string kw)
    { log.push_back("rm:" + std::string(ok ? "1" : "0") + kw); };

    r.ctl->set_mentions_enabled(true);
    r.ctl->set_room_mentions_enabled(false);
    r.ctl->set_notify_all_messages(true);
    r.ctl->set_notify_on_keywords(false);
    r.ctl->add_notification_keyword("word");
    r.ctl->remove_notification_keyword("word");
    CHECK(log == std::vector<std::string>{"m01", "r00", "a01", "k00",
                                          "add:0word", "rm:0word"});
}

TEST_CASE("SettingsController: loading mentions on a session-less client "
          "delivers once",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    int loaded = 0;
    r.ctl->on_mentions_settings_loaded =
        [&](bool, bool, bool, bool, std::vector<std::string>) { ++loaded; };
    r.ctl->load_mentions_settings();
    r.ctl->load_mentions_settings();
    CHECK(loaded == 2); // flag released after each completed load
}

TEST_CASE("SettingsController: image packs", "[settings][controller][ops]")
{
    {
        ScRig r(nullptr);
        int packs = 0, imgs = 0, saves = 0;
        r.ctl->on_image_packs_loaded = [&](std::vector<tesseract::ImagePack>)
        { ++packs; };
        r.ctl->on_user_pack_images_loaded =
            [&](std::vector<tesseract::ImagePackImage>) { ++imgs; };
        r.ctl->on_user_pack_save_result = [&](bool ok, std::string)
        {
            CHECK_FALSE(ok);
            ++saves;
        };
        r.ctl->load_image_packs();
        r.ctl->save_user_pack_changes({});
        CHECK(packs == 1);
        CHECK(imgs == 1);
        CHECK(saves == 1);
    }
    {
        tesseract::Client client;
        ScRig r(&client);
        int packs = 0, imgs = 0;
        r.ctl->on_image_packs_loaded = [&](std::vector<tesseract::ImagePack> p)
        {
            CHECK(p.empty());
            ++packs;
        };
        r.ctl->on_user_pack_images_loaded =
            [&](std::vector<tesseract::ImagePackImage> i)
        {
            CHECK(i.empty());
            ++imgs;
        };
        r.ctl->load_image_packs();
        CHECK(packs == 1);
        CHECK(imgs == 1);
        // A pack subscription that fails does not reload.
        r.ctl->set_pack_subscribed("!r:x", "key", true);
        CHECK(packs == 1);
    }
}

TEST_CASE("SettingsController: saving user pack changes aggregates the first error",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    std::vector<std::pair<bool, std::string>> results;
    r.ctl->on_user_pack_save_result = [&](bool ok, std::string e)
    { results.emplace_back(ok, e); };

    tesseract::views::UserPackEditor::Result diff;
    diff.removed_shortcodes = {"gone"};
    tesseract::views::StagedPackImage existing;
    existing.shortcode = "kept";
    existing.existing_url = "mxc://x/kept";
    existing.favorite = true;
    tesseract::views::StagedPackImage fresh;
    fresh.shortcode = "new";
    fresh.pending_bytes = {1, 2, 3};
    fresh.pending_mime = "image/png";
    diff.images = {existing, fresh};
    r.ctl->save_user_pack_changes(std::move(diff));

    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].first);
    CHECK_FALSE(results[0].second.empty());
}

TEST_CASE("SettingsController: device deletion needing no client is released",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    int deleted = 0;
    r.ctl->on_device_deleted = [&](std::string, bool ok, std::string)
    {
        CHECK_FALSE(ok);
        ++deleted;
    };
    r.ctl->delete_device("D1");
    r.ctl->delete_device("D1"); // not blocked: the op was released
    r.ctl->confirm_device_deletion("D1", "sess");
    CHECK(deleted == 3);
    // cancel releases without a callback.
    r.ctl->cancel_device_deletion("D1");
}

TEST_CASE("SettingsController: device deletion on a session-less client fails "
          "and releases the slot",
          "[settings][controller][ops]")
{
    tesseract::Client client;
    ScRig r(&client);
    int deleted = 0, uia = 0, renamed = 0;
    r.ctl->on_device_deleted = [&](std::string, bool, std::string) { ++deleted; };
    r.ctl->on_device_needs_uia =
        [&](std::string, std::string, std::string) { ++uia; };
    r.ctl->on_device_renamed = [&](std::string, bool ok, std::string)
    {
        CHECK_FALSE(ok);
        ++renamed;
    };
    r.ctl->delete_device("D1");
    r.ctl->confirm_device_deletion("D1", "sess");
    r.ctl->rename_device("D1", "name");
    CHECK(deleted == 2);
    CHECK(uia == 0);
    CHECK(renamed == 1);
}

TEST_CASE("SettingsController: room key export/import dialog flow",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    // Without the dialogs wired both are no-ops.
    CHECK_NOTHROW(r.ctl->export_room_keys());
    CHECK_NOTHROW(r.ctl->import_room_keys());

    std::string prompted;
    std::function<void(std::string)> pass_cb;
    r.ctl->show_passphrase_prompt = [&](std::string title,
                                        std::function<void(std::string)> cb)
    {
        prompted = title;
        pass_cb = std::move(cb);
    };
    std::function<void(std::string)> save_cb;
    r.ctl->show_save_file_dialog = [&](std::string suggested,
                                       std::function<void(std::string)> cb)
    {
        CHECK(suggested == "room-keys.txt");
        save_cb = std::move(cb);
    };
    std::function<void(std::string)> open_cb;
    r.ctl->show_open_file_dialog = [&](std::function<void(std::string)> cb)
    { open_cb = std::move(cb); };

    int exports = 0, imports = 0;
    r.ctl->on_export_keys_result = [&](bool ok, std::string)
    {
        CHECK_FALSE(ok);
        ++exports;
    };
    r.ctl->on_import_keys_result = [&](bool ok, std::string)
    {
        CHECK_FALSE(ok);
        ++imports;
    };

    // Export: empty passphrase aborts, empty path aborts, full flow reports.
    r.ctl->export_room_keys();
    REQUIRE(pass_cb);
    pass_cb("");
    CHECK_FALSE(save_cb);
    pass_cb("secret");
    REQUIRE(save_cb);
    save_cb("");
    CHECK(exports == 0);
    save_cb("/tmp/keys.txt");
    CHECK(exports == 1);

    // Import.
    pass_cb = nullptr;
    r.ctl->import_room_keys();
    REQUIRE(open_cb);
    open_cb("");
    CHECK_FALSE(pass_cb);
    open_cb("/tmp/keys.txt");
    REQUIRE(pass_cb);
    pass_cb("");
    CHECK(imports == 0);
    pass_cb("secret");
    CHECK(imports == 1);
}

TEST_CASE("SettingsController: notifications toggle without a connector is safe",
          "[settings][controller][ops]")
{
    ScRig r(nullptr);
    CHECK_NOTHROW(r.ctl->set_notifications_enabled(true));
    CHECK_NOTHROW(r.ctl->set_notifications_enabled(false));
}
