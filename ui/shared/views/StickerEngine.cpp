#include "views/StickerEngine.h"

#include "views/image_pack_order.h"
#include "views/text_match.h"

#include <tesseract/client.h>

#include <set>
#include <utility>

namespace tesseract::views
{

namespace
{
constexpr std::string_view kPrefix = "/sticker ";

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
} // namespace

std::optional<std::string> StickerEngine::match(std::string_view text) const
{
    if (text.size() < kPrefix.size() || text.substr(0, kPrefix.size()) != kPrefix)
    {
        return std::nullopt;
    }
    std::string_view rest = text.substr(kPrefix.size());
    std::size_t b = 0;
    std::size_t e = rest.size();
    while (b < e && is_space(rest[b]))
    {
        ++b;
    }
    while (e > b && is_space(rest[e - 1]))
    {
        --e;
    }
    return std::string(rest.substr(b, e - b));
}

std::vector<tesseract::ImagePackImage> search_stickers(
    const std::vector<tesseract::ImagePack>& ordered_packs,
    const std::function<std::vector<tesseract::ImagePackImage>(const std::string& pack_id)>&
        images_for_pack,
    const std::vector<tesseract::ImagePackImage>& favorites, const std::string& query)
{
    std::vector<tesseract::ImagePackImage> out;
    std::set<std::pair<std::string, std::string>> seen;
    auto consider = [&](const tesseract::ImagePackImage& img)
    {
        if (!contains_ci(img.body, query) && !contains_ci(img.shortcode, query))
        {
            return;
        }
        if (seen.insert({img.pack_id, img.shortcode}).second)
        {
            out.push_back(img);
        }
    };
    for (const auto& f : favorites)
    {
        consider(f);
    }
    if (images_for_pack)
    {
        for (const auto& pack : ordered_packs)
        {
            for (const auto& img : images_for_pack(pack.id))
            {
                consider(img);
            }
        }
    }
    return out;
}

std::vector<tesseract::ImagePack>
sticker_packs_for_room(tesseract::Client& client, const std::string& room_id,
                       const std::vector<std::string>& parent_space_ids)
{
    // Keep only packs whose usage allows stickers (per MSC2545; missing usage
    // means any, which the SDK already normalised to PackUsage::Any).
    std::vector<tesseract::ImagePack> filtered;
    for (auto& p : client.list_image_packs())
    {
        if (any(p.usage & PackUsage::Sticker))
        {
            filtered.push_back(std::move(p));
        }
    }
    return order_picker_packs(std::move(filtered), room_id, parent_space_ids);
}

std::vector<tesseract::ImagePackImage>
search_room_stickers(tesseract::Client& client, const std::string& room_id,
                     const std::vector<std::string>& parent_space_ids, const std::string& query)
{
    return search_stickers(
        sticker_packs_for_room(client, room_id, parent_space_ids),
        [&client](const std::string& pack_id)
        {
            return client.list_pack_images(pack_id, PackUsageFilter::Sticker);
        },
        client.list_favorite_stickers(), query);
}

} // namespace tesseract::views