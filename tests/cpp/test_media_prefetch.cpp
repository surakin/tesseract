#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"
#include <tesseract/settings.h>

#include <tesseract/visual.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct MediaPrefetchFakeImage : tk::Image
{
    int width() const override
    {
        return 4;
    }
    int height() const override
    {
        return 4;
    }
    std::size_t memory_bytes() const override
    {
        return 64;
    }
};

// Minimal concrete ShellBase, same shape as PickerImageCacheTestShell in
// test_picker_image_cache.cpp. decode_image_ is test-controlled: by default
// it returns a still image for any non-empty bytes; set `decode_delay` to
// make it block (simulating a slow/stuck decode) so tests can exercise
// run_media_prefetch_impl_'s deadline behavior deterministically.
struct MediaPrefetchTestShell : tesseract::test::TestShellBase
{
    // Workers post into ui_queue_ until they finish, and ui_queue_ is part of
    // this derived object, so let them finish before it goes away.
    ~MediaPrefetchTestShell() override
    {
        media_prefetch_pool_.wait_idle(std::chrono::seconds(5));
    }

    void post_to_ui_(std::function<void()> fn) override
    {
        // Queue only: the test thread plays the UI thread and runs these via
        // pump_ui_until(). The callbacks touch UI-thread-only shell state
        // (media_prefetch_in_flight_, media_decode_pending_until_ms_), so
        // running them inline on the calling media_prefetch_pool_ worker, as
        // this used to, raced the other worker and the test thread on those
        // containers and could corrupt the heap.
        {
            std::lock_guard<std::mutex> lock(post_to_ui_done_mu);
            ui_queue_.push_back(std::move(fn));
        }
        post_to_ui_done_cv.notify_all();
    }

    // Run posted UI callbacks on the calling (test) thread until `done()`
    // holds or `timeout` passes. post_to_ui_calls counts callbacks that have
    // finished running, so waiting on it means the straggler's cache store
    // has actually happened, not just its decode.
    template <typename Pred>
    bool pump_ui_until(Pred done, std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            std::vector<std::function<void()>> batch;
            {
                std::unique_lock<std::mutex> lock(post_to_ui_done_mu);
                if (done())
                    return true;
                if (!post_to_ui_done_cv.wait_until(lock, end,
                                                   [this] { return !ui_queue_.empty(); }))
                    return done();
                batch.swap(ui_queue_);
            }
            for (auto& fn : batch)
            {
                fn();
                ++post_to_ui_calls;
            }
        }
    }
    void on_media_bytes_ready_(const tk::CacheKey&, MediaKind,
                               std::vector<uint8_t>) override
    {
    }
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int,
                                int) override
    {
        ++decode_calls;
        if (decode_delay > std::chrono::milliseconds{0})
        {
            std::this_thread::sleep_for(decode_delay);
        }
        if (bytes.empty())
        {
            return {};
        }
        DecodedImage d;
        d.still = std::make_unique<MediaPrefetchFakeImage>();
        return d;
    }
    void start_anim_tick_() override
    {
        ++anim_tick_starts;
    }
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override
    {
    }

    int anim_tick_starts = 0;
    std::atomic<int> decode_calls{0};
    std::chrono::milliseconds decode_delay{0};

    // Guards ui_queue_; the cv is signalled whenever a callback is queued.
    // post_to_ui_calls is only touched by the test thread (pump_ui_until).
    std::mutex post_to_ui_done_mu;
    std::condition_variable post_to_ui_done_cv;
    std::vector<std::function<void()>> ui_queue_;
    int post_to_ui_calls = 0;

    tk::PixmapCache&        image_cache()     { return am_.image_cache(); }
    tk::PixmapCache&        thumbnail_cache() { return am_.thumbnail_cache(); }
    tk::AnimImageCache&     anim_cache()      { return am_.anim_cache(); }

    std::unordered_set<std::string>& media_fetches_in_flight()
    {
        return media_fetches_in_flight_;
    }
    std::unordered_set<std::string>& media_prefetch_in_flight()
    {
        return media_prefetch_in_flight_;
    }

    using ShellBase::DecodedImage;
    using ShellBase::MediaPrefetchKey;
    using ShellBase::media_prefetch_decode_clamp_;
    using ShellBase::media_decode_clamp_;
    using ShellBase::media_prefetch_supports_kind_;
    using ShellBase::run_media_prefetch_impl_;
    using ShellBase::store_decoded_media_;
    using ShellBase::store_media_bytes_;
    using ShellBase::thumb_key;
};

