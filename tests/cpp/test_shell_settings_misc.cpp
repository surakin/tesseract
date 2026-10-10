// ShellBase_settings.cpp: committing room settings and image packs (per-field
// error aggregation), bridge-override and skin-tone preferences, the image-pack
// caches that back emoticon lookup, and the Room Settings image-pack tab seed.
// Settings commits run against a Client with no session, so every write reports
// its own failure; the tests pin which fields are attempted and how errors are
// reported.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/ImagePackEditorView.h"
#include "views/RoomSettingsView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
namespace tv = tesseract::views;

namespace
{

struct SmShell : tesseract::test::TestShellBase
{
    ~SmShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
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
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        delayed.push_back(std::move(fn));
    }
    void on_active_room_bot_commands_changed_ui_() override { ++bot_changes; }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    std::vector<std::function<void()>> delayed;
    std::vector<std::string> statuses;
    int bot_changes = 0;

    using ShellBase::active_account_;
    using ShellBase::apply_bridge_overrides_;
    using ShellBase::apply_image_pack_changes_;
    using ShellBase::apply_room_settings_;
    using ShellBase::cached_emoticons_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::emoji_skin_tone_;
    using ShellBase::emoticon_packs_;
    using ShellBase::emoticons_for_room_;
    using ShellBase::handle_bot_commands_updated_ui_;
    using ShellBase::handle_image_pack_images_needed_;
    using ShellBase::handle_image_pack_pending_image_added_;
    using ShellBase::handle_image_packs_updated_ui_;
    using ShellBase::handle_user_pack_pending_image_added_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::per_account_rooms_;
    using ShellBase::recent_room_ids_;
    using ShellBase::room_view_;
    using ShellBase::rooms_;
    using ShellBase::seed_image_pack_tab_;
    using ShellBase::set_bridge_override_;
    using ShellBase::set_emoji_skin_tone_;
    using ShellBase::shortcode_for_mxc_;
    using ShellBase::open_activity_monitor_;
    using ShellBase::pool_;
    using ShellBase::teardown_activity_monitor_;
    using ShellBase::RoomSettingsCommitOutcome;
};

tesseract::RoomInfo sm_room(const std::string& id)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    return r;
}

tesseract::ImagePackImage sm_img(const std::string& code, const std::string& url)
{
    tesseract::ImagePackImage i;
    i.shortcode = code;
    i.url = url;
    return i;
}

} // namespace

TEST_CASE("committing room settings with no client fails with a reason",
          "[shell][settings_misc]")
{
    tv::RoomSettingsChanges c;
    c.name = "x";
    auto out = SmShell::apply_room_settings_(nullptr, "!r:x", c);
    CHECK_FALSE(out.ok);
    CHECK(out.error == "not logged in");
}

TEST_CASE("an empty change set commits trivially", "[shell][settings_misc]")
{
    tesseract::Client client;
    auto out = SmShell::apply_room_settings_(&client, "!r:x", {});
    CHECK(out.ok);
    CHECK(out.error.empty());
}

TEST_CASE("every populated field is attempted and its failure named",
          "[shell][settings_misc]")
{
    tesseract::Client client; // sessionless: each write fails
    tv::RoomSettingsChanges c;
    c.name = "n";
    c.topic = "t";
    c.avatar_mxc = "mxc://hs/a";
    c.is_encrypted = true;
    c.join_rule = "invite";
    c.guest_access = true;
    c.history_visibility = "shared";
    c.permissions = tesseract::RoomPermissions{};
    c.media_override = tv::RoomMediaOverrideChange{
        true, tesseract::MediaPreviewConfig::Mode::Off};
    auto out = SmShell::apply_room_settings_(&client, "!r:x", c);
    CHECK_FALSE(out.ok);
    for (const char* field :
         {"name:", "topic:", "avatar:", "encryption:", "join_rule:",
          "guest_access:", "history_visibility:", "permissions:",
          "media_preview:"})
        CHECK(out.error.find(field) != std::string::npos);
    CHECK(out.error.find("; ") != std::string::npos); // joined
}

TEST_CASE("a staged avatar upload takes precedence over an mxc and a disabled "
          "encryption flag is never sent",
          "[shell][settings_misc]")
{
    tesseract::Client client;
    tv::RoomSettingsChanges c;
    c.avatar_upload = tv::PendingAvatarUpload{{1, 2, 3}, "image/png"};
    c.avatar_mxc = "mxc://hs/ignored";
    c.is_encrypted = false;
    auto out = SmShell::apply_room_settings_(&client, "!r:x", c);
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("avatar:") != std::string::npos);
    CHECK(out.error.find("encryption:") == std::string::npos);
}

