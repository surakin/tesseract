#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <tesseract/settings.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct AvatarFakeImage : tk::Image
{
    int width() const override
    {
        return 1;
    }
    int height() const override
    {
        return 1;
    }
    std::size_t memory_bytes() const noexcept override
    {
        return 0;
    }
};

std::vector<std::unique_ptr<tk::Image>> avatar_frames(int n)
{
    std::vector<std::unique_ptr<tk::Image>> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        v.push_back(std::make_unique<AvatarFakeImage>());
    }
    return v;
}

struct AvatarAccountManager
{
    tesseract::AccountManager am_;
};

// Same shape as SendShell in test_shell_dispatch_room_send.cpp, trimmed to
// what the avatar paths touch; exposes the protected members under test.
struct AvatarShell : AvatarAccountManager, ShellBase
{
    AvatarShell() : ShellBase(am_)
    {
    }
    ~AvatarShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }

    void post_to_ui_(std::function<void()> fn) override
    {
        fn();
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        fn();
    }
    void request_relayout_() override
    {
    }
    void request_repaint_() override
    {
    }
    void on_rooms_updated_() override
    {
    }
    // (mxc, byte count) of every avatar/media delivery handed to the shell.
    std::vector<std::pair<std::string, std::size_t>> media_ready;
    void on_media_bytes_ready_(const tk::CacheKey& key, MediaKind,
                               std::vector<uint8_t> bytes) override
    {
        media_ready.emplace_back(key.id, bytes.size());
    }
    void on_tab_state_changed_ui_() override
    {
    }
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override
    {
        return 1000;
    }
    void start_anim_tick_() override
    {
    }
    void repaint_pickers_() override
    {
    }
    void navigate_to_room_(const std::string&) override
    {
    }
    void pick_image_file_(std::function<void(std::vector<uint8_t>, std::string)>) override
    {
    }
    void show_encryption_setup_overlay_(tesseract::views::EncryptionSetupOverlay::Mode) override
    {
    }
    void raise_and_activate_() override
    {
    }
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return nullptr;
    }
    tesseract::CallWindowBase* create_call_window_() override
    {
        return nullptr;
    }
    bool is_ctrl_held_() const override
    {
        return false;
    }
    void switch_active_account_(const std::string&) override
    {
    }
    void refresh_account_ui_after_switch_() override
    {
    }
    void bind_settings_controller_() override
    {
    }
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override
    {
    }
    std::unique_ptr<tesseract::IEventHandler> make_account_bridge_(const std::string&) override
    {
        return nullptr;
    }
    void install_account_notifier_(tesseract::AccountSession&) override
    {
    }
    void request_relogin_(const std::string&) override
    {
    }
    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }

    using ShellBase::account_manager_;
    using ShellBase::animate_avatars_effective_;
    using ShellBase::deliver_animated_avatar_;
    using ShellBase::handle_setting_changed_;
    using ShellBase::note_member_event_;
    using ShellBase::avatar_mxcs_;
    using ShellBase::avatar_mode_gen_;
    using ShellBase::on_avatar_animation_mode_changed_;
    using ShellBase::set_current_scale_;
    using ShellBase::pool_;
    using ShellBase::avatar_anim_key_;
    using ShellBase::avatar_image_;
    using ShellBase::current_room_id_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_user_id_;
    using ShellBase::own_room_avatar_;
    using ShellBase::store_decoded_media_;
    using ShellBase::strip_avatar_url_;
    using ShellBase::DecodedImage;
};

// Restores Settings::animate_avatars after a test flips it.
struct AnimateAvatarsGuard
{
    bool saved = tesseract::Settings::instance().animate_avatars;
    explicit AnimateAvatarsGuard(bool v)
    {
        tesseract::Settings::instance().animate_avatars = v;
    }
    ~AnimateAvatarsGuard()
    {
        tesseract::Settings::instance().animate_avatars = saved;
    }
};

} // namespace

TEST_CASE("avatar_image_ prefers an animated entry over the still", "[avatars][animated]")
{
    AvatarShell s;
    const std::string mxc = "mxc://x/av";
    auto still = std::make_unique<AvatarFakeImage>();
    const tk::Image* still_ptr = still.get();
    s.account_manager_.thumbnail_cache().store(tk::CacheKey::media(mxc), std::move(still));

    CHECK(s.avatar_image_(mxc) == still_ptr);

    s.account_manager_.anim_cache().store(s.avatar_anim_key_(mxc), avatar_frames(3), {50, 50, 50},
                                          1000);
    const tk::Image* got = s.avatar_image_(mxc);
    REQUIRE(got != nullptr);
    CHECK(got != still_ptr);

    CHECK(s.avatar_image_("mxc://x/other") == nullptr);
}

