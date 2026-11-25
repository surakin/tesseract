#pragma once

// Importing a world into a room and exporting the room's world to a directory (no widgets, no
// threads: the caller runs these on a worker). The client calls are injected so the logic is
// tested without a server. Every byte read from outside (a zip, a directory, the room's bundle)
// is checked by the SDK's bundle code, which enforces the limits.

#include <tesseract/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views::world
{

struct TransferOps
{
    std::function<WorldBundlePack(const std::string& dir)> pack;
    std::function<WorldBundleExtract(const std::vector<std::uint8_t>&, const std::string& dest)>
        extract;
    std::function<Result(const std::string& room_id, const std::string& name,
                         const std::vector<std::uint8_t>& bytes)>
        set_room_world;
    std::function<RoomWorldRead(const std::string& room_id)> room_world;
    std::function<Result(const RoomWorld&, std::vector<std::uint8_t>&)> download;
    // The directory holding the shipped meadow bundle (exported when the room has no world).
    std::function<std::string()> meadow_dir;
    // Optional: called with the bundle hash after import_world published it, so the importer's own
    // world does not ask for their consent.
    std::function<void(const std::string& hash)> trust_published;
};

// The SDK's bundle code and a client's world calls. `with_client` runs its argument with the client
// and returns true, or returns false (and runs nothing) when the client is gone; each call reaches
// the client only for its own duration.
using WithClient = std::function<bool(const std::function<void(Client&)>&)>;
TransferOps client_transfer_ops(WithClient with_client);
TransferOps client_transfer_ops(Client& client);

// Publishes the world at `path` as `room_id`'s world: a directory is packed, a `.zip` file is
// read; either way the bundle is extracted to a scratch directory first (nothing is uploaded when
// it does not check out) and the manifest's name names the world. A symbolic link is refused.
// Returns false with the reason in `error` (may be null).
bool import_world(const TransferOps& ops, const std::string& room_id, const std::string& path,
                  std::string* error);
bool import_world(Client& client, const std::string& room_id, const std::string& path,
                  std::string* error);

// Writes `room_id`'s world into a new subdirectory of `dest_dir` named after it (the shipped
// meadow's own files when the room has no world) and puts that directory in `*out_dir`. Refuses
// to write into an existing directory; nothing is left behind on failure.
bool export_world(const TransferOps& ops, const std::string& room_id, const std::string& dest_dir,
                  std::string* error, std::string* out_dir = nullptr);
bool export_world(Client& client, const std::string& room_id, const std::string& dest_dir,
                  std::string* error, std::string* out_dir = nullptr);

// A world's name as a directory name: no separators, control or direction-changing characters, no
// leading dot or trailing dot/space, no Windows reserved name, at most 64 bytes; "world" when
// nothing is left.
std::string safe_world_dir_name(const std::string& name);

} // namespace tesseract::views::world
