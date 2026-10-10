// ShellBase_media_images.cpp: the ensure_* request guards (dedup, cache,
// backoff, group gating), the disk-miss -> request -> deliver pipeline driven
// end to end through a queued UI thread, per-row media policy (MSC4278), the
// URL-preview / blurhash / avatar bookkeeping, and the media completion
// handlers. Network is never touched: Client has no session, so requests stay
// pending until the test completes them through handle_media_ready_ui_.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;
using tesseract::MediaSource;
using tesseract::views::MessageRowData;

namespace
{

struct MiFakeImg : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return 4; }
};

struct MediaShell : tesseract::test::TestShellBase
{
    ~MediaShell() override
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

    // Worker threads post here, so guard the queue.
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        delayed.push_back(std::move(fn));
    }
    void request_relayout_() override { ++relayouts; }
    void request_repaint_() override { ++repaints; }
    void repaint_pickers_() override { ++picker_repaints; }
    void start_anim_tick_() override { ++anim_ticks; }
    void on_media_bytes_ready_(const tk::CacheKey& k, MediaKind kind,
                               std::vector<uint8_t> b) override
    {
        delivered.emplace_back(k.to_string(), kind, std::move(b));
    }
    void cache_rgba_image_(const tk::CacheKey& k, int w, int h,
                           std::vector<uint8_t>) override
    {
        rgba.emplace_back(k.to_string(), w, h);
    }
    DecodedImage decode_image_(const std::vector<uint8_t>& b, int, int) override
    {
        ++decodes;
        DecodedImage d;
        if (!b.empty() && decode_ok)
            d.still = std::make_unique<MiFakeImg>();
        return d;
    }
    void on_invites_updated_() override {}

    // Run queued UI work and let worker jobs settle, until quiescent.
    void pump()
    {
        for (int i = 0; i < 100; ++i)
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
    // The single pending media request id (REQUIREs there is exactly one).
    std::uint64_t only_pending()
    {
        REQUIRE(pending_media_.size() == 1);
        return pending_media_.begin()->first;
    }

    std::mutex mu;
    std::vector<std::function<void()>> queue, delayed;
    std::vector<std::tuple<std::string, MediaKind, std::vector<uint8_t>>> delivered;
    std::vector<std::tuple<std::string, int, int>> rgba;
    int relayouts = 0, repaints = 0, picker_repaints = 0, anim_ticks = 0,
        decodes = 0;
    bool decode_ok = true;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::active_media_group_;
    using ShellBase::animate_avatars_effective_;
    using ShellBase::apply_media_preview_config_;
    using ShellBase::avatar_image_;
    using ShellBase::avatar_mode_gen_;
    using ShellBase::avatar_mxcs_;
    using ShellBase::avatar_px_;
    using ShellBase::begin_media_req_;
    using ShellBase::begin_media_stream_req_;
    using ShellBase::blurhash_attempted_;
    using ShellBase::client_;
    using ShellBase::commit_room_media_preview_override_;
    using ShellBase::current_room_id_;
    using ShellBase::current_scale_;
    using ShellBase::deliver_animated_avatar_;
    using ShellBase::effective_preview_mode_;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::ensure_blurhash_image_;
    using ShellBase::ensure_media_image_;
    using ShellBase::ensure_media_thumbnail_;
    using ShellBase::ensure_picker_image_;
    using ShellBase::ensure_picker_sticker_;
    using ShellBase::ensure_room_avatar_;
    using ShellBase::ensure_room_preview_override_;
    using ShellBase::ensure_row_media_;
    using ShellBase::ensure_url_preview_;
    using ShellBase::ensure_user_avatar_;
    using ShellBase::ensure_viewer_fullres_;
    using ShellBase::evict_avatar_caches_;
    using ShellBase::fetch_room_security_state_;
    using ShellBase::fullres_key_;
    using ShellBase::handle_animate_avatars_toggle_;
    using ShellBase::handle_media_chunk_ui_;
    using ShellBase::handle_media_preview_config_fetched_ui_;
    using ShellBase::handle_media_preview_config_updated_ui_;
    using ShellBase::handle_media_ready_ui_;
    using ShellBase::handle_room_media_preview_override_updated_ui_;
    using ShellBase::handle_room_preview_override_ready_ui_;
    using ShellBase::handle_room_security_state_ready_ui_;
    using ShellBase::handle_url_preview_ready_ui_;
    using ShellBase::load_media_bytes_;
    using ShellBase::media_allowed_;
    using ShellBase::media_decode_failed_;
    using ShellBase::media_fetch_failed_;
    using ShellBase::media_fetches_in_flight_;
    using ShellBase::media_key_to_req_;
    using ShellBase::media_preview_hidden_;
    using ShellBase::media_prepped_event_ids_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_user_id_;
    using ShellBase::next_request_id_;
    using ShellBase::note_member_event_;
    using ShellBase::on_avatar_animation_mode_changed_;
    using ShellBase::on_url_preview_failed_;
    using ShellBase::on_url_preview_ready_;
    using ShellBase::on_visible_rows_changed_;
    using ShellBase::own_room_avatar_;
    using ShellBase::own_room_avatar_in_flight_;
    using ShellBase::pending_media_;
    using ShellBase::pending_media_streams_;
    using ShellBase::pending_preview_overrides_;
    using ShellBase::pending_security_state_requests_;
    using ShellBase::picker_sticker_key_;
    using ShellBase::request_own_room_avatar_;
    using ShellBase::revealed_events_;
    using ShellBase::reveal_media_fetch_;
    using ShellBase::room_preview_overrides_;
    using ShellBase::room_view_;
    using ShellBase::seed_room_media_section_;
    using ShellBase::server_info_;
    using ShellBase::shell_sticker_;
    using ShellBase::should_auto_preview_;
    using ShellBase::sticker_fetches_in_flight_;
    using ShellBase::store_media_bytes_;
    using ShellBase::strip_avatar_url_;
    using ShellBase::thumb_key;
    using ShellBase::url_preview_data_;
    using ShellBase::url_preview_in_flight_;
    using ShellBase::url_previews_;
    using ShellBase::video_thumb_in_flight_;
    using ShellBase::viewer_fullres_;
    using ShellBase::viewer_fullres_in_flight_;
    using ShellBase::viewer_image_lookup_;
    using ShellBase::voice_prefetched_;
    using ShellBase::voice_waveform_in_flight_;
    using ShellBase::wire_media_preview_gating_;
    using ShellBase::ensure_tile_async;
    using ShellBase::tile_fetch_failed_;
    using ShellBase::tile_fetches_in_flight_;
    using ShellBase::cancel_media_group_;
    using ShellBase::media_group_for_room_;
    using MediaKind = ShellBase::MediaKind;
};


tesseract::MediaSourceRef mi_src(const std::string& u)
{
    return MediaSource::plain(u);
}

struct MiFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    MediaShell s;
    // Unique per fixture: the disk cache outlives a test case in-process.
    std::string url = "mxc://hs/u" + std::to_string(next_id()++);
    static int& next_id()
    {
        static int n = 0;
        return n;
    }
    MiFx()
    {
        auto& st = tesseract::Settings::instance();
        st.media_previews = tesseract::Settings::MediaPreviews::On;
        st.prefetch_full_media = false;
        st.animate_avatars = false;
        st.invite_avatars = true;
        s.client_ = &client;
        s.current_room_id_ = "!r:x";
        s.my_user_id_ = "@me:x";
        s.server_info_.preview_url_enabled = true;
    }
};

} // namespace

