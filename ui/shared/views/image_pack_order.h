#pragma once

// order_picker_packs — pure, Client-free filtering + ordering logic shared
// by EmojiPicker and StickerPicker's custom-pack tabs, and (via
// is_pack_picker_visible) by the shortcode popup's candidate list. Mirrors
// the compute_room_settings_changes / would_lock_out_of_permissions idiom
// (RoomSettingsView.h) of keeping business logic testable without a
// widget/Client dependency.

#include <tesseract/image_pack.h>

#include <string>
#include <vector>

namespace tesseract::views
{

// Returns true iff `pack` should be visible to the emoji/sticker pickers
// and the shortcode popup for a composer currently showing
// `current_room_id`: the personal pack, current_room_id's own pack, the
// pack of any Space that current_room_id is (nested-)in (`parent_space_ids`
// — see ShellBase::parent_spaces_for_room_), or any pack the user has
// explicitly subscribed to via im.ponies.emote_rooms/m.image_pack.rooms.
// Hides packs from any other room that only appears in list_image_packs()
// because that room was recently visited (see active_rooms in the Rust
// aggregator) but isn't the room in question, isn't one of its parent
// spaces, and isn't subscribed.
bool is_pack_picker_visible(const tesseract::ImagePack& pack,
                           const std::string& current_room_id,
                           const std::vector<std::string>& parent_space_ids);

// Filters `packs` (already usage-filtered by the caller) down to
// is_pack_picker_visible() entries, then stable-partitions the survivors
// into personal pack first, then the pack sourced from `current_room_id`
// (if any), then packs sourced from any of `parent_space_ids` in their
// existing relative order, then every subscribed-room pack in their
// existing relative order. `current_room_id` empty means no room context
// (e.g. viewing the room list) — buckets 2 and 3 are simply empty in that
// case.
std::vector<tesseract::ImagePack> order_picker_packs(
    std::vector<tesseract::ImagePack> packs, const std::string& current_room_id,
    const std::vector<std::string>& parent_space_ids);

} // namespace tesseract::views
