#pragma once

#include <cstdint>
#include <string>

namespace tk
{

// Tags which decoded-image cache (and, for the "general" 4 kinds, which
// size clamp) a piece of media maps to. Mirrors ShellBase::MediaKind (see
// ShellBase.h's "Media kind tag" section, which is now a type alias for
// this enum) — lives here in tk/, not app/ShellBase.h, purely so
// views/*.h's collect_prefetchable_media_keys() methods can name it
// without a views/ -> app/ include cycle (ShellBase.h already includes
// views/*.h fully).
enum class MediaKind : std::uint8_t
{
    RoomAvatar,     // -> thumbnail_cache_, triggers room-list repaint
    UserAvatar,     // -> thumbnail_cache_, triggers message-list repaint
    MediaImage,     // -> anim_cache_ or image_cache_ (full-size)
    MediaThumbnail, // -> anim_cache_ or thumbnail_cache_ (inline preview)
    Tile,           // -> image_cache_["tile:z/x/y"], triggers full message-list repaint
    Sticker,        // -> image_cache_ (full-size), decode clamped to kStickerSize
    Reaction,       // -> image_cache_ (full-size), decode clamped to reaction icon size
};

// One media reference a view's collect_prefetchable_media_keys() wants the
// pre-paint disk-cache prefetch pass (ShellBase::run_media_prefetch_) to
// warm. `key` is the exact resolved in-memory cache key the view's own
// image_provider_-style lookup will use during paint (e.g. a
// MediaSource::fetch_token(), never a bare mxc:// that still needs
// transformation) — this is what ShellBase checks/stores in
// thumbnail_cache_/image_cache_/anim_cache_, for every kind.
//
// `w`/`h` are the requested display size (pre-DPI-scale), and matter ONLY
// for MediaKind::MediaThumbnail: unlike every other kind, a thumbnail's
// on-disk cache key is namespaced by size + the live display scale (see
// ShellBase::ensure_media_thumbnail_ / ShellBase::thumb_key) and therefore
// differs from `key`. The collector must pass the SAME w/h its own
// ensure_media_thumbnail_ call site uses, or the disk lookup silently
// misses. Ignored (leave at 0) for every other kind.
//
// Only MediaKind::MediaImage/MediaThumbnail/Sticker/Reaction are actually
// prefetched today — see ShellBase::run_media_prefetch_impl_'s scope note.
struct MediaPrefetchKey
{
    std::string key;
    MediaKind   kind;
    int w = 0;
    int h = 0;
};

} // namespace tk
