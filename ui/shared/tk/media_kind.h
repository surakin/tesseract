#pragma once

#include "tk/cache_key.h"

#include <cstdint>

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
// warm. `key` is the exact resolved CacheKey the view's own
// image_provider_-style lookup will use during paint — this is what
// ShellBase checks/stores in thumbnail_cache_/image_cache_/anim_cache_, for
// every kind. For MediaKind::MediaThumbnail, `key` must already carry the
// same w/h (post display-scale) as the call site's own
// ensure_media_thumbnail_ call, or the disk lookup silently misses — see
// CacheKey::thumbnail / ShellBase::ensure_media_thumbnail_.
//
// Only MediaKind::MediaImage/MediaThumbnail/Sticker/Reaction are actually
// prefetched today — see ShellBase::run_media_prefetch_impl_'s scope note.
struct MediaPrefetchKey
{
    CacheKey  key;
    MediaKind kind;
    // Room-scoped media-fetch group this key belongs to (0 for group-less
    // content: pickers, room-list avatars/tiles). Views themselves don't
    // know this — ShellBase::run_media_prefetch_() fills it in per source
    // after collecting — but it needs to travel with the key so a discarded
    // animated decode's direct hand-off to ensure_media_image_() (see
    // ShellBase::store_decoded_media_'s doc comment) dispatches under the
    // same group the row's own lazy fetch would have used, rather than
    // guessing 0 and having should_deliver_ silently drop it for a
    // room-scoped row.
    std::uint64_t group_id = 0;
};

} // namespace tk
