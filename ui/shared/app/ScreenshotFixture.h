#pragma once

#include "views/MessageListView.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

namespace tk
{
class CanvasFactory;
class PixmapCache;
}

namespace tesseract::screenshot
{

struct Fixture
{
    std::string                       user_id;
    std::string                       display_name;
    std::string                       avatar_url;
    std::vector<tesseract::RoomInfo>  rooms;
    std::string                       selected_room_id;
    std::vector<views::MessageRowData> messages;
};

/// Build the fixed, network-free conversation used by CI screenshots.
Fixture make_fixture();

/// Decode the fixture's checked-in avatar assets into the normal thumbnail
/// cache. Returns false if any source asset is missing or invalid.
bool install_avatar_assets(tk::CanvasFactory& factory, tk::PixmapCache& cache);

} // namespace tesseract::screenshot
