#include "views/world/WorldAssetLoader.h"

#include <tesseract/hash.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace tesseract::views::world
{

namespace
{

constexpr std::uint64_t kMaxBlobBytes = 256ull * 1024 * 1024;

void fnv(std::uint64_t* h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i)
    {
        *h ^= p[i];
        *h *= 1099511628211ull;
    }
}

double ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string last_error(const engine_api* api)
{
    engine_host::Buffer message;
    (void)engine_host::LastError(api, &message);
    return message.Data() != nullptr ? std::string(message.Data(), message.Size()) : std::string();
}

std::string describe_name(const engine_api* api, engine_host::Status status)
{
    const engine_string name = status.Name(api);
    return std::string(name.data, name.length);
}

std::string describe(const engine_api* api, engine_host::Status status)
{
    const engine_string name = status.Name(api);
    std::string text(name.data, name.length);
    const std::string detail = last_error(api);
    return detail.empty() ? text : text + ": " + detail;
}

ImportedAsset asset_from(const CachedImport& e)
{
    ImportedAsset a;
    for (const CachedResource& r : e.resources)
    {
        a.ids.push_back(r.id);
        a.types.push_back(r.type);
        a.paths.push_back(r.path);
    }
    a.level_id = e.level_id;
    a.summary = e.summary;
    return a;
}

} // namespace

std::uint64_t options_fingerprint(const engine_import_options& o)
{
    std::uint64_t h = 14695981039346656037ull;
    fnv(&h, &o.flags, sizeof(o.flags));
    fnv(&h, &o.scale, sizeof(o.scale));
    fnv(&h, &o.clip_rate, sizeof(o.clip_rate));
    fnv(&h, &o.id_seed, sizeof(o.id_seed));
    if (o.path_prefix.data != nullptr && o.path_prefix.length > 0)
    {
        fnv(&h, o.path_prefix.data, o.path_prefix.length);
    }
    return h;
}

std::string library_stamp(const std::string& library_path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto size = fs::file_size(library_path, ec);
    if (ec)
    {
        return "unknown";
    }
    const auto mtime = fs::last_write_time(library_path, ec);
    if (ec)
    {
        return "unknown";
    }
    return std::to_string(size) + "-" + std::to_string(mtime.time_since_epoch().count());
}

WorldAssetLoader::WorldAssetLoader(const engine_api* api, const engine_host::Engine& engine,
                                   std::uint32_t caps, const AssetCacheStore* store,
                                   std::string engine_stamp)
    : api_(api), engine_(engine), caps_(caps), store_(store), stamp_(std::move(engine_stamp))
{
}

bool WorldAssetLoader::try_cache_(const std::string& name, const std::string& key,
                                  ImportedAsset* out, std::string* note)
{
    std::optional<CachedImport> entry = store_->load(name, key);
    if (!entry || entry->resources.empty())
    {
        return false; // a miss; an unreadable file is the store's business
    }
    std::vector<engine_resource_add_info> infos;
    infos.reserve(entry->resources.size());
    for (const CachedResource& r : entry->resources)
    {
        auto info = engine_host::Init<engine_resource_add_info>();
        info.flags = ENGINE_RESOURCE_CHECK_HASH;
        info.id = r.id;
        info.path = engine_host::Str(r.path.data(), r.path.size());
        info.bytes = r.blob.data();
        info.size = r.blob.size();
        std::memcpy(info.expected_hash, r.content_hash.data(), sizeof(info.expected_hash));
        infos.push_back(info);
    }
    std::string failure, remove_failure;
    size_t failed_index = 0;
    engine_host::Status status = engine_.AddResources(infos.data(), infos.size(), &failed_index);
    bool added = status.Ok();
    if (!added)
    {
        failure = "cached resources refused: " + describe(api_, status);
    }
    else
    {
        engine_host::Resource handle;
        status = engine_.LoadResource(entry->level_id, nullptr, ENGINE_RESOURCE_LEVEL, &handle);
        if (!status.Ok())
        {
            failure = "cached level does not load: " + describe(api_, status);
        }
    }
    if (!failure.empty())
    {
        if (added)
        {
            for (const CachedResource& r : entry->resources)
            {
                const engine_host::Status removed =
                    engine_.RemoveResource(r.id, ENGINE_RESOURCE_FORCE);
                if (!removed.Ok() && remove_failure.empty())
                {
                    remove_failure = describe_name(api_, removed);
                }
            }
        }
        if (!remove_failure.empty())
        {
            std::fprintf(stderr, "[world] %s: could not remove a cached resource (%s)\n",
                         name.c_str(), remove_failure.c_str());
            failure += "; could not remove a cached resource (" + remove_failure + ")";
        }
        store_->remove(name, key);
        *note = failure;
        return false;
    }
    *out = asset_from(*entry);
    return true;
}