TEST_CASE("store_decoded_media_ keeps animated avatars only when animating", "[avatars][animated]")
{
    const std::string mxc = "mxc://x/anim";
    auto make = []
    {
        AvatarShell::DecodedImage d;
        d.still = std::make_unique<AvatarFakeImage>();
        d.frames = avatar_frames(3);
        d.delays_ms = {40, 40, 40};
        return d;
    };

    SECTION("setting on: stored as an animation")
    {
        AnimateAvatarsGuard g(true);
        AvatarShell s;
        REQUIRE(s.animate_avatars_effective_());
        CHECK(s.store_decoded_media_(tk::CacheKey::media(mxc), ShellBase::MediaKind::UserAvatar,
                                     make()));
        CHECK(s.account_manager_.anim_cache().has(s.avatar_anim_key_(mxc)));
        CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    }
    SECTION("setting off: first-frame still")
    {
        AnimateAvatarsGuard g(false);
        AvatarShell s;
        CHECK_FALSE(s.animate_avatars_effective_());
        CHECK(s.store_decoded_media_(tk::CacheKey::media(mxc), ShellBase::MediaKind::UserAvatar,
                                     make()));
        CHECK_FALSE(s.account_manager_.anim_cache().has(s.avatar_anim_key_(mxc)));
        CHECK(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    }
    SECTION("setting off, frames only: falls back to the first frame")
    {
        AnimateAvatarsGuard g(false);
        AvatarShell s;
        auto d = make();
        d.still.reset();
        CHECK(s.store_decoded_media_(tk::CacheKey::media(mxc), ShellBase::MediaKind::UserAvatar,
                                     std::move(d)));
        CHECK(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    }
}

TEST_CASE("strip_avatar_url_ shows the room avatar, else the account avatar", "[avatars][strip]")
{
    AvatarShell s;
    s.my_user_id_ = "@me:x";
    s.my_avatar_url_ = "mxc://x/account";

    SECTION("no room open")
    {
        CHECK(s.strip_avatar_url_() == "mxc://x/account");
    }
    SECTION("room without an own avatar")
    {
        s.current_room_id_ = "!r:x";
        CHECK(s.strip_avatar_url_() == "mxc://x/account");
        s.own_room_avatar_["@me:x\n!r:x"] = ""; // looked up: none
        CHECK(s.strip_avatar_url_() == "mxc://x/account");
    }
    SECTION("room with an own avatar")
    {
        s.current_room_id_ = "!r:x";
        s.own_room_avatar_["@me:x\n!r:x"] = "mxc://x/room";
        CHECK(s.strip_avatar_url_() == "mxc://x/room");
        s.current_room_id_ = "!other:x";
        CHECK(s.strip_avatar_url_() == "mxc://x/account");
    }
}

TEST_CASE("toggling Animate avatars evicts only avatar entries", "[avatars][animated]")
{
    AnimateAvatarsGuard g(true);
    AvatarShell s;
    const std::string av = "mxc://x/avatar";
    const std::string other = "mxc://x/sticker";

    // An avatar still, an animated avatar, and unrelated shared-cache content.
    s.account_manager_.thumbnail_cache().store(tk::CacheKey::media(av),
                                               std::make_unique<AvatarFakeImage>());
    s.account_manager_.anim_cache().store(s.avatar_anim_key_(av), avatar_frames(2), {50, 50}, 1000);
    s.account_manager_.thumbnail_cache().store(tk::CacheKey::media(other),
                                               std::make_unique<AvatarFakeImage>());
    s.account_manager_.anim_cache().store(tk::CacheKey::media(other), avatar_frames(2), {50, 50},
                                          1000);
    s.avatar_mxcs_.insert(av);

    tesseract::Settings::instance().animate_avatars = false;
    s.handle_setting_changed_(tesseract::views::SettingKey::AnimateAvatars);

    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(av)));
    CHECK_FALSE(s.account_manager_.anim_cache().has(s.avatar_anim_key_(av)));
    CHECK(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(other)));
    CHECK(s.account_manager_.anim_cache().has(tk::CacheKey::media(other)));

    tesseract::Settings::instance().animate_avatars =
        true; // restore (guard restores the saved value)
}

TEST_CASE("an animated avatar the decoder can't read falls back to the shell's still decode",
          "[avatars][animated]")
{
    AnimateAvatarsGuard g(true);
    AvatarShell s; // decode_image_ returns an empty result
    s.deliver_animated_avatar_("mxc://x/odd", ShellBase::MediaKind::UserAvatar,
                               std::vector<std::uint8_t>(7, 0x47));
    REQUIRE(s.pool_.wait_idle(std::chrono::seconds(2)));
    REQUIRE(s.media_ready.size() == 1);
    CHECK(s.media_ready[0].first == "mxc://x/odd");
    CHECK(s.media_ready[0].second == 7); // original bytes, not dropped
}

TEST_CASE("avatar_anim_key_ does not depend on the display scale", "[avatars][animated]")
{
    AvatarShell s;
    const auto before = s.avatar_anim_key_("mxc://x/a");
    s.set_current_scale_(2.0f);
    CHECK(s.avatar_anim_key_("mxc://x/a") == before);
}

TEST_CASE("an avatar animation mode change bumps the generation and evicts only when asked",
          "[avatars][animated]")
{
    AvatarShell s;
    const std::string av = "mxc://x/avatar";
    s.account_manager_.thumbnail_cache().store(tk::CacheKey::media(av),
                                               std::make_unique<AvatarFakeImage>());
    s.avatar_mxcs_.insert(av);
    const auto gen0 = s.avatar_mode_gen_;

    s.on_avatar_animation_mode_changed_(/*evict=*/false); // e.g. entering low power
    CHECK(s.avatar_mode_gen_ == gen0 + 1);
    CHECK(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(av)));

    s.on_avatar_animation_mode_changed_(/*evict=*/true); // e.g. leaving low power
    CHECK(s.avatar_mode_gen_ == gen0 + 2);
    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(tk::CacheKey::media(av)));
}
