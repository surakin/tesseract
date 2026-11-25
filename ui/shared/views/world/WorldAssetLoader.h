#pragma once

// Loads a glTF world asset into the engine: from the disk cache of minted resources when it holds the
// asset, otherwise by importing it (and then storing the result). Engine calls stay on the calling thread.

#include "views/world/AssetCacheStore.h"

#include <engine/engine.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tesseract::views::world
{

/// What a load made, read in place of `engine_import_result`; `level_id` is the last id.
struct ImportedAsset
{
    std::vector<std::uint64_t> ids;
    std::vector<std::uint32_t> types;
    std::vector<std::string> paths;
    std::uint64_t level_id = 0;
    std::string summary;
};

/// FNV-1a 64 over flags, scale, clip_rate, id_seed and the bytes of path_prefix.
std::uint64_t options_fingerprint(const engine_import_options& o);

/// "<size>-<mtime ticks>" of the library file, or "unknown" when it cannot be stat'ed.
std::string library_stamp(const std::string& library_path);

class WorldAssetLoader
{
public:
    /// `store` may be null (cache off); `caps` is `engine_info::caps`.
    WorldAssetLoader(const engine_api* api, const engine_host::Engine& engine, std::uint32_t caps,
                     const AssetCacheStore* store, std::string engine_stamp);

    /// False only when the import itself fails (the engine's last error says why).
    bool load(const std::string& name, const std::vector<std::uint8_t>& glb,
              const engine_import_options& options, ImportedAsset* out, AssetLoadReport* report);

private:
    bool try_cache_(const std::string& name, const std::string& key, ImportedAsset* out,
                    std::string* note);
    void store_entry_(const std::string& name, const std::string& key,
                      const engine_import_result& r, const ImportedAsset& asset,
                      AssetLoadReport* report);

    const engine_api* api_;
    const engine_host::Engine& engine_;
    std::uint32_t caps_;
    const AssetCacheStore* store_;
    std::string stamp_;
};

} // namespace tesseract::views::world
