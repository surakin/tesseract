#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tk
{

// Namespace discriminant for CacheKey. Distinct from MediaKind (media_kind.h):
// MediaKind answers "which cache instance / decode-size clamp does this
// fetch use" (routing); CacheUsage answers "which namespace within that
// cache instance does this key belong to." They are related but not 1:1 —
// MediaKind::MediaImage, ::Sticker, and ::Reaction all deliberately share
// CacheUsage::Media (the same plain-mxc keyspace in image_cache_), so a
// sticker and an inline image referencing the same mxc reuse one decode,
// same as before this type existed.
enum class CacheUsage : std::uint8_t
{
    Thumbnail,      // size-namespaced: avatars + inline/reply thumbnails.
                    // thumbnail_cache_ / anim_cache_. Uses id + w + h.
    Media,          // plain mxc/fetch_token: full images, stickers,
                    // reactions, picker previews. image_cache_ / anim_cache_.
    VideoThumbnail, // event_id-keyed generated video first-frame thumbnail.
                    // image_cache_ (memory) + media_disk_cache_ (disk).
    VideoFull,      // src-keyed fully-buffered video bytes. media_disk_cache_.
    Blurhash,       // event_id-keyed synthetic decoded placeholder. image_cache_.
    Tile,           // "z/x/y"-keyed map tile. image_cache_.
    FullresViewer,  // url-keyed lightbox full-resolution decode.
                    // viewer_fullres_ + media_disk_cache_ + in-flight set.
    GifSource,      // url-keyed GIF-picker strip original bytes. media_disk_cache_.
    GifPreview,     // url-keyed GIF-picker decoded static preview (per-window).
    PillBitmap,     // hash-keyed rasterized mention-pill bitmap.
};

// Typed replacement for the hand-rolled string-prefix scheme every
// decoded-media cache used to key on (e.g. "t{w}x{h}:" + url,
// "video_thumb::" + event_id, "blurhash::" + event_id, "tile:" + z/x/y).
// `usage` makes each namespace a distinct type instead of a string
// convention, so two different call sites can never collide by accident.
struct CacheKey
{
    CacheUsage  usage = CacheUsage::Media;
    std::string id;
    int w = 0; // meaningful only for CacheUsage::Thumbnail
    int h = 0; // meaningful only for CacheUsage::Thumbnail

    bool operator==(const CacheKey&) const = default;

    // Canonical string form, reproducing today's exact hand-built prefixed
    // strings byte-for-byte (e.g. Thumbnail{id,w,h} -> "t{w}x{h}:{id}").
    // Used only where a flat string is still required: MediaDiskCache's
    // on-disk filename hash (must stay stable across process restarts, so
    // existing cached files on disk remain valid) and debug logging. NOT
    // used for in-memory map lookups — CacheKeyHash hashes the fields
    // directly to avoid building a string on every peek()/acquire().
    std::string to_string() const;

    static CacheKey thumbnail(std::string id, int w, int h);
    static CacheKey media(std::string id);
    static CacheKey video_thumbnail(std::string event_id);
    static CacheKey video_full(std::string src);
    static CacheKey blurhash(std::string event_id);
    static CacheKey tile(int z, int x, int y);
    static CacheKey fullres(std::string url);
    static CacheKey gif_source(std::string url);
    static CacheKey gif_preview(std::string url);
    static CacheKey pill_bitmap(std::size_t hash);
};

struct CacheKeyHash
{
    std::size_t operator()(const CacheKey& k) const noexcept;
};

} // namespace tk