std::vector<std::uint8_t> fake_bytes(const std::string& tag)
{
    return std::vector<std::uint8_t>(tag.begin(), tag.end());
}

} // namespace

// ── media_prefetch_supports_kind_ / media_prefetch_decode_clamp_ ───────────

// The prefetch disk key for an avatar depends on Settings::animate_avatars
// (animated thumbnails live under an "anim:"-namespaced key). Tests that seed
// the plain avatar key pin the setting off; the animated key has its own test.
struct MediaPrefetchAnimateAvatars
{
    bool saved = tesseract::Settings::instance().animate_avatars;
    explicit MediaPrefetchAnimateAvatars(bool v) { tesseract::Settings::instance().animate_avatars = v; }
    ~MediaPrefetchAnimateAvatars() { tesseract::Settings::instance().animate_avatars = saved; }
};

TEST_CASE("media_prefetch_supports_kind_ excludes only Tile", "[media-prefetch]")
{
    using MK = ShellBase::MediaKind;
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::MediaImage));
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::MediaThumbnail));
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::Sticker));
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::Reaction));
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::RoomAvatar));
    CHECK(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::UserAvatar));
    CHECK_FALSE(MediaPrefetchTestShell::media_prefetch_supports_kind_(MK::Tile));
}

TEST_CASE("media_prefetch_decode_clamp_ uses a fixed, unscaled size for avatars",
          "[media-prefetch]")
{
    using MK = ShellBase::MediaKind;
    const auto room = MediaPrefetchTestShell::media_prefetch_decode_clamp_(MK::RoomAvatar);
    const auto user = MediaPrefetchTestShell::media_prefetch_decode_clamp_(MK::UserAvatar);
    CHECK(room.first == tesseract::visual::kAvatarCacheSize);
    CHECK(room.second == tesseract::visual::kAvatarCacheSize);
    CHECK(user.first == tesseract::visual::kAvatarCacheSize);
    CHECK(user.second == tesseract::visual::kAvatarCacheSize);
}

// ── store_decoded_media_ ────────────────────────────────────────────────────