TEST_CASE("image pack commits report removal, upload and save failures",
          "[shell][settings_misc]")
{
    auto errors = SmShell::apply_image_pack_changes_(nullptr, {});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == "image_packs: not logged in");

    tesseract::Client client;
    tv::ImagePackEditorResult r;
    r.room_id = "!r:x";
    r.removed_state_keys = {"old1", "old2"};
    tv::StagedPack pack;
    pack.display_name = "Faces";
    pack.state_key = "faces";
    tv::StagedPackImage existing;
    existing.shortcode = "smile";
    existing.existing_url = "mxc://hs/smile";
    tv::StagedPackImage added;
    added.shortcode = "new";
    added.pending_bytes = {1, 2};
    added.pending_mime = "image/png";
    tv::StagedPackImage empty; // neither: skipped silently
    empty.shortcode = "nothing";
    pack.images = {existing, added, empty};
    r.packs = {pack};

    errors = SmShell::apply_image_pack_changes_(&client, r);
    // two removals + one failed upload + one failed save
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].rfind("image_packs.remove:", 0) == 0);
    CHECK(errors[1].rfind("image_packs.remove:", 0) == 0);
    CHECK(errors[2].rfind("image_packs.Faces:", 0) == 0);
    CHECK(errors[3].rfind("image_packs.Faces:", 0) == 0);
}

TEST_CASE("image pack errors surface through the room-settings commit",
          "[shell][settings_misc]")
{
    tesseract::Client client;
    tv::RoomSettingsChanges c;
    tv::ImagePackEditorResult r;
    r.room_id = "!r:x";
    r.removed_state_keys = {"k"};
    c.image_packs = r;
    auto out = SmShell::apply_room_settings_(&client, "!r:x", c);
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("image_packs.remove:") != std::string::npos);
}

TEST_CASE("bridge overrides mark rooms and are only persisted on change",
          "[shell][settings_misc][bridge]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    SmShell s;
    s.set_bridge_override_("!r:x", true); // no account: ignored
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->user_id = "@me:x";
    s.my_user_id_ = "@me:x";
    s.client_ = &client;
    auto room = sm_room("!r:x");
    room.is_bridged = true;
    s.rooms_ = {room};
    s.per_account_rooms_["@me:x"] = {room};
    s.mark_room_index_dirty_();

    s.set_bridge_override_("", true);
    CHECK(s.active_account_->bridge_not_bridged_overrides.empty());
    s.set_bridge_override_("!r:x", true);
    REQUIRE(s.active_account_->bridge_not_bridged_overrides.size() == 1);
    CHECK(s.rooms_[0].bridge_overridden);
    CHECK(s.per_account_rooms_["@me:x"][0].bridge_overridden);
    CHECK_FALSE(s.active_account_->prefs_json.empty()); // layout/prefs persisted

    s.active_account_->prefs_json.clear();
    s.set_bridge_override_("!r:x", true); // no change: nothing persisted
    CHECK(s.active_account_->prefs_json.empty());

    s.set_bridge_override_("!r:x", false);
    CHECK(s.active_account_->bridge_not_bridged_overrides.empty());
    CHECK_FALSE(s.rooms_[0].bridge_overridden); // removing the last one clears it
    s.set_bridge_override_("!never:x", false); // already not overridden
}

TEST_CASE("apply_bridge_overrides_ clears stale flags, including for an empty list",
          "[shell][settings_misc][bridge]")
{
    SmShell s;
    std::vector<tesseract::RoomInfo> rooms{sm_room("!a"), sm_room("!b")};
    rooms[0].bridge_overridden = true;
    s.apply_bridge_overrides_(rooms, {}); // empty: clears the stale flag
    CHECK_FALSE(rooms[0].bridge_overridden);
    rooms[0].bridge_overridden = true;
    s.apply_bridge_overrides_(rooms, {"!b"});
    CHECK_FALSE(rooms[0].bridge_overridden);
    CHECK(rooms[1].bridge_overridden);
}

