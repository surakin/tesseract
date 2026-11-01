#pragma once

#include "views/MessageListView.h"

#include <tesseract/types.h>

#include <string>
#include <utility>
#include <vector>

namespace tk
{
class CanvasFactory;
class PixmapCache;
} // namespace tk

namespace tesseract::screenshot
{

struct Fixture
{
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
    std::vector<tesseract::RoomInfo> rooms;
    std::string selected_room_id;
    std::vector<views::MessageRowData> messages;

    // Thread scene: the root row first, then its replies.
    std::string thread_root_id;
    std::vector<views::MessageRowData> thread_messages;

    // Room-info scene.
    std::vector<tesseract::RoomMember> members;
    std::vector<std::pair<std::string, tesseract::PresenceState>> presence;

    // Display names shown in the composer's typing indicator.
    std::vector<std::string> typing_names;
};

/// Build the fixed, network-free conversation used by CI screenshots.
Fixture make_fixture();

/// Decode the fixture's checked-in avatar and media assets into the normal
/// thumbnail cache. Returns false if any source asset is missing or invalid.
bool install_assets(tk::CanvasFactory& factory, tk::PixmapCache& cache);

/// Cache keys (`tk::CacheKey::media(key)`) that install_assets() stores.
std::vector<std::string> installed_asset_keys();

} // namespace tesseract::screenshot