// ── ensure_media_image_ guards and pipeline ─────────────────────────────────

TEST_CASE("ensure_media_image_ ignores empty, cached, failed and backed-off urls",
          "[shell][media_images]")
{
    MiFx f;
    f.s.ensure_media_image_("", 0, 0);
    CHECK(f.s.media_fetches_in_flight_.empty());

    f.s.account_manager_.image_cache().store(tk::CacheKey::media(f.url),
                                             std::make_unique<MiFakeImg>());
    f.s.ensure_media_image_(f.url, 0, 0);
    CHECK(f.s.media_fetches_in_flight_.empty());

    f.s.media_decode_failed_.insert("mxc://hs/bad");
    f.s.ensure_media_image_("mxc://hs/bad", 0, 0);
    f.s.media_fetch_failed_["mxc://hs/slow"].retry_after =
        std::chrono::steady_clock::now() + std::chrono::hours(1);
    f.s.ensure_media_image_("mxc://hs/slow", 0, 0);
    CHECK(f.s.media_fetches_in_flight_.empty());
}

TEST_CASE("ensure_media_image_ dedups in flight and delivers fetched bytes",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.active_media_group_ = 7;
    f.s.ensure_media_image_(f.url, 0, 0, 7);
    f.s.ensure_media_image_(f.url, 0, 0, 7); // deduped
    CHECK(f.s.media_fetches_in_flight_.count(f.url) == 1);

    f.s.pump(); // disk miss -> network request registered
    const auto id = f.s.only_pending();
    CHECK(f.s.media_key_to_req_.count(f.url) == 1);

    f.s.handle_media_ready_ui_(id, {1, 2, 3});
    f.s.pump(); // persist, then deliver
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(std::get<0>(f.s.delivered[0]) == tk::CacheKey::media(f.url).to_string());
    CHECK(std::get<2>(f.s.delivered[0]) == std::vector<uint8_t>{1, 2, 3});
    CHECK(f.s.media_fetches_in_flight_.empty());
    CHECK(f.s.media_key_to_req_.empty());
    // The bytes were persisted for the next session.
    CHECK(f.s.load_media_bytes_(tk::CacheKey::media(f.url)) ==
          std::vector<uint8_t>{1, 2, 3});

    // A late duplicate completion is a no-op.
    f.s.handle_media_ready_ui_(id, {9});
    CHECK(f.s.delivered.size() == 1);
}

TEST_CASE("a disk-cache hit is delivered without a network request",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.store_media_bytes_(tk::CacheKey::media(f.url), {4, 5});
    f.s.ensure_media_image_(f.url, 0, 0);
    f.s.pump();
    CHECK(f.s.pending_media_.empty());
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(std::get<2>(f.s.delivered[0]) == std::vector<uint8_t>{4, 5});
    CHECK(f.s.media_fetches_in_flight_.empty());
}

TEST_CASE("an empty network response backs the url off",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.ensure_media_image_(f.url, 0, 0);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {});
    f.s.pump();
    REQUIRE(f.s.delivered.size() == 1); // empty delivery tells the shell it failed
    CHECK(std::get<2>(f.s.delivered[0]).empty());
    CHECK(f.s.media_fetch_failed_.count(f.url) == 1);
    CHECK(f.s.media_fetches_in_flight_.empty());

    f.s.ensure_media_image_(f.url, 0, 0); // backed off: no new request
    CHECK(f.s.media_fetches_in_flight_.empty());
}

TEST_CASE("a download for a room that is no longer active is dropped",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.active_media_group_ = 8;
    f.s.ensure_media_image_(f.url, 0, 0, 7); // group 7 != active 8
    f.s.pump();
    CHECK(f.s.pending_media_.empty());
    CHECK(f.s.media_fetches_in_flight_.empty());
}

TEST_CASE("with no client the pipeline just releases the in-flight key",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.client_ = nullptr;
    f.s.ensure_media_image_(f.url, 0, 0);
    f.s.pump();
    CHECK(f.s.pending_media_.empty());
    CHECK(f.s.media_fetches_in_flight_.empty());
}

TEST_CASE("cancelling the room's media group frees its in-flight keys",
          "[shell][media_images][pipeline]")
{
    MiFx f;
    f.s.active_media_group_ = 7;
    f.s.ensure_media_image_(f.url, 0, 0, 7);
    f.s.pump();
    REQUIRE(f.s.pending_media_.size() == 1);
    f.s.cancel_media_group_(7);
    CHECK(f.s.pending_media_.empty());
    CHECK(f.s.media_fetches_in_flight_.empty());
    CHECK(f.s.media_key_to_req_.empty());
}

// ── thumbnails, avatars ──────────────────────────────────────────────────────

TEST_CASE("ensure_media_thumbnail_ keys by size and scale",
          "[shell][media_images]")
{
    MiFx f;
    f.s.current_scale_ = 2.0f;
    f.s.ensure_media_thumbnail_(f.url, 64, 48, false, 0);
    CHECK(f.s.media_fetches_in_flight_.count(MediaShell::thumb_key(f.url, 128, 96)) == 1);
    f.s.ensure_media_thumbnail_(f.url, 64, 48, false, 0); // dedup
    CHECK(f.s.media_fetches_in_flight_.size() == 1);

    f.s.ensure_media_thumbnail_("", 64, 48, false, 0);
    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media("mxc://hs/t"),
                                                 std::make_unique<MiFakeImg>());
    f.s.ensure_media_thumbnail_("mxc://hs/t", 64, 48, false, 0);
    CHECK(f.s.media_fetches_in_flight_.size() == 1);
}

TEST_CASE("ensure_user_avatar_ tracks the mxc and skips cached or failed ones",
          "[shell][media_images][avatar]")
{
    MiFx f;
    f.s.ensure_user_avatar_("");
    CHECK(f.s.avatar_mxcs_.empty());

    f.s.ensure_user_avatar_("mxc://hs/a1");
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/a1") == 1);
    const auto key = MediaShell::thumb_key("mxc://hs/a1", f.s.avatar_px_(),
                                            f.s.avatar_px_());
    CHECK(f.s.media_fetches_in_flight_.count(key) == 1);

    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media("mxc://hs/a2"),
                                                 std::make_unique<MiFakeImg>());
    f.s.ensure_user_avatar_("mxc://hs/a2");
    CHECK(f.s.media_fetches_in_flight_.size() == 1);

    f.s.media_decode_failed_.insert("mxc://hs/a3");
    f.s.ensure_user_avatar_("mxc://hs/a3");
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/a3") == 0);
}

TEST_CASE("prefetch_full_media also warms the full-size avatar",
          "[shell][media_images][avatar]")
{
    MiFx f;
    tesseract::Settings::instance().prefetch_full_media = true;
    f.s.ensure_user_avatar_("mxc://hs/a1");
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/a1") == 1);
    CHECK(f.s.media_fetches_in_flight_.size() == 2);
}

TEST_CASE("animated avatars use their own namespaced thumbnail key",
          "[shell][media_images][avatar]")
{
    MiFx f;
    tesseract::Settings::instance().animate_avatars = true;
    CHECK(f.s.animate_avatars_effective_());
    f.s.ensure_user_avatar_("mxc://hs/a1");
    CHECK(f.s.media_fetches_in_flight_.count(MediaShell::thumb_key(
              "anim:mxc://hs/a1", f.s.avatar_px_(), f.s.avatar_px_())) == 1);
}

TEST_CASE("ensure_room_avatar_ prefers the room avatar, falls back to the DM one",
          "[shell][media_images][avatar]")
{
    MiFx f;
    tesseract::RoomInfo none;
    none.id = "!n:x";
    f.s.ensure_room_avatar_(none);
    CHECK(f.s.avatar_mxcs_.empty());

    tesseract::RoomInfo room;
    room.id = "!r:x";
    room.avatar_url = "mxc://hs/room";
    room.dm_avatar_url = "mxc://hs/dm";
    f.s.ensure_room_avatar_(room);
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/room") == 1);
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/dm") == 0);

    room.avatar_url.clear();
    f.s.ensure_room_avatar_(room);
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/dm") == 1);
}

TEST_CASE("avatar animation toggle evicts only avatar entries and bumps the "
          "generation",
          "[shell][media_images][avatar]")
{
    MiFx f;
    tesseract::Settings::instance().animate_avatars = false;
    f.s.avatar_mxcs_ = {"mxc://hs/a"};
    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media("mxc://hs/a"),
                                                 std::make_unique<MiFakeImg>());
    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media("mxc://hs/other"),
                                                 std::make_unique<MiFakeImg>());
    f.s.media_decode_failed_.insert("mxc://hs/a");
    const auto gen = f.s.avatar_mode_gen_;

    f.s.handle_animate_avatars_toggle_(false); // unchanged: no-op
    CHECK(f.s.avatar_mode_gen_ == gen);

    f.s.handle_animate_avatars_toggle_(true);
    CHECK(f.s.avatar_mode_gen_ == gen + 1);
    CHECK_FALSE(f.s.account_manager_.thumbnail_cache().contains(
        tk::CacheKey::media("mxc://hs/a")));
    CHECK(f.s.account_manager_.thumbnail_cache().contains(
        tk::CacheKey::media("mxc://hs/other")));
    CHECK(f.s.media_decode_failed_.count("mxc://hs/a") == 0);

    f.s.on_avatar_animation_mode_changed_(false);
    CHECK(f.s.avatar_mode_gen_ == gen + 2);
}

TEST_CASE("deliver_animated_avatar_ decodes and stores, or falls back to the "
          "shell decoder",
          "[shell][media_images][avatar]")
{
    MiFx f;
    f.s.deliver_animated_avatar_("mxc://hs/a", MediaShell::MediaKind::UserAvatar, {1, 2});
    f.s.pump();
    CHECK(f.s.decodes == 1);
    CHECK(f.s.account_manager_.thumbnail_cache().contains(
        tk::CacheKey::media("mxc://hs/a")));
    CHECK(f.s.repaints >= 1);

    // Already stored: the early-out skips decoding.
    f.s.deliver_animated_avatar_("mxc://hs/a", MediaShell::MediaKind::UserAvatar, {1, 2});
    f.s.pump();
    CHECK(f.s.decodes == 1);

    // Undecodable: bytes are handed to the shell's own decoder.
    f.s.decode_ok = false;
    f.s.deliver_animated_avatar_("mxc://hs/b", MediaShell::MediaKind::UserAvatar, {7});
    f.s.pump();
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(std::get<2>(f.s.delivered[0]) == std::vector<uint8_t>{7});
}

TEST_CASE("avatar_image_ finds cached thumbnails", "[shell][media_images][avatar]")
{
    MiFx f;
    CHECK(f.s.avatar_image_("mxc://hs/a") == nullptr);
    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media("mxc://hs/a"),
                                                 std::make_unique<MiFakeImg>());
    CHECK(f.s.avatar_image_("mxc://hs/a") != nullptr);
}

TEST_CASE("strip avatar prefers the per-room avatar and re-reads on room change",
          "[shell][media_images][avatar]")
{
    MiFx f;
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>();
    f.s.active_account_->user_id = "@me:x";
    f.s.active_account_->client = std::make_unique<tesseract::Client>();
    f.s.my_avatar_url_ = "mxc://hs/global";

    // First paint in the room kicks the lookup; the global avatar shows until
    // the (sessionless, empty) result lands.
    CHECK(f.s.strip_avatar_url_() == "mxc://hs/global");
    CHECK(f.s.own_room_avatar_in_flight_.size() == 1);
    f.s.pump();
    CHECK(f.s.own_room_avatar_in_flight_.empty());
    CHECK(f.s.strip_avatar_url_() == "mxc://hs/global");

    f.s.own_room_avatar_["@me:x\n!r:x"] = "mxc://hs/room-specific";
    CHECK(f.s.strip_avatar_url_() == "mxc://hs/room-specific");

    f.s.current_room_id_.clear();
    CHECK(f.s.strip_avatar_url_() == "mxc://hs/global");

    // A second request while one is running is folded into a re-run.
    f.s.request_own_room_avatar_("");
    f.s.request_own_room_avatar_("!r:x");
    f.s.request_own_room_avatar_("!r:x");
    f.s.pump();
    CHECK(f.s.own_room_avatar_in_flight_.empty());
}

// ── pickers, tiles, viewer ──────────────────────────────────────────────────

TEST_CASE("picker images fetch, decode and land in the image cache",
          "[shell][media_images][picker]")
{
    MiFx f;
    f.s.ensure_picker_image_("", false);
    CHECK(f.s.emoji_fetches_in_flight_.empty());

    f.s.ensure_picker_image_(f.url, false);
    f.s.ensure_picker_image_(f.url, false);
    CHECK(f.s.emoji_fetches_in_flight_.size() == 1);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {1});
    f.s.pump();
    CHECK(f.s.emoji_fetches_in_flight_.empty());
    CHECK(f.s.account_manager_.image_cache().contains(tk::CacheKey::media(f.url)));
    CHECK(f.s.picker_repaints >= 1);

    f.s.ensure_picker_image_(f.url, false); // cached: nothing in flight
    CHECK(f.s.emoji_fetches_in_flight_.empty());
}

TEST_CASE("sticker picker uses the sticker set and clears it on failure",
          "[shell][media_images][picker]")
{
    MiFx f;
    f.s.ensure_picker_image_(f.url, true);
    CHECK(f.s.sticker_fetches_in_flight_.size() == 1);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {}); // empty -> failure
    f.s.pump();
    CHECK(f.s.sticker_fetches_in_flight_.empty());

    // Disk hit decodes without a request; an undecodable result stays uncached.
    f.s.store_media_bytes_(tk::CacheKey::media("mxc://hs/disk"), {1});
    f.s.decode_ok = false;
    f.s.ensure_picker_image_("mxc://hs/disk", true);
    f.s.pump();
    CHECK(f.s.pending_media_.empty());
    CHECK_FALSE(f.s.account_manager_.image_cache().contains(
        tk::CacheKey::media("mxc://hs/disk")));
}

TEST_CASE("picker stickers are keyed by picker size and deduped",
          "[shell][media_images][picker]")
{
    MiFx f;
    f.s.ensure_picker_sticker_("");
    f.s.ensure_picker_sticker_(f.url);
    f.s.ensure_picker_sticker_(f.url);
    CHECK(f.s.media_fetches_in_flight_.size() == 1);
    CHECK(f.s.media_fetches_in_flight_.count(
              f.s.picker_sticker_key_(f.url).to_string()) == 1);
}