void WorldAssetLoader::store_entry_(const std::string& name, const std::string& key,
                                    const engine_import_result& r, const ImportedAsset& asset,
                                    AssetLoadReport* report)
{
    CachedImport entry;
    for (std::size_t i = 0; i < asset.ids.size(); ++i)
    {
        CachedResource c;
        c.id = asset.ids[i];
        c.type = asset.types[i];
        c.path = asset.paths[i];
        engine_host::Buffer blob;
        engine_host::Status status = engine_.ReadResource(c.id, kMaxBlobBytes, &blob);
        if (!status.Ok())
        {
            report->note = "cannot read back a resource: " + describe(api_, status);
            return;
        }
        auto info = engine_host::Init<engine_resource_info>();
        status = api_->resource_mounted(engine_.Get(), c.id, &info);
        if (!status.Ok())
        {
            report->note = "cannot read a resource's hash: " + describe(api_, status);
            return;
        }
        std::memcpy(c.content_hash.data(), info.content_hash, c.content_hash.size());
        c.blob.assign(reinterpret_cast<const std::uint8_t*>(blob.Data()),
                      reinterpret_cast<const std::uint8_t*>(blob.Data()) + blob.Size());
        entry.resources.push_back(std::move(c));
    }
    entry.level_id = asset.level_id;
    entry.manifest.assign(r.manifest.data, r.manifest.size);
    entry.warnings.assign(r.warnings.data, r.warnings.size);
    entry.summary = asset.summary;
    if (store_->store(name, key, entry))
    {
        report->stored = true;
    }
    else
    {
        report->note = "cannot write the cache entry";
    }
}

bool WorldAssetLoader::load(const std::string& name, const std::vector<std::uint8_t>& glb,
                            const engine_import_options& options, ImportedAsset* out,
                            AssetLoadReport* report)
{
    const auto t0 = std::chrono::steady_clock::now();
    *report = AssetLoadReport();
    report->name = name;
    const bool cache = store_ != nullptr && (caps_ & ENGINE_CAP_RESOURCE_CACHE) != 0;
    std::string key;
    if (cache)
    {
        key = make_cache_key(sha256(glb.data(), glb.size()), options_fingerprint(options),
                             engine_host::Engine::ImportFormatVersion(api_), stamp_);
        std::string note;
        if (try_cache_(name, key, out, &note))
        {
            report->source = AssetLoadReport::Source::Cache;
            report->milliseconds = ms_since(t0);
            std::fprintf(stderr, "[world] %s: from cache in %.1f ms\n", name.c_str(),
                         report->milliseconds);
            return true;
        }
        report->note = note;
    }

    engine_host::ImportResult result;
    if (!engine_.ImportGltf(glb.data(), glb.size(), options, &result).Ok())
    {
        return false;
    }
    const engine_import_result& r = result.Raw();
    ImportedAsset asset;
    for (std::uint32_t i = 0; i < r.resource_count; ++i)
    {
        std::uint64_t id;
        std::uint32_t type;
        std::memcpy(&id, r.ids.data + i * sizeof(id), sizeof(id));
        std::memcpy(&type, r.types.data + i * sizeof(type), sizeof(type));
        asset.ids.push_back(id);
        asset.types.push_back(type);
    }
    {
        const std::string paths(r.paths.data, r.paths.size);
        std::size_t pos = 0;
        for (std::uint32_t i = 0; i < r.resource_count; ++i)
        {
            std::size_t eol = paths.find('\n', pos);
            if (eol == std::string::npos)
            {
                eol = paths.size();
            }
            asset.paths.push_back(pos <= paths.size() ? paths.substr(pos, eol - pos)
                                                      : std::string());
            pos = eol + 1;
        }
    }
    asset.level_id = asset.ids.empty() ? 0 : asset.ids.back();
    asset.summary.assign(r.summary.data, r.summary.size);

    if (cache && !asset.ids.empty())
    {
        store_entry_(name, key, r, asset, report);
    }
    *out = std::move(asset);
    report->source = AssetLoadReport::Source::Imported;
    report->milliseconds = ms_since(t0);
    const std::string outcome =
        report->stored ? "stored"
                       : "not stored (" +
                             (report->note.empty() ? std::string("cache off") : report->note) + ")";
    std::fprintf(stderr, "[world] %s: imported in %.1f ms, %s\n", name.c_str(),
                 report->milliseconds, outcome.c_str());
    return true;
}

} // namespace tesseract::views::world
