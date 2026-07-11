#include "views/image_pack_order.h"

namespace tesseract::views
{

bool is_pack_picker_visible(const tesseract::ImagePack& pack,
                           const std::string& current_room_id)
{
    if (pack.source_kind == tesseract::PackSourceKind::User)
        return true;
    if (!current_room_id.empty() && pack.source_room == current_room_id)
        return true;
    return pack.is_subscribed;
}

std::vector<tesseract::ImagePack> order_picker_packs(
    std::vector<tesseract::ImagePack> packs, const std::string& current_room_id)
{
    std::vector<tesseract::ImagePack> personal;
    std::vector<tesseract::ImagePack> current_room;
    std::vector<tesseract::ImagePack> subscribed;

    for (auto& p : packs)
    {
        if (!is_pack_picker_visible(p, current_room_id))
        {
            continue; // not personal, not current room, not subscribed — hide
        }
        if (p.source_kind == tesseract::PackSourceKind::User)
        {
            personal.push_back(std::move(p));
        }
        else if (!current_room_id.empty() && p.source_room == current_room_id)
        {
            current_room.push_back(std::move(p));
        }
        else
        {
            subscribed.push_back(std::move(p));
        }
    }

    std::vector<tesseract::ImagePack> out;
    out.reserve(personal.size() + current_room.size() + subscribed.size());
    for (auto& p : personal) out.push_back(std::move(p));
    for (auto& p : current_room) out.push_back(std::move(p));
    for (auto& p : subscribed) out.push_back(std::move(p));
    return out;
}

} // namespace tesseract::views