TEST_CASE("shell_sticker_ requests an uncached sticker and returns cached ones",
          "[shell][media_images][picker]")
{
    MiFx f;
    CHECK(f.s.shell_sticker_(f.url) == nullptr);
    CHECK(f.s.media_fetches_in_flight_.count(f.url) == 1);
    f.s.account_manager_.image_cache().store(tk::CacheKey::media("mxc://hs/s"),
                                             std::make_unique<MiFakeImg>());
    CHECK(f.s.shell_sticker_("mxc://hs/s") != nullptr);
}

TEST_CASE("viewer full-res load: miss, download, decode, store", 
          "[shell][media_images][viewer]")
{
    MiFx f;
    f.s.ensure_viewer_fullres_("");
    CHECK(f.s.viewer_fullres_in_flight_.empty());

    f.s.ensure_viewer_fullres_(f.url);
    f.s.ensure_viewer_fullres_(f.url); // in flight
    CHECK(f.s.viewer_fullres_in_flight_.size() == 1);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {1, 2});
    f.s.pump();
    CHECK(f.s.viewer_fullres_in_flight_.empty());
    CHECK(f.s.viewer_fullres_.count(tk::CacheKey::fullres(f.url)) == 1);
    CHECK(f.s.viewer_image_lookup_(f.url) != nullptr);
    CHECK(f.s.relayouts >= 1);

    f.s.ensure_viewer_fullres_(f.url); // cached: nothing
    CHECK(f.s.viewer_fullres_in_flight_.empty());
}

TEST_CASE("viewer full-res: failures, undecodable data and the FIFO cap",
          "[shell][media_images][viewer]")
{
    MiFx f;
    f.s.ensure_viewer_fullres_(f.url);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {}); // network failure
    f.s.pump();
    CHECK(f.s.viewer_fullres_in_flight_.empty());
    CHECK(f.s.media_fetch_failed_.count(MediaShell::fullres_key_(f.url)) == 1);

    f.s.decode_ok = false;
    f.s.ensure_viewer_fullres_("mxc://hs/undecodable");
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {1});
    f.s.pump();
    CHECK(f.s.media_decode_failed_.count(
              MediaShell::fullres_key_("mxc://hs/undecodable")) == 1);

    f.s.decode_ok = true;
    for (int i = 0; i < 8; ++i)
    {
        f.s.ensure_viewer_fullres_("mxc://hs/v" + std::to_string(i));
        f.s.pump();
        f.s.handle_media_ready_ui_(f.s.only_pending(), {1});
        f.s.pump();
    }
    CHECK(f.s.viewer_fullres_.size() <= 6);
    CHECK(f.s.viewer_fullres_.count(tk::CacheKey::fullres("mxc://hs/v7")) == 1);
    CHECK(f.s.viewer_fullres_.count(tk::CacheKey::fullres("mxc://hs/v0")) == 0);
}

TEST_CASE("viewer full-res without a client releases its in-flight key",
          "[shell][media_images][viewer]")
{
    MiFx f;
    f.s.client_ = nullptr;
    f.s.ensure_viewer_fullres_(f.url);
    CHECK(f.s.viewer_fullres_in_flight_.empty());
}

TEST_CASE("viewer_image_lookup_ falls back through the caches",
          "[shell][media_images][viewer]")
{
    MiFx f;
    CHECK(f.s.viewer_image_lookup_(f.url) == nullptr);
    f.s.account_manager_.thumbnail_cache().store(tk::CacheKey::media(f.url),
                                                 std::make_unique<MiFakeImg>());
    CHECK(f.s.viewer_image_lookup_(f.url) != nullptr);
}

// ── URL previews and blurhash ───────────────────────────────────────────────

