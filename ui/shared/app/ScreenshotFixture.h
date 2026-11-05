#pragma once

#include "views/MessageListView.h"

#include <tesseract/types.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// A path for `mxc://<absolute path>` URLs, which the fixture uses to point at
/// real files on disk. Ordinary `mxc://server/media-id` URLs give nullopt.
std::optional<std::filesystem::path> local_media_path(std::string_view url);

} // namespace tesseract::screenshot
