#include "RoomListPreview.h"

#include "views/RoomListView.h"

#include "tesseract/types.h"
#include "tk/host.h"
#include "tk/i18n.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace tesseract::views
{

namespace
{

constexpr float kRoomPreviewWidth = 260.0f;
constexpr float kRoomPreviewHeight = 190.0f;

std::vector<tesseract::RoomInfo> make_preview_rooms()
{
    std::vector<tesseract::RoomInfo> rooms;

    tesseract::RoomInfo general;
    general.id = "!preview-general:localhost";
    general.name = tk::tr("General");
    general.last_message_kind = "text";
    general.last_message_sender_name = tk::tr("Alex");
    general.last_message_body = tk::tr("Hello my friend!");
    general.last_activity_ts = 1'700'000'060'000ULL;
    general.unread_count = 2;
    general.notification_count = 2;
    rooms.push_back(std::move(general));

    tesseract::RoomInfo design;
    design.id = "!preview-design:localhost";
    design.name = tk::tr("Design");
    design.last_message_kind = "text";
    design.last_message_sender_name = tk::tr("Sam");
    design.last_message_body = tk::tr("The new icons look great");
    design.last_activity_ts = 1'700'000'050'000ULL;
    rooms.push_back(std::move(design));

    tesseract::RoomInfo dm;
    dm.id = "!preview-dm:localhost";
    dm.name = tk::tr("Jordan");
    dm.is_direct = true;
    dm.last_message_kind = "text";
    dm.last_message_body = tk::tr("See you tomorrow");
    dm.last_activity_ts = 1'700'000'040'000ULL;
    rooms.push_back(std::move(dm));

    return rooms;
}

} // namespace

RoomListPreview::RoomListPreview()
{
    list_ = add_child(tk::create_widget<RoomListView>(this));
    list_->set_search_hidden(true);
    list_->set_rooms(make_preview_rooms());
}

void RoomListPreview::set_compact(bool compact)
{
    if (!list_)
    {
        return;
    }
    list_->set_compact(compact);
    if (host())
    {
        host()->request_repaint_rect(bounds_);
    }
}

tk::Size RoomListPreview::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {std::min(constraints.w, kRoomPreviewWidth), kRoomPreviewHeight};
}

} // namespace tesseract::views
