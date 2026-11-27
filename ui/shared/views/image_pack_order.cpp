#include "views/image_pack_order.h"

#include <algorithm>
#include <cstddef>
#include <unordered_set>

namespace tesseract::views
{

bool is_pack_picker_visible(const tesseract::ImagePack& pack, const std::string& current_room_id,
                            const std::vector<std::string>& parent_space_ids)
{
    if (pack.source_kind == tesseract::PackSourceKind::User)
    {
        return true;
    }
    if (!current_room_id.empty() && pack.source_room == current_room_id)
    {
        return true;
    }
    if (std::find(parent_space_ids.begin(), parent_space_ids.end(), pack.source_room) !=
        parent_space_ids.end())
    {
        return true;
    }
    return pack.is_subscribed;
}

std::vector<bool> picker_visible_mask(const std::vector<tesseract::ImagePack>& packs,
                                      const std::string& current_room_id,
                                      const std::vector<std::string>& parent_space_ids)
{
    std::vector<bool> mask(packs.size());
    std::unordered_set<std::string> shown_ids;
    for (std::size_t i = 0; i < packs.size(); ++i)
    {
        mask[i] = is_pack_picker_visible(packs[i], current_room_id, parent_space_ids);
        if (mask[i])
        {
            shown_ids.insert(packs[i].id);
        }
    }
    // Coverers are stable packs, which are never covered themselves, so one
    // pass against the shown set is enough.
    for (std::size_t i = 0; i < packs.size(); ++i)
    {
        const auto& cov = packs[i].covered_by;
        if (mask[i] && std::any_of(cov.begin(), cov.end(),
                                   [&](const std::string& id)
                                   {
                                       return id != packs[i].id && shown_ids.count(id) > 0;
                                   }))
        {
            mask[i] = false;
        }
    }
    return mask;
}

std::vector<tesseract::ImagePack>
order_picker_packs(std::vector<tesseract::ImagePack> packs, const std::string& current_room_id,
                   const std::vector<std::string>& parent_space_ids)
{
    std::vector<tesseract::ImagePack> personal;
    std::vector<tesseract::ImagePack> current_room;
    std::vector<tesseract::ImagePack> space;
    std::vector<tesseract::ImagePack> subscribed;

    const std::vector<bool> visible = picker_visible_mask(packs, current_room_id, parent_space_ids);
    for (std::size_t i = 0; i < packs.size(); ++i)
    {
        auto& p = packs[i];
        if (!visible[i])
        {
            continue; // not personal/current room/space/subscribed, or a covered duplicate
        }
        if (p.source_kind == tesseract::PackSourceKind::User)
        {
            personal.push_back(std::move(p));
        }
        else if (!current_room_id.empty() && p.source_room == current_room_id)
        {
            current_room.push_back(std::move(p));
        }
        else if (std::find(parent_space_ids.begin(), parent_space_ids.end(), p.source_room) !=
                 parent_space_ids.end())
        {
            space.push_back(std::move(p));
        }
        else
        {
            subscribed.push_back(std::move(p));
        }
    }

    std::vector<tesseract::ImagePack> out;
    out.reserve(personal.size() + current_room.size() + space.size() + subscribed.size());
    for (auto& p : personal)
    {
        out.push_back(std::move(p));
    }
    for (auto& p : current_room)
    {
        out.push_back(std::move(p));
    }
    for (auto& p : space)
    {
        out.push_back(std::move(p));
    }
    for (auto& p : subscribed)
    {
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace tesseract::views