TEST_CASE("store_decoded_media_ routes a still image by kind", "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    ShellBase::DecodedImage d;
    d.still = std::make_unique<MediaPrefetchFakeImage>();
    CHECK(s.store_decoded_media_(tk::CacheKey::media("mxc://a/1"), ShellBase::MediaKind::MediaImage,
                                 std::move(d)));
    CHECK(s.image_cache().contains(tk::CacheKey::media("mxc://a/1")));
    CHECK_FALSE(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://a/1")));
}

TEST_CASE("store_decoded_media_ routes MediaThumbnail into thumbnail_cache_",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    ShellBase::DecodedImage d;
    d.still = std::make_unique<MediaPrefetchFakeImage>();
    CHECK(s.store_decoded_media_(tk::CacheKey::media("mxc://a/2"), ShellBase::MediaKind::MediaThumbnail,
                                 std::move(d)));
    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://a/2")));
    CHECK_FALSE(s.image_cache().contains(tk::CacheKey::media("mxc://a/2")));
}

TEST_CASE("store_decoded_media_ defers animated frames to the lazy windowed decode path",
          "[media-prefetch]")
{
    // Fix for a prefetch-vs-windowed-decode race: prefetch's own decode has
    // no session/windowing, so if it stored multi-frame results directly,
    // it would win the store race against the lazy path's windowed decode
    // for exactly the on-screen stickers windowing exists to help (prefetch
    // runs on every pre-paint pass, ahead of the cell's own paint-triggered
    // decode). store_decoded_media_ now leaves all animated content to the
    // lazy path — see its doc comment in ShellBase.cpp.
    MediaPrefetchTestShell s;
    ShellBase::DecodedImage d;
    d.frames.push_back(std::make_unique<MediaPrefetchFakeImage>());
    d.delays_ms.push_back(50);
    CHECK_FALSE(s.store_decoded_media_(tk::CacheKey::media("mxc://a/3"), ShellBase::MediaKind::Sticker,
                                       std::move(d)));
    CHECK_FALSE(s.anim_cache().has(tk::CacheKey::media("mxc://a/3")));
    CHECK(s.anim_tick_starts == 0);
}

TEST_CASE("store_decoded_media_ on an empty decode returns false",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    CHECK_FALSE(s.store_decoded_media_(tk::CacheKey::media("mxc://a/4"), ShellBase::MediaKind::MediaImage,
                                       ShellBase::DecodedImage{}));
    CHECK_FALSE(s.image_cache().contains(tk::CacheKey::media("mxc://a/4")));
}

TEST_CASE("store_decoded_media_ routes RoomAvatar/UserAvatar into thumbnail_cache_",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    ShellBase::DecodedImage d1;
    d1.still = std::make_unique<MediaPrefetchFakeImage>();
    CHECK(s.store_decoded_media_(tk::CacheKey::media("mxc://room/1"), ShellBase::MediaKind::RoomAvatar,
                                 std::move(d1)));
    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://room/1")));

    ShellBase::DecodedImage d2;
    d2.still = std::make_unique<MediaPrefetchFakeImage>();
    CHECK(s.store_decoded_media_(tk::CacheKey::media("mxc://user/1"), ShellBase::MediaKind::UserAvatar,
                                 std::move(d2)));
    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://user/1")));
}

TEST_CASE("store_decoded_media_ stores a multi-frame avatar as its first frame when animation is off",
          "[media-prefetch]")
{
    MediaPrefetchAnimateAvatars pin_still(false);
    MediaPrefetchTestShell s;
    // With animate_avatars off an avatar mxc whose bytes decode into multiple
    // frames is shown as a still: it never goes through anim_cache_. A
    // frames-only decode result (no `still`) falls back to its first frame.
    ShellBase::DecodedImage d;
    d.frames.push_back(std::make_unique<MediaPrefetchFakeImage>());
    d.delays_ms.push_back(50);
    CHECK(s.store_decoded_media_(tk::CacheKey::media("mxc://room/2"), ShellBase::MediaKind::RoomAvatar,
                                 std::move(d)));
    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://room/2")));
    CHECK_FALSE(s.anim_cache().has(tk::CacheKey::media("mxc://room/2")));
}

TEST_CASE("run_media_prefetch_impl_ warms an avatar via its animated disk key when animating",
          "[media-prefetch]")
{
    MediaPrefetchAnimateAvatars pin_animated(true);
    MediaPrefetchTestShell s;
    const std::string mxc = "mxc://user/anim-1";
    const int px = tesseract::visual::kAvatarCacheSize;
    // Animated thumbnails are cached under "anim:"+mxc (see ensure_user_avatar_).
    s.store_media_bytes_(tk::CacheKey::thumbnail("anim:" + mxc, px, px), fake_bytes("avatar-bytes"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::thumbnail(mxc, px, px), ShellBase::MediaKind::UserAvatar}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() + std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    CHECK(s.decode_calls == 1);
}

// ── run_media_prefetch_impl_ ────────────────────────────────────────────────

TEST_CASE("run_media_prefetch_impl_ warms the cache for a disk hit",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    s.store_media_bytes_(tk::CacheKey::media("mxc://b/1"), fake_bytes("hello"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("mxc://b/1"), ShellBase::MediaKind::MediaImage}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK(s.image_cache().contains(tk::CacheKey::media("mxc://b/1")));
    CHECK(s.decode_calls == 1);
}

TEST_CASE("run_media_prefetch_impl_ warms an avatar via its namespaced disk key",
          "[media-prefetch]")
{
    MediaPrefetchAnimateAvatars pin_still(false);
    MediaPrefetchTestShell s;
    // Avatars' disk key is namespaced by size (unlike the general kinds,
    // whose disk key equals the plain memory key) — see
    // ensure_user_avatar_'s thumb_key(mxc, avatar_px, avatar_px). Seed the
    // SAME namespaced key ensure_user_avatar_ would use (current_scale_
    // defaults to 1.0, so avatar_px == kAvatarCacheSize exactly).
    const std::string mxc = "mxc://user/warm-1";
    const std::string disk_key =
        s.thumb_key(mxc, tesseract::visual::kAvatarCacheSize,
                    tesseract::visual::kAvatarCacheSize);
    s.store_media_bytes_(tk::CacheKey::thumbnail(mxc, tesseract::visual::kAvatarCacheSize,
                                                 tesseract::visual::kAvatarCacheSize),
                         fake_bytes("avatar-bytes"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::thumbnail(mxc, tesseract::visual::kAvatarCacheSize,
                                 tesseract::visual::kAvatarCacheSize),
         ShellBase::MediaKind::UserAvatar}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    // Stored under the plain mxc (the memory key), not the namespaced disk
    // key — matching what the paint-time avatar lookup peeks.
    CHECK(s.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    CHECK_FALSE(s.thumbnail_cache().contains(tk::CacheKey::media(disk_key)));
    CHECK(s.decode_calls == 1);
}

TEST_CASE("run_media_prefetch_impl_ misses an avatar stored under the plain (unscaled) key",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    // Seeding the disk cache under the PLAIN mxc (as if it were a general
    // MediaImage-kind entry) must NOT satisfy an avatar prefetch — proves
    // the namespacing is actually applied, not just present in a comment.
    const std::string mxc = "mxc://user/warm-2";
    s.store_media_bytes_(tk::CacheKey::media(mxc), fake_bytes("avatar-bytes"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::thumbnail(mxc, tesseract::visual::kAvatarCacheSize,
                                 tesseract::visual::kAvatarCacheSize),
         ShellBase::MediaKind::UserAvatar}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK_FALSE(s.thumbnail_cache().contains(tk::CacheKey::media(mxc)));
    CHECK(s.decode_calls == 0);
}

TEST_CASE("run_media_prefetch_impl_ leaves a genuine disk miss untouched",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("mxc://b/2"), ShellBase::MediaKind::MediaImage}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK_FALSE(s.image_cache().contains(tk::CacheKey::media("mxc://b/2")));
    // No bytes on disk -> never reaches decode_image_ at all.
    CHECK(s.decode_calls == 0);
}

TEST_CASE("run_media_prefetch_impl_ skips already-decoded keys without touching disk",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    s.image_cache().store(tk::CacheKey::media("mxc://b/3"), std::make_unique<MediaPrefetchFakeImage>());
    // No bytes stored on disk for this key — if the filter didn't skip it,
    // the disk read would simply miss; decode_calls == 0 proves the skip
    // happened up front rather than "happened to miss".
    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("mxc://b/3"), ShellBase::MediaKind::MediaImage}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK(s.decode_calls == 0);
}

TEST_CASE("run_media_prefetch_impl_ filters out thumb:: sentinel keys",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    s.store_media_bytes_(tk::CacheKey::media("thumb::event1"), fake_bytes("hello"));
    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("thumb::event1"), ShellBase::MediaKind::MediaThumbnail}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK_FALSE(s.thumbnail_cache().contains(tk::CacheKey::media("thumb::event1")));
    CHECK(s.decode_calls == 0);
}

TEST_CASE("run_media_prefetch_impl_ filters out unsupported kinds (RoomAvatar/UserAvatar/Tile)",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    s.store_media_bytes_(tk::CacheKey::media("mxc://b/4"), fake_bytes("hello"));
    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("mxc://b/4"), ShellBase::MediaKind::RoomAvatar}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    CHECK_FALSE(s.thumbnail_cache().contains(tk::CacheKey::media("mxc://b/4")));
    CHECK(s.decode_calls == 0);
}

TEST_CASE("run_media_prefetch_impl_ respects the item cap", "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    std::vector<ShellBase::MediaPrefetchKey> keys;
    for (int i = 0; i < 10; ++i)
    {
        std::string key = "mxc://b/cap" + std::to_string(i);
        s.store_media_bytes_(tk::CacheKey::media(key), fake_bytes("x"));
        keys.push_back({tk::CacheKey::media(key), ShellBase::MediaKind::MediaImage});
    }
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/3);

    CHECK(s.decode_calls == 3);
}

TEST_CASE("run_media_prefetch_impl_ never blocks past its deadline even when decode hangs",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    s.decode_delay = std::chrono::milliseconds{400};
    s.store_media_bytes_(tk::CacheKey::media("mxc://b/5"), fake_bytes("slow"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media("mxc://b/5"), ShellBase::MediaKind::MediaImage}};
    const auto start = std::chrono::steady_clock::now();
    s.run_media_prefetch_impl_(
        keys, start + std::chrono::milliseconds(50), /*max_items=*/12);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // The call must return at (or very close to) the 50ms deadline, not
    // after the 400ms decode — this is the core GTK4-safety property: a
    // slow/stuck decode can never stall the caller past its budget.
    CHECK(elapsed < std::chrono::milliseconds(250));
    // Not yet warmed — the decode was still in flight when we stopped
    // waiting for it.
    CHECK_FALSE(s.image_cache().contains(tk::CacheKey::media("mxc://b/5")));

    // The straggler is still decoding on a media_prefetch_pool_ worker
    // thread; it then posts the callback that actually calls
    // store_decoded_media_. Run that callback here, on the test thread
    // standing in for the UI thread — bounded, a safety net against a hung
    // test, not the thing under test — before asserting the result landed,
    // exactly once.
    REQUIRE(s.pump_ui_until([&s] { return s.post_to_ui_calls >= 1; },
                            std::chrono::seconds(2)));
    CHECK(s.image_cache().contains(tk::CacheKey::media("mxc://b/5")));
    CHECK(s.decode_calls == 1);
    // >= 1, not == 1: the straggler's own post_to_ui_ call is one of them,
    // but its callback also calls schedule_relayout_(), which posts a
    // second, nested continuation via post_to_ui_alive_ — both are
    // expected, not a sign of a duplicate store (store_decoded_media_ only
    // ever ran once, per decode_calls == 1 above).
    CHECK(s.post_to_ui_calls >= 1);
}

// ── single-flight guard: prefetch must NOT touch media_fetches_in_flight_ ──

TEST_CASE("run_media_prefetch_impl_ never marks a key in media_fetches_in_flight_",
          "[media-prefetch]")
{
    MediaPrefetchAnimateAvatars pin_still(false);
    // Regression: sharing media_fetches_in_flight_ with the network path let a
    // cold-cache prefetch mark an avatar "in flight", so the lazy
    // ensure_room_avatar_ call during the very next paint saw the guard set
    // and never dispatched the real download — the avatar stayed blank until
    // clicked. The prefetch owns a SEPARATE set (media_prefetch_in_flight_).
    MediaPrefetchTestShell s;
    s.decode_delay = std::chrono::milliseconds{400};
    const std::string mxc = "mxc://user/inflight-1";
    const std::string disk_key =
        s.thumb_key(mxc, tesseract::visual::kAvatarCacheSize,
                    tesseract::visual::kAvatarCacheSize);
    s.store_media_bytes_(tk::CacheKey::thumbnail(mxc, tesseract::visual::kAvatarCacheSize,
                                                 tesseract::visual::kAvatarCacheSize),
                         fake_bytes("avatar-bytes"));

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::thumbnail(mxc, tesseract::visual::kAvatarCacheSize,
                                 tesseract::visual::kAvatarCacheSize),
         ShellBase::MediaKind::RoomAvatar}};
    const auto start = std::chrono::steady_clock::now();
    s.run_media_prefetch_impl_(keys, start + std::chrono::milliseconds(50),
                               /*max_items=*/12);

    // Task still decoding here (400ms delay, 50ms deadline). The network
    // path's guard must be untouched; the prefetch's own guard holds it.
    CHECK(s.media_fetches_in_flight().count(disk_key) == 0);
    CHECK(s.media_prefetch_in_flight().count(disk_key) == 1);

    REQUIRE(s.pump_ui_until([&s] { return s.post_to_ui_calls >= 1; },
                            std::chrono::seconds(2)));
    // Slot freed once the straggler finished, so the next paint pass can
    // redispatch if still needed.
    CHECK(s.media_prefetch_in_flight().count(disk_key) == 0);
    CHECK(s.media_fetches_in_flight().count(disk_key) == 0);
}