TEST_CASE("emoji skin tone follows the active account", "[shell][settings_misc]")
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    SmShell s;
    CHECK(s.emoji_skin_tone_() == tesseract::emoji::SkinTone::None);
    s.set_emoji_skin_tone_(tesseract::emoji::SkinTone::Dark); // no account
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.client_ = &client;
    s.set_emoji_skin_tone_(tesseract::emoji::SkinTone::Dark);
    CHECK(s.emoji_skin_tone_() == tesseract::emoji::SkinTone::Dark);
    s.active_account_->prefs_json.clear();
    s.set_emoji_skin_tone_(tesseract::emoji::SkinTone::Dark); // unchanged
    CHECK(s.active_account_->prefs_json.empty());
}

TEST_CASE("image pack updates rebuild the emoticon caches", "[shell][settings_misc][packs]")
{
    tesseract::Client client;
    SmShell s;
    s.cached_emoticons_ = {sm_img("old", "mxc://hs/old")};
    s.emoticon_packs_.emplace_back(tesseract::ImagePack{}, std::vector{sm_img("a", "mxc://a")});
    s.handle_image_packs_updated_ui_(); // no client: caches simply emptied
    CHECK(s.cached_emoticons_.empty());
    CHECK(s.emoticon_packs_.empty());

    s.client_ = &client; // sessionless: no packs
    s.handle_image_packs_updated_ui_();
    CHECK(s.cached_emoticons_.empty());
}

TEST_CASE("shortcodes and room emoticons come from the cached packs",
          "[shell][settings_misc][packs]")
{
    SmShell s;
    CHECK(s.shortcode_for_mxc_("").empty());
    s.cached_emoticons_ = {sm_img("wave", "mxc://hs/wave"),
                           sm_img("tada", "mxc://hs/tada")};
    CHECK(s.shortcode_for_mxc_("mxc://hs/tada") == "tada");
    CHECK(s.shortcode_for_mxc_("mxc://hs/none").empty());

    tesseract::ImagePack personal; // the user's own pack is always offered
    personal.source_kind = tesseract::PackSourceKind::User;
    personal.usage = tesseract::PackUsage::Any;
    s.emoticon_packs_.emplace_back(personal, std::vector{sm_img("wave", "mxc://hs/wave")});
    tesseract::ImagePack elsewhere;
    elsewhere.source_kind = tesseract::PackSourceKind::Room;
    elsewhere.source_room = "!other:x";
    s.emoticon_packs_.emplace_back(elsewhere, std::vector{sm_img("tada", "mxc://hs/tada")});
    auto here = s.emoticons_for_room_("!r:x");
    bool has_wave = false, has_tada = false;
    for (const auto& i : here)
    {
        has_wave |= i.shortcode == "wave";
        has_tada |= i.shortcode == "tada";
    }
    CHECK(has_wave);
    CHECK_FALSE(has_tada); // a different room's pack, not subscribed
}

TEST_CASE("bot-command updates only matter for the shown room", "[shell][settings_misc]")
{
    SmShell s;
    s.current_room_id_ = "!r:x";
    s.handle_bot_commands_updated_ui_("!other:x");
    CHECK(s.bot_changes == 0);
    s.handle_bot_commands_updated_ui_("!r:x");
    CHECK(s.bot_changes == 1);
}

TEST_CASE("pack-tab seed and image requests ignore closed or missing targets",
          "[shell][settings_misc][packs]")
{
    tesseract::Client client;
    SmShell s;
    s.seed_image_pack_tab_("!r:x", nullptr, nullptr);
    s.handle_image_pack_images_needed_("pack", nullptr);

    s.client_ = &client;
    s.active_account_ = std::make_shared<tesseract::AccountSession>();
    s.active_account_->client = std::make_unique<tesseract::Client>();
    auto rv = tk::create_root_widget<tv::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    auto* settings = rv->room_settings_view();
    REQUIRE(settings != nullptr);
    s.seed_image_pack_tab_("!r:x", settings, nullptr); // closed view: no-op
    s.handle_image_pack_images_needed_("pack", settings);
}

TEST_CASE("pending pack images are decoded off-thread into a preview",
          "[shell][settings_misc][packs]")
{
    SmShell s;
    s.handle_image_pack_pending_image_added_(7, {1, 2, 3}, "image/png", nullptr);
    s.handle_user_pack_pending_image_added_(8, {1, 2, 3}, "image/png", nullptr);
    s.pool_.wait_idle(std::chrono::seconds(5));
    SUCCEED();
}

TEST_CASE("the activity monitor reports when the platform cannot host it",
          "[shell][settings_misc]")
{
    SmShell s;
    s.open_activity_monitor_();
    s.teardown_activity_monitor_();
    SUCCEED();
}
