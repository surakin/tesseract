#pragma once

#include "MediaViewerItem.h"
#include "MessageListView.h"

#include <string>
#include <vector>

namespace tesseract::views
{

// Pure helpers that build MediaViewerItems from the timeline's hit structs, and
// describe how an item should be saved. No widget or shell state involved.

MediaViewerItem item_from_image_hit(const MessageListView::ImageHit& hit);
MediaViewerItem item_from_video_hit(const MessageListView::VideoHit& hit);
// An avatar / profile picture: source and display key are the same mxc URL, the
// size is unknown and `name` doubles as caption and save name.
MediaViewerItem item_from_avatar(const std::string& url, const std::string& name);

// One MediaViewerItem per gallery cell (all kinds, in order). An item's caption
// is its own body unless that is empty or just its filename, in which case the
// gallery's shared caption is used.
std::vector<MediaViewerItem> items_from_gallery(const MessageListView::GalleryHit& hit);

// Everything a shell needs to open its native "save as" dialog for an item.
struct MediaSaveSpec
{
    std::string title;          // dialog title (already translated)
    std::string suggested_name; // initial file name
    std::string filter_name;    // translated group name, e.g. "Images"; empty = no specific filter
    std::vector<std::string> patterns; // e.g. {"*.jpg", "*.png"}; empty when filter_name is empty
};

MediaSaveSpec media_save_spec(const MediaViewerItem& item);

} // namespace tesseract::views
