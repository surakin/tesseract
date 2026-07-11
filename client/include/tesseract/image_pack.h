#pragma once
/// MSC2545 image pack C++ types — surfaces from the Rust aggregator
/// (`sdk/src/image_packs.rs`). All field names mirror the FFI shape so the
/// translation in `ffi_convert.h` is a flat per-field copy.

#include <cstdint>
#include <string>
#include <vector>

namespace tesseract
{

/// MSC2545 usage bits. A pack/image with neither bit set means "both" per
/// spec (missing/empty `usage` is treated as "all usages allowed"); the
/// aggregator normalises that to `PackUsage::Any` before surfacing.
enum class PackUsage : std::uint8_t
{
    None = 0,
    Sticker = 1 << 0,
    Emoticon = 1 << 1,
    Any = Sticker | Emoticon,
};

constexpr inline PackUsage operator|(PackUsage a, PackUsage b) noexcept
{
    return static_cast<PackUsage>(static_cast<std::uint8_t>(a) |
                                  static_cast<std::uint8_t>(b));
}
constexpr inline PackUsage operator&(PackUsage a, PackUsage b) noexcept
{
    return static_cast<PackUsage>(static_cast<std::uint8_t>(a) &
                                  static_cast<std::uint8_t>(b));
}
constexpr inline bool any(PackUsage v) noexcept
{
    return static_cast<std::uint8_t>(v) != 0;
}

/// Identifies where a pack came from. `User` is the per-account pack stored
/// in the user's account_data; `Room` packs live as state events in another
/// room which the user has globally enabled via
/// `im.ponies.emote_rooms` / `m.image_pack.rooms`.
enum class PackSourceKind : std::uint8_t
{
    User,
    Room
};

struct ImagePack
{
    /// Synthetic stable id. `"user"` for the user pack, `"room:!id/key"` for
    /// every (room_id, state_key) combination. Stable across rebuilds — UIs
    /// can use it as a hash key.
    std::string id;
    std::string display_name;
    std::string avatar_url; // mxc://
    std::string attribution;
    PackUsage usage = PackUsage::Any;
    PackSourceKind source_kind = PackSourceKind::User;
    /// For `PackSourceKind::Room`, the source room's Matrix ID. Empty
    /// otherwise.
    std::string source_room;
    /// For `PackSourceKind::Room`, the state_key inside that room. Empty
    /// otherwise. May legitimately be the empty string ("default" pack).
    std::string source_state_key;
    /// Only meaningful for `PackSourceKind::Room`; true when this pack is in
    /// the user's explicit `m.image_pack.rooms`/`im.ponies.emote_rooms`
    /// subscription list, as opposed to being visible only because the user
    /// is joined to the source room.
    bool is_subscribed = false;
};

struct ImagePackImage
{
    /// Pack this entry belongs to (matches `ImagePack::id`).
    std::string pack_id;
    /// Short identifier — the map key in `images` (e.g. "happy").
    std::string shortcode;
    /// mxc:// URI of the image.
    std::string url;
    /// Optional human-readable body; falls back to shortcode at send time.
    std::string body;
    /// Literal MSC2545 `info` object serialised as JSON ("{}" when absent).
    std::string info_json;
    PackUsage usage = PackUsage::Any;
    /// Tesseract-private flag (`im.tesseract.favorite`). Surfaced in the
    /// sticker picker's Favorites tab. Not part of MSC2545.
    bool favorite = false;
};

/// Predicate filter used by `Client::list_pack_images`.
enum class PackUsageFilter : std::uint8_t
{
    Any,
    Sticker,
    Emoticon
};

inline const char* pack_usage_filter_to_str(PackUsageFilter f) noexcept
{
    switch (f)
    {
    case PackUsageFilter::Sticker:
        return "sticker";
    case PackUsageFilter::Emoticon:
        return "emoticon";
    case PackUsageFilter::Any:
        [[fallthrough]];
    default:
        return "any";
    }
}

/// One resolved image passed to `Client::save_room_pack` — `url` must
/// already be an uploaded `mxc://` URI (upload any brand-new image bytes
/// via `Client::upload_media` first); `save_room_pack` never uploads
/// anything itself.
struct PackImageInput
{
    std::string shortcode;
    std::string url; // mxc://
    std::string body;
    std::string info_json;
};

} // namespace tesseract
