#include "tk/cache_key.h"

#include "tk/hash_combine.h"

namespace tk
{

std::string CacheKey::to_string() const
{
    switch (usage)
    {
        case CacheUsage::Thumbnail:
            return "t" + std::to_string(w) + "x" + std::to_string(h) + ":" + id;
        case CacheUsage::Media:
            return id;
        case CacheUsage::VideoThumbnail:
            // Disk-persisted under this exact format historically (the old
            // in-memory-only sentinel used "thumb::" instead, but that key
            // was never written to media_disk_cache_, so matching the disk
            // format here is what keeps existing cached files valid).
            return "video_thumb::" + id;
        case CacheUsage::VideoFull:
            return "video-full:" + id;
        case CacheUsage::Blurhash:
            return "blurhash::" + id;
        case CacheUsage::Tile:
            return "tile:" + id;
        case CacheUsage::FullresViewer:
            return "fullres:" + id;
        case CacheUsage::GifSource:
            return "gifsrc:" + id;
        case CacheUsage::GifPreview:
            return "gifpreview:" + id;
        case CacheUsage::PillBitmap:
            return "pill:" + id;
    }
    return id;
}

CacheKey CacheKey::thumbnail(std::string id, int w, int h)
{
    return CacheKey{CacheUsage::Thumbnail, std::move(id), w, h};
}

CacheKey CacheKey::media(std::string id)
{
    return CacheKey{CacheUsage::Media, std::move(id)};
}

CacheKey CacheKey::video_thumbnail(std::string event_id)
{
    return CacheKey{CacheUsage::VideoThumbnail, std::move(event_id)};
}

CacheKey CacheKey::video_full(std::string src)
{
    return CacheKey{CacheUsage::VideoFull, std::move(src)};
}

CacheKey CacheKey::blurhash(std::string event_id)
{
    return CacheKey{CacheUsage::Blurhash, std::move(event_id)};
}

CacheKey CacheKey::tile(int z, int x, int y)
{
    return CacheKey{CacheUsage::Tile, std::to_string(z) + "/" + std::to_string(x) +
                                           "/" + std::to_string(y)};
}

CacheKey CacheKey::fullres(std::string url)
{
    return CacheKey{CacheUsage::FullresViewer, std::move(url)};
}

CacheKey CacheKey::gif_source(std::string url)
{
    return CacheKey{CacheUsage::GifSource, std::move(url)};
}

CacheKey CacheKey::gif_preview(std::string url)
{
    return CacheKey{CacheUsage::GifPreview, std::move(url)};
}

CacheKey CacheKey::pill_bitmap(std::size_t hash)
{
    return CacheKey{CacheUsage::PillBitmap, std::to_string(hash)};
}

std::size_t CacheKeyHash::operator()(const CacheKey& k) const noexcept
{
    std::size_t h = hash_combine(0, static_cast<std::size_t>(k.usage));
    h = hash_combine(h, std::hash<std::string>{}(k.id));
    h = hash_combine(h, static_cast<std::size_t>(k.w));
    h = hash_combine(h, static_cast<std::size_t>(k.h));
    return h;
}

} // namespace tk