TEST_CASE("URL preview request lifecycle", "[shell][media_images][preview]")
{
    MiFx f;
    f.s.server_info_.preview_url_enabled = false;
    f.s.ensure_url_preview_("https://a.example/");
    CHECK(f.s.pending_media_.empty()); // homeserver has previews off
    f.s.server_info_.preview_url_enabled = true;

    f.s.ensure_url_preview_("");
    f.s.ensure_url_preview_("https://a.example/");
    f.s.ensure_url_preview_("https://a.example/"); // in flight
    REQUIRE(f.s.pending_media_.size() == 1);
    CHECK(f.s.url_preview_in_flight_.count("https://a.example/") == 1);

    f.s.handle_url_preview_ready_ui_(
        f.s.only_pending(),
        R"({"og:title":"T","og:description":"D","og:image":"mxc://i/1"})");
    CHECK(f.s.url_preview_in_flight_.empty());
    REQUIRE(f.s.url_preview_data_.count("https://a.example/") == 1);
    CHECK(f.s.url_preview_data_.at("https://a.example/").title == "T");
    // The preview image is requested as a thumbnail.
    CHECK(f.s.media_fetches_in_flight_.size() == 1);

    f.s.ensure_url_preview_("https://a.example/"); // known: no new request
    CHECK(f.s.pending_media_.empty());

    f.s.handle_url_preview_ready_ui_(12345, "{}"); // unknown id ignored
}

TEST_CASE("a failed URL preview records the failure without a card",
          "[shell][media_images][preview]")
{
    MiFx f;
    f.s.ensure_url_preview_("https://b.example/");
    f.s.handle_url_preview_ready_ui_(f.s.only_pending(), "");
    CHECK(f.s.url_previews_.at("https://b.example/").failed);
    CHECK(f.s.url_preview_data_.count("https://b.example/") == 0);
    f.s.on_url_preview_failed_("https://b.example/"); // no views: harmless
}

TEST_CASE("URL preview cancel frees the in-flight key",
          "[shell][media_images][preview]")
{
    MiFx f;
    f.s.ensure_url_preview_("https://c.example/");
    f.s.cancel_media_group_(MediaShell::media_group_for_room_("!r:x"));
    CHECK(f.s.url_preview_in_flight_.empty());
}

TEST_CASE("blurhash placeholders are decoded once per event",
          "[shell][media_images][blurhash]")
{
    MiFx f;
    const std::string hash = "LEHV6nWB2yk8pyo0adR*.7kCMdnj";
    f.s.ensure_blurhash_image_("$e1", hash, 200, 100);
    f.s.ensure_blurhash_image_("$e1", hash, 200, 100);
    REQUIRE(f.s.rgba.size() == 1);
    CHECK(std::get<1>(f.s.rgba[0]) == 32);
    CHECK(std::get<2>(f.s.rgba[0]) == 16); // aspect preserved

    f.s.ensure_blurhash_image_("$e2", hash, 100, 200);
    CHECK(std::get<1>(f.s.rgba[1]) == 16);
    CHECK(std::get<2>(f.s.rgba[1]) == 32);
    f.s.ensure_blurhash_image_("$e3", hash, 0, 0);
    CHECK(std::get<1>(f.s.rgba[2]) == 32);
    f.s.ensure_blurhash_image_("$e4", "!!", 10, 10); // invalid hash: skipped
    CHECK(f.s.rgba.size() == 3);
}

// ── ensure_row_media_ (Event) ───────────────────────────────────────────────

TEST_CASE("event media: image with thumbnail and full-media prefetch",
          "[shell][media_images][row]")
{
    MiFx f;
    auto img = std::make_unique<tesseract::ImageEvent>();
    img->event_id = "$i";
    img->sender = "@a:x";
    img->sender_avatar_url = "mxc://hs/avatar";
    img->thumbnail = mi_src("mxc://hs/thumb");
    img->source = mi_src("mxc://hs/full");
    img->blurhash = "LEHV6nWB2yk8pyo0adR*.7kCMdnj";
    img->width = 300;
    img->height = 200;

    f.s.ensure_row_media_(*img, true);
    CHECK(f.s.avatar_mxcs_.count("mxc://hs/avatar") == 1);
    const int w = static_cast<int>(std::lround(
        tesseract::visual::kMaxInlineImageWidth * f.s.current_scale_));
    const int h = static_cast<int>(std::lround(
        tesseract::visual::kMaxInlineImageHeight * f.s.current_scale_));
    CHECK(f.s.media_fetches_in_flight_.count(
              MediaShell::thumb_key("mxc://hs/thumb", w, h)) == 1);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/full") == 0);
    CHECK(f.s.rgba.size() == 1);

    tesseract::Settings::instance().prefetch_full_media = true;
    f.s.ensure_row_media_(*img, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/full") == 1);
}

TEST_CASE("event media: an image with no thumbnail fetches the source",
          "[shell][media_images][row]")
{
    MiFx f;
    auto img = std::make_unique<tesseract::ImageEvent>();
    img->event_id = "$i";
    img->source = mi_src("mxc://hs/full");
    f.s.ensure_row_media_(*img, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/full") == 1);
}

TEST_CASE("event media: previews off suppresses media until revealed",
          "[shell][media_images][row]")
{
    MiFx f;
    tesseract::Settings::instance().media_previews =
        tesseract::Settings::MediaPreviews::Off;
    auto img = std::make_unique<tesseract::ImageEvent>();
    img->event_id = "$i";
    img->sender = "@a:x";
    img->source = mi_src("mxc://hs/full");
    img->blurhash = "LEHV6nWB2yk8pyo0adR*.7kCMdnj";
    f.s.ensure_row_media_(*img, false);
    CHECK(f.s.media_fetches_in_flight_.empty());
    CHECK(f.s.rgba.size() == 1); // the placeholder is not gated

    f.s.revealed_events_.insert("$i");
    f.s.ensure_row_media_(*img, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/full") == 1);
}

TEST_CASE("event media: your own media is exempt in Private mode",
          "[shell][media_images][row]")
{
    MiFx f;
    tesseract::Settings::instance().media_previews =
        tesseract::Settings::MediaPreviews::Private;
    f.s.room_preview_overrides_["!r:x"].join_rule = "public";
    auto mine = std::make_unique<tesseract::ImageEvent>();
    mine->event_id = "$mine";
    mine->sender = "@me:x";
    mine->source = mi_src("mxc://hs/mine");
    f.s.ensure_row_media_(*mine, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/mine") == 1);

    auto theirs = std::make_unique<tesseract::ImageEvent>();
    theirs->event_id = "$theirs";
    theirs->sender = "@a:x";
    theirs->source = mi_src("mxc://hs/theirs");
    f.s.ensure_row_media_(*theirs, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/theirs") == 0);
}

TEST_CASE("event media: stickers", "[shell][media_images][row]")
{
    MiFx f;
    auto st = std::make_unique<tesseract::StickerEvent>();
    st->event_id = "$s";
    st->thumbnail = mi_src("mxc://hs/sthumb");
    st->source = mi_src("mxc://hs/sfull");
    f.s.ensure_row_media_(*st, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/sthumb") == 1);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/sfull") == 0);

    auto bare = std::make_unique<tesseract::StickerEvent>();
    bare->event_id = "$s2";
    bare->source = mi_src("mxc://hs/only");
    f.s.ensure_row_media_(*bare, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/only") == 1);
}

TEST_CASE("event media: voice is prefetched once with a waveform request",
          "[shell][media_images][row]")
{
    MiFx f;
    auto v = std::make_unique<tesseract::VoiceEvent>();
    v->event_id = "$v";
    v->source = mi_src("mxc://hs/voice");
    f.s.ensure_row_media_(*v, false);
    CHECK(f.s.voice_prefetched_.count("mxc://hs/voice") == 1);
    CHECK(f.s.voice_waveform_in_flight_.count("mxc://hs/voice") == 1);
    CHECK(f.s.pending_media_.size() == 1);

    f.s.ensure_row_media_(*v, false); // already requested
    CHECK(f.s.pending_media_.size() == 1);

    // Completing with unplayable bytes derives no waveform and just ends.
    f.s.handle_media_ready_ui_(f.s.only_pending(), {1, 2, 3});
    f.s.pump();

    // A sender-supplied waveform needs only the audio warm-up.
    auto w = std::make_unique<tesseract::VoiceEvent>();
    w->event_id = "$w";
    w->source = mi_src("mxc://hs/voice2");
    w->waveform = {1, 2, 3};
    f.s.ensure_row_media_(*w, false);
    CHECK(f.s.voice_waveform_in_flight_.count("mxc://hs/voice2") == 0);
    CHECK(f.s.voice_prefetched_.count("mxc://hs/voice2") == 1);
}

TEST_CASE("event media: audio is only warmed when full-media prefetch is on",
          "[shell][media_images][row]")
{
    MiFx f;
    auto a = std::make_unique<tesseract::AudioEvent>();
    a->event_id = "$a";
    a->source = mi_src("mxc://hs/audio");
    f.s.ensure_row_media_(*a, false);
    CHECK(f.s.voice_prefetched_.empty());
    tesseract::Settings::instance().prefetch_full_media = true;
    f.s.ensure_row_media_(*a, false);
    CHECK(f.s.voice_prefetched_.count("mxc://hs/audio") == 1);
    f.s.ensure_row_media_(*a, false);
    CHECK(f.s.pending_media_.size() == 1);
}

TEST_CASE("event media: video uses the server thumbnail or generates one",
          "[shell][media_images][row]")
{
    MiFx f;
    auto v = std::make_unique<tesseract::VideoEvent>();
    v->event_id = "$vid";
    v->thumbnail = mi_src("mxc://hs/vthumb");
    v->source = mi_src("mxc://hs/video");
    f.s.ensure_row_media_(*v, false);
    CHECK(f.s.media_fetches_in_flight_.size() == 1);
    CHECK(f.s.video_thumb_in_flight_.empty());

    auto g = std::make_unique<tesseract::VideoEvent>();
    g->event_id = "$vid2";
    g->source = mi_src("mxc://hs/video2");
    f.s.ensure_row_media_(*g, false);
    CHECK(f.s.video_thumb_in_flight_.count("$vid2") == 1);
    f.s.pump(); // no cached frame; with no session nothing more happens
}

TEST_CASE("event media: reactions with images are fetched", 
          "[shell][media_images][row]")
{
    MiFx f;
    auto t = std::make_unique<tesseract::Event>();
    t->event_id = "$t";
    t->type = tesseract::EventType::Text;
    tesseract::Reaction r;
    r.key = ":x:";
    r.source = mi_src("mxc://hs/react");
    t->reactions.push_back(r);
    tesseract::Reaction plain;
    plain.key = "+";
    t->reactions.push_back(plain);
    f.s.ensure_row_media_(*t, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/react") == 1);
    CHECK(f.s.media_fetches_in_flight_.size() == 1);
}

TEST_CASE("event media: text links request a homeserver preview",
          "[shell][media_images][row]")
{
    MiFx f;
    auto t = std::make_unique<tesseract::Event>();
    t->event_id = "$t";
    t->type = tesseract::EventType::Text;
    t->body = "look https://example.com/page";
    f.s.ensure_row_media_(*t, false);
    CHECK(f.s.url_preview_in_flight_.count("https://example.com/page") == 1);

    auto h = std::make_unique<tesseract::Event>();
    h->event_id = "$h";
    h->type = tesseract::EventType::Text;
    h->formatted_body = "<a href=\"https://example.org/x\">x</a>";
    f.s.ensure_row_media_(*h, false);
    CHECK(f.s.url_preview_in_flight_.count("https://example.org/x") == 1);
}

TEST_CASE("event media: bundled previews win over the homeserver lookup",
          "[shell][media_images][row]")
{
    MiFx f;
    auto t = std::make_unique<tesseract::Event>();
    t->event_id = "$t";
    t->type = tesseract::EventType::Text;
    t->body = "https://example.com/a https://example.com/b";
    t->bundled_url_previews_present = true;
    tesseract::UrlPreview with_img;
    with_img.matched_url = "https://example.com/a";
    with_img.title = "A";
    with_img.image = mi_src("mxc://hs/pimg");
    tesseract::UrlPreview ask_server;
    ask_server.matched_url = "https://example.com/b";
    tesseract::UrlPreview complete;
    complete.matched_url = "https://example.com/c";
    complete.title = "C";
    t->bundled_url_previews = {with_img, ask_server, complete};
    f.s.ensure_row_media_(*t, false);
    CHECK(f.s.media_fetches_in_flight_.size() == 1); // only A's image
    CHECK(f.s.url_preview_in_flight_.count("https://example.com/b") == 1);
    CHECK(f.s.url_preview_in_flight_.size() == 1);
}

TEST_CASE("event media: membership and read-receipt avatars are fetched on "
          "request only",
          "[shell][media_images][row]")
{
    MiFx f;
    auto m = std::make_unique<tesseract::MembershipStateEvent>();
    m->event_id = "$m";
    m->sender_avatar_url = "mxc://hs/sender";
    m->target_avatar_url = "mxc://hs/target";
    tesseract::ReadReceipt rr;
    rr.avatar_url = "mxc://hs/reader";
    m->read_receipts.push_back(rr);
    f.s.ensure_row_media_(*m, false);
    CHECK(f.s.avatar_mxcs_.empty());
    f.s.ensure_row_media_(*m, true);
    CHECK(f.s.avatar_mxcs_.size() == 3);
}

// ── ensure_row_media_ (MessageRowData) ──────────────────────────────────────

TEST_CASE("row media: kinds mirror the event path", "[shell][media_images][row]")
{
    using K = MessageRowData::Kind;
    MiFx f;
    MessageRowData img;
    img.kind = K::Image;
    img.event_id = "$i";
    img.thumbnail = mi_src("mxc://hs/t");
    img.source = mi_src("mxc://hs/f");
    img.image_animated = true;
    img.blurhash = "LEHV6nWB2yk8pyo0adR*.7kCMdnj";
    img.media_w = 10;
    img.media_h = 20;
    img.sender_avatar_url = "mxc://hs/av";
    img.membership_target_avatar_url = "mxc://hs/target";
    tesseract::ReadReceipt rr;
    rr.avatar_url = "mxc://hs/reader";
    img.read_receipts.push_back(rr);
    f.s.ensure_row_media_(img, true);
    CHECK(f.s.avatar_mxcs_.size() == 3);
    CHECK(f.s.rgba.size() == 1);
    CHECK(std::get<1>(f.s.rgba[0]) == 16);
    const std::size_t after_first = f.s.media_fetches_in_flight_.size();
    tesseract::Settings::instance().prefetch_full_media = true;
    f.s.ensure_row_media_(img, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/f") == 1);
    CHECK(f.s.media_fetches_in_flight_.size() == after_first + 1);

    MessageRowData st;
    st.kind = K::Sticker;
    st.event_id = "$s";
    st.thumbnail = mi_src("mxc://hs/st");
    st.source = mi_src("mxc://hs/sf");
    f.s.ensure_row_media_(st, false);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/st") == 1);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/sf") == 1);

    MessageRowData vo;
    vo.kind = K::Voice;
    vo.event_id = "$v";
    vo.audio_source = mi_src("mxc://hs/voice");
    f.s.ensure_row_media_(vo, false);
    CHECK(f.s.voice_waveform_in_flight_.count("mxc://hs/voice") == 1);
    f.s.ensure_row_media_(vo, false);

    MessageRowData au;
    au.kind = K::Audio;
    au.event_id = "$a";
    au.audio_source = mi_src("mxc://hs/audio");
    f.s.ensure_row_media_(au, false);
    CHECK(f.s.voice_prefetched_.count("mxc://hs/audio") == 1);

    MessageRowData vid;
    vid.kind = K::Video;
    vid.event_id = "$vid";
    vid.video_has_server_thumbnail = true;
    vid.thumbnail = mi_src("mxc://hs/vt");
    vid.source = mi_src("mxc://hs/vs");
    f.s.ensure_row_media_(vid, false);
    CHECK(f.s.video_thumb_in_flight_.empty());
    MessageRowData gen = vid;
    gen.event_id = "$vid2";
    gen.video_has_server_thumbnail = false;
    f.s.ensure_row_media_(gen, false);
    CHECK(f.s.video_thumb_in_flight_.count("$vid2") == 1);

    MessageRowData tx;
    tx.kind = K::Text;
    tx.event_id = "$t";
    tx.body = "see https://example.com/x";
    tesseract::Reaction r;
    r.source = mi_src("mxc://hs/react");
    tx.reactions.push_back(r);
    f.s.ensure_row_media_(tx, false);
    CHECK(f.s.url_preview_in_flight_.count("https://example.com/x") == 1);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/react") == 1);

    MessageRowData html;
    html.kind = K::Notice;
    html.event_id = "$h";
    html.formatted_body = "<a href=\"https://example.net/y\">y</a>";
    f.s.ensure_row_media_(html, false);
    CHECK(f.s.url_preview_in_flight_.count("https://example.net/y") == 1);

    MessageRowData bundled;
    bundled.kind = K::Emote;
    bundled.event_id = "$b";
    bundled.bundled_previews_present = true;
    tesseract::views::UrlPreviewData p1;
    p1.image_source = mi_src("mxc://hs/bimg");
    tesseract::views::UrlPreviewData p2;
    p2.matched_url = "https://example.com/ask";
    bundled.bundled_previews = {p1, p2};
    f.s.ensure_row_media_(bundled, false);
    CHECK(f.s.media_fetches_in_flight_.count(
              MediaShell::thumb_key("mxc://hs/bimg", 64, 64)) == 1);
    CHECK(f.s.url_preview_in_flight_.count("https://example.com/ask") == 1);
}

TEST_CASE("row media: suppressed rows stay unfetched until revealed",
          "[shell][media_images][row]")
{
    using K = MessageRowData::Kind;
    MiFx f;
    tesseract::Settings::instance().media_previews =
        tesseract::Settings::MediaPreviews::Off;
    MessageRowData img;
    img.kind = K::Image;
    img.event_id = "$i";
    img.source = mi_src("mxc://hs/f");
    f.s.ensure_row_media_(img, false);
    CHECK(f.s.media_fetches_in_flight_.empty());
    f.s.reveal_media_fetch_(img);
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/f") == 1);
}

TEST_CASE("reveal_media_fetch_ covers every media kind",
          "[shell][media_images][reveal]")
{
    using K = MessageRowData::Kind;
    MiFx f;
    MessageRowData img;
    img.kind = K::Image;
    img.thumbnail = mi_src("mxc://hs/t");
    f.s.reveal_media_fetch_(img);
    CHECK(f.s.media_fetches_in_flight_.size() == 1);

    MessageRowData st;
    st.kind = K::Sticker;
    st.thumbnail = mi_src("mxc://hs/st");
    f.s.reveal_media_fetch_(st);
    MessageRowData st2;
    st2.kind = K::Sticker;
    st2.source = mi_src("mxc://hs/st2");
    f.s.reveal_media_fetch_(st2);
    MessageRowData img2;
    img2.kind = K::Image;
    img2.source = mi_src("mxc://hs/i2");
    f.s.reveal_media_fetch_(img2);
    CHECK(f.s.media_fetches_in_flight_.size() == 4);

    MessageRowData vid;
    vid.kind = K::Video;
    vid.event_id = "$v";
    vid.video_has_server_thumbnail = true;
    vid.thumbnail = mi_src("mxc://hs/vt");
    f.s.reveal_media_fetch_(vid);
    MessageRowData gen;
    gen.kind = K::Video;
    gen.event_id = "$g";
    gen.source = mi_src("mxc://hs/gs");
    f.s.reveal_media_fetch_(gen);
    CHECK(f.s.video_thumb_in_flight_.count("$g") == 1);
    CHECK(f.s.media_fetches_in_flight_.size() == 5);
}

// ── MSC4278 policy ──────────────────────────────────────────────────────────

TEST_CASE("effective preview mode honours per-room overrides and join rule",
          "[shell][media_images][policy]")
{
    using P = tesseract::Settings::MediaPreviews;
    MiFx f;
    auto& st = tesseract::Settings::instance();
    st.media_previews = P::Private;
    std::string rule;
    CHECK(f.s.effective_preview_mode_("!r:x", rule) == P::Private);
    CHECK(rule.empty());

    auto& ov = f.s.room_preview_overrides_["!r:x"];
    ov.join_rule = "invite";
    CHECK(f.s.effective_preview_mode_("!r:x", rule) == P::Private);
    CHECK(rule == "invite");
    CHECK(f.s.should_auto_preview_("!r:x")); // private room: allowed
    ov.join_rule = "public";
    CHECK_FALSE(f.s.should_auto_preview_("!r:x"));
    CHECK(f.s.media_allowed_("!r:x", true)); // own media exempt
    CHECK(f.s.media_preview_hidden_("!r:x", "$e", false));
    f.s.revealed_events_.insert("$e");
    CHECK_FALSE(f.s.media_preview_hidden_("!r:x", "$e", false));

    ov.has_media_previews = true;
    ov.media_previews = tesseract::MediaPreviewConfig::Mode::On;
    CHECK(f.s.effective_preview_mode_("!r:x", rule) == P::On);
    CHECK(f.s.should_auto_preview_("!r:x"));
}

TEST_CASE("room preview override fetch: request, ready, updated, commit",
          "[shell][media_images][policy]")
{
    MiFx f;
    f.s.ensure_room_preview_override_("");
    CHECK(f.s.pending_preview_overrides_.empty());
    f.s.ensure_room_preview_override_("!r:x");
    f.s.ensure_room_preview_override_("!r:x"); // in flight
    REQUIRE(f.s.pending_preview_overrides_.size() == 1);
    const auto id = f.s.pending_preview_overrides_.begin()->first;

    f.s.handle_room_preview_override_ready_ui_(
        999, R"({})"); // unknown request
    CHECK(f.s.room_preview_overrides_.empty());
    f.s.handle_room_preview_override_ready_ui_(
        id, R"({"has_media_previews":true,"media_previews":0,"join_rule":"public"})");
    REQUIRE(f.s.room_preview_overrides_.count("!r:x") == 1);
    CHECK(f.s.room_preview_overrides_["!r:x"].has_media_previews);
    CHECK(f.s.room_preview_overrides_["!r:x"].media_previews ==
          tesseract::MediaPreviewConfig::Mode::Off);
    CHECK(f.s.relayouts >= 1);
    f.s.ensure_room_preview_override_("!r:x"); // already known
    CHECK(f.s.pending_preview_overrides_.empty());

    // Live update only applies for the active account.
    f.s.handle_room_media_preview_override_updated_ui_(
        "@me:x", "!r:x", R"({"has_media_previews":false})");
    CHECK(f.s.room_preview_overrides_["!r:x"].has_media_previews); // no account yet
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>();
    f.s.active_account_->user_id = "@me:x";
    f.s.handle_room_media_preview_override_updated_ui_(
        "@other:x", "!r:x", R"({"has_media_previews":false})");
    CHECK(f.s.room_preview_overrides_["!r:x"].has_media_previews);
    f.s.handle_room_media_preview_override_updated_ui_(
        "@me:x", "!r:x", R"({"has_media_previews":false})");
    CHECK_FALSE(f.s.room_preview_overrides_["!r:x"].has_media_previews);

    f.s.commit_room_media_preview_override_(
        "", true, tesseract::MediaPreviewConfig::Mode::Off);
    f.s.commit_room_media_preview_override_(
        "!r:x", true, tesseract::MediaPreviewConfig::Mode::Private);
    CHECK(f.s.room_preview_overrides_["!r:x"].has_media_previews);
    CHECK(f.s.room_preview_overrides_["!r:x"].media_previews ==
          tesseract::MediaPreviewConfig::Mode::Private);
    f.s.commit_room_media_preview_override_(
        "!r:x", false, tesseract::MediaPreviewConfig::Mode::Off);
    CHECK_FALSE(f.s.room_preview_overrides_["!r:x"].has_media_previews);
    f.s.seed_room_media_section_("!r:x"); // no room view: no-op
}

TEST_CASE("global media preview config: apply, updated and fetched",
          "[shell][media_images][policy]")
{
    using P = tesseract::Settings::MediaPreviews;
    MiFx f;
    f.s.apply_media_preview_config_(P::Off, false);
    CHECK(tesseract::Settings::instance().media_previews == P::Off);
    CHECK_FALSE(tesseract::Settings::instance().invite_avatars);
    f.s.apply_media_preview_config_(P::Private, true);
    f.s.apply_media_preview_config_(P::On, true);
    CHECK(tesseract::Settings::instance().media_previews == P::On);

    f.s.handle_media_preview_config_fetched_ui_(
        1, R"({"media_previews":1,"invite_avatars":false})");
    CHECK(tesseract::Settings::instance().media_previews == P::Private);
    CHECK_FALSE(tesseract::Settings::instance().invite_avatars);
    f.s.handle_media_preview_config_fetched_ui_(
        2, R"({"media_previews":2,"invite_avatars":true})");
    CHECK(tesseract::Settings::instance().media_previews == P::On);

    const auto rid = f.s.next_request_id_;
    f.s.handle_media_preview_config_updated_ui_("@me:x", "{}"); // no account
    CHECK(f.s.next_request_id_ == rid);
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>();
    f.s.active_account_->user_id = "@me:x";
    f.s.handle_media_preview_config_updated_ui_("@other:x", "{}");
    CHECK(f.s.next_request_id_ == rid);
    f.s.handle_media_preview_config_updated_ui_("@me:x", "{}");
    CHECK(f.s.next_request_id_ == rid + 1);
}

TEST_CASE("room security state requests are tracked and ignored without the "
          "settings view",
          "[shell][media_images][policy]")
{
    MiFx f;
    f.s.fetch_room_security_state_("", nullptr);
    CHECK(f.s.pending_security_state_requests_.empty());
    f.s.fetch_room_security_state_("!r:x", nullptr);
    REQUIRE(f.s.pending_security_state_requests_.size() == 1);
    const auto id = f.s.pending_security_state_requests_.begin()->first;
    f.s.handle_room_security_state_ready_ui_(id + 100, {}); // unknown
    CHECK(f.s.pending_security_state_requests_.size() == 1);
    f.s.handle_room_security_state_ready_ui_(id, {}); // no room view
    CHECK(f.s.pending_security_state_requests_.empty());

    MiFx none;
    none.s.client_ = nullptr;
    none.s.fetch_room_security_state_("!r:x", nullptr);
    CHECK(none.s.pending_security_state_requests_.empty());
}

// ── completion handlers and visible rows ────────────────────────────────────

TEST_CASE("media chunks: stream, finish and fail", "[shell][media_images][stream]")
{
    MiFx f;
    std::vector<std::uint64_t> totals;
    std::vector<std::uint8_t> got;
    int done = 0;
    int failed = 0;
    std::uint8_t fail_status = 0;
    auto id = f.s.begin_media_stream_req_(
        0,
        [&](std::vector<std::uint8_t>&& c, std::uint64_t t)
        {
            totals.push_back(t);
            got.insert(got.end(), c.begin(), c.end());
        },
        [&] { ++done; },
        [&](std::uint8_t st)
        {
            ++failed;
            fail_status = st;
        });
    f.s.handle_media_chunk_ui_(id + 50, {1}, 0, 1); // unknown id
    f.s.handle_media_chunk_ui_(id, {1, 2}, 0, 10);
    f.s.handle_media_chunk_ui_(id, {3}, 0, 10);
    CHECK(got == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(f.s.pending_media_streams_.size() == 1);
    f.s.handle_media_chunk_ui_(id, {}, 1, 10);
    CHECK(done == 1);
    CHECK(f.s.pending_media_streams_.empty());
    f.s.handle_media_chunk_ui_(id, {}, 1, 10); // gone
    CHECK(done == 1);

    auto id2 = f.s.begin_media_stream_req_(0, nullptr, [&] { ++done; },
                                           [&](std::uint8_t st)
                                           {
                                               ++failed;
                                               fail_status = st;
                                           });
    f.s.handle_media_chunk_ui_(id2, {}, 3, 0);
    CHECK(failed == 1);
    CHECK(fail_status == 3);
    CHECK(done == 1);
}

TEST_CASE("visible rows prioritise in-flight requests and lazily fetch media",
          "[shell][media_images][visible]")
{
    MiFx f;
    f.s.active_media_group_ = 5;
    f.s.begin_media_req_(5, [](std::vector<std::uint8_t>&&) {}, {}, "mxc://hs/k");
    f.s.on_visible_rows_changed_({"mxc://hs/k", "mxc://hs/none"}); // no room view
    f.s.on_visible_rows_changed_({});

    tesseract::Client c2;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    f.s.room_view_ = rv.get();
    f.s.on_visible_rows_changed_({"mxc://hs/k"}); // unlaid-out list: no range
    CHECK(f.s.media_prepped_event_ids_.empty());
}

TEST_CASE("media preview gating hooks the message list",
          "[shell][media_images][policy]")
{
    MiFx f;
    f.s.wire_media_preview_gating_(nullptr);
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    auto* ml = rv->message_list();
    MessageRowData row;
    row.kind = MessageRowData::Kind::Image;
    row.event_id = "$i";
    row.source = mi_src("mxc://hs/f");
    ml->set_messages({row}, true);
    f.s.wire_media_preview_gating_(ml);
    REQUIRE(ml->on_reveal_media);
    ml->on_reveal_media("$other"); // not in the list
    CHECK(f.s.revealed_events_.count("$other") == 1);
    CHECK(f.s.media_fetches_in_flight_.empty());
    ml->on_reveal_media("$i");
    CHECK(f.s.media_fetches_in_flight_.count("mxc://hs/f") == 1);
}

TEST_CASE("member profile events patch the shown rows", 
          "[shell][media_images][member]")
{
    MiFx f;
    auto rv = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomInfo info;
    info.id = "!r:x";
    rv->set_room(info);
    MessageRowData row;
    row.event_id = "$1";
    row.sender = "@bob:x";
    row.sender_name = "Bob";
    rv->message_list()->set_messages({row}, true);
    f.s.room_view_ = rv.get();

    auto m = std::make_unique<tesseract::MembershipStateEvent>();
    m->target_user_id = "@bob:x";
    m->target_display_name = "Robert";
    m->action = tesseract::MembershipAction::DisplayNameChanged;
    f.s.note_member_event_("!r:x", *m);
    CHECK(rv->message_list()->messages()[0].sender_name == "Robert");

    // Other rooms, joins and non-membership events are ignored.
    m->target_display_name = "Nope";
    f.s.note_member_event_("!other:x", *m);
    m->action = tesseract::MembershipAction::Joined;
    f.s.note_member_event_("!r:x", *m);
    tesseract::Event plain;
    plain.type = tesseract::EventType::Text;
    f.s.note_member_event_("!r:x", plain);
    CHECK(rv->message_list()->messages()[0].sender_name == "Robert");

    // Our own profile event re-reads the strip avatar for the shown room.
    f.s.active_account_ = std::make_shared<tesseract::AccountSession>();
    f.s.active_account_->user_id = "@me:x";
    f.s.active_account_->client = std::make_unique<tesseract::Client>();
    m->target_user_id = "@me:x";
    m->action = tesseract::MembershipAction::AvatarChanged;
    f.s.note_member_event_("!r:x", *m);
    CHECK(f.s.own_room_avatar_in_flight_.size() == 1);
    f.s.pump();
}

// ── map tiles ────────────────────────────────────────────────────────────────

TEST_CASE("map tiles fetch through the same pipeline and remember failures",
          "[shell][media_images][tile]")
{
    MiFx f;
    // Unique coordinates: the tile disk cache outlives a test case in-process.
    const int x = 1000 + f.s.pending_media_.size() + static_cast<int>(MiFx::next_id()++);
    f.s.ensure_tile_async(7, x, 3);
    f.s.ensure_tile_async(7, x, 3); // in flight
    CHECK(f.s.tile_fetches_in_flight_.size() == 1);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {9, 9});
    f.s.pump();
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(std::get<1>(f.s.delivered[0]) == MediaShell::MediaKind::Tile);
    CHECK(f.s.tile_fetches_in_flight_.empty());

    // A failed download is remembered so the tile is not retried.
    f.s.ensure_tile_async(7, x + 1, 3);
    f.s.pump();
    f.s.handle_media_ready_ui_(f.s.only_pending(), {});
    f.s.pump();
    CHECK(f.s.tile_fetch_failed_.size() == 1);
    f.s.ensure_tile_async(7, x + 1, 3);
    CHECK(f.s.tile_fetches_in_flight_.empty());
}
