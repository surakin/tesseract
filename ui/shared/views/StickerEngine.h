#pragma once
#include <tesseract/image_pack.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

/// Stateless helper for the `/sticker <query>` composer command. Like
/// `/gif`, the command owns the whole composer text.
class StickerEngine
{
public:
    /// Returns the trimmed query when `text` starts with `"/sticker "` (the
    /// query may be empty), or `nullopt` when it is not a `/sticker` command.
    std::optional<std::string> match(std::string_view text) const;
};

/// Pure sticker search over already-ordered sticker packs. Favourites that
/// match come first, then matches in pack order; duplicates (same pack_id +
/// shortcode) are dropped. Matches `body` or `shortcode`, case-insensitively.
std::vector<tesseract::ImagePackImage> search_stickers(
    const std::vector<tesseract::ImagePack>& ordered_packs,
    const std::function<std::vector<tesseract::ImagePackImage>(const std::string& pack_id)>&
        images_for_pack,
    const std::vector<tesseract::ImagePackImage>& favorites, const std::string& query);

/// Packs that may hold stickers (usage allows it), filtered and ordered for the
/// composer in `room_id` exactly as the sticker picker shows them.
std::vector<tesseract::ImagePack>
sticker_packs_for_room(tesseract::Client& client, const std::string& room_id,
                       const std::vector<std::string>& parent_space_ids);

/// Client-backed `search_stickers` for the sticker picker's packs + favourites.
std::vector<tesseract::ImagePackImage>
search_room_stickers(tesseract::Client& client, const std::string& room_id,
                     const std::vector<std::string>& parent_space_ids, const std::string& query);

} // namespace tesseract::views