TEST_CASE("run_media_prefetch_impl_ skips a key the network path is already fetching",
          "[media-prefetch]")
{
    MediaPrefetchTestShell s;
    const std::string key = "mxc://b/already-fetching";
    s.store_media_bytes_(tk::CacheKey::media(key), fake_bytes("bytes"));
    s.media_fetches_in_flight().insert(key);

    std::vector<ShellBase::MediaPrefetchKey> keys{
        {tk::CacheKey::media(key), ShellBase::MediaKind::MediaImage}};
    s.run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() +
                                          std::chrono::seconds(2),
                               /*max_items=*/12);

    // Did not compete with the in-flight network fetch's own decode step.
    CHECK(s.decode_calls == 0);
    CHECK(s.media_prefetch_in_flight().count(key) == 0);
}

TEST_CASE("media_decode_clamp_ takes a picker sticker's size from its key",
          "[media-prefetch]")
{
    const auto picker = tk::CacheKey::picker_sticker("mxc://b/s", 192, 192);
    CHECK(MediaPrefetchTestShell::media_decode_clamp_(
              ShellBase::MediaKind::Sticker, picker) ==
          std::pair<int, int>{192, 192});
    // A plain sticker key keeps the timeline's kStickerSize clamp.
    CHECK(MediaPrefetchTestShell::media_decode_clamp_(
              ShellBase::MediaKind::Sticker, tk::CacheKey::media("mxc://b/s")) ==
          MediaPrefetchTestShell::media_prefetch_decode_clamp_(
              ShellBase::MediaKind::Sticker));
    // Same mxc at different sizes never shares an entry with the plain key.
    CHECK(picker != tk::CacheKey::media("mxc://b/s"));
    CHECK(picker != tk::CacheKey::picker_sticker("mxc://b/s", 96, 96));
}
