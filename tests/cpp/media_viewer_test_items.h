#pragma once

// Tiny MediaViewerItem builders shared by the viewer-related test files.

#include "views/MediaViewerItem.h"

#include <cstdint>
#include <string>
#include <utility>

namespace tesseract::views::test
{

inline MediaViewerItem image_item(std::string url, std::string thumb = {},
                                  std::string caption = {}, int w = 0, int h = 0)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Image;
    item.source = std::move(url);
    item.thumbnail = std::move(thumb);
    item.filename = caption;
    item.caption = std::move(caption);
    item.width = w;
    item.height = h;
    return item;
}

inline MediaViewerItem video_item(std::string source, std::string thumb = {},
                                  std::string mime = "video/mp4",
                                  std::uint64_t duration_ms = 0, int w = 0, int h = 0)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Video;
    item.source = std::move(source);
    item.thumbnail = std::move(thumb);
    item.mime_type = std::move(mime);
    item.duration_ms = duration_ms;
    item.width = w;
    item.height = h;
    return item;
}

} // namespace tesseract::views::test
