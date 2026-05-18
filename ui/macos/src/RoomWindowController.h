#pragma once
#import <Cocoa/Cocoa.h>
#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include "views/MessageListView.h"
#include <string>
#include <unordered_map>

@interface RoomWindowController : NSWindowController <NSWindowDelegate>
@end

// C++ factory — called from MacShell::create_secondary_room_window_.
namespace tesseract
{
RoomWindowBase* make_mac_room_window(
    ShellBase* shell, const std::string& room_id,
    const std::unordered_map<std::string, views::UrlPreviewData>* preview_data);
} // namespace tesseract
