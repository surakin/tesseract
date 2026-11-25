#include "views/world/WorldLoader.h"

#include "tk/i18n.h"
#include "views/WorldConsentDialog.h"
#include "views/world/world_room.h"

#include <fstream>
#include <sstream>

namespace tesseract::views::world
{

namespace fs = std::filesystem;

namespace
{

bool is_hash(const std::string& h)
{
    if (h.size() != 64)
    {
        return false;
    }
    for (char c : h)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

constexpr std::size_t kMaxStoredManifest = 256 * 1024;
// Longest reason shown to the user; the text may quote names the bundle chose.
constexpr std::size_t kMaxReasonChars = 200;

std::string needs_newer_text()
{
    return tk::tr("This world needs a newer Tesseract.");
}

} // namespace

fs::path bundle_dir_for(const fs::path& root, const std::string& hash)
{
    return is_hash(hash) ? root / hash : fs::path{};
}

std::size_t prune_world_bundles(const fs::path& root, std::chrono::hours max_age,
                                fs::file_time_type now)
{
    std::error_code ec;
    std::size_t removed = 0;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e;
        if (!it->is_directory(e) || e)
        {
            continue;
        }
        const auto t = fs::last_write_time(it->path(), e);
        if (!e && now - t > max_age)
        {
            fs::remove_all(it->path(), e);
            if (!e)
            {
                ++removed;
            }
        }
    }
    return removed;
}

struct WorldLoader::Result_
{
    enum class Kind
    {
        Meadow,
        Bundle,
        Fail,
        Dropped // the account went away: nothing to report
    } kind = Kind::Fail;
    std::string error;
    EngineScene::Options options;
    WorldConsentRequest request;
    bool ask = false;
};

WorldLoader::WorldLoader(Services services, Callbacks callbacks)
    : svc_(std::move(services)), cb_(std::move(callbacks))
{
}

void WorldLoader::request(const std::string& room_id)
{
    room_id_ = room_id;
    const std::uint64_t gen = ++gen_;
    loaded_ = false;
    pending_ = true;
    changed_pending_ = false;
    loaded_hash_.clear();
    if (cb_.hide_changed_banner)
    {
        cb_.hide_changed_banner();
    }
    auto result = std::make_shared<Result_>();
    // Copies: the work runs on another thread.
    const Ops ops = svc_.bind();
    const auto extract = svc_.extract;
    WorldTrustStore* const trust = svc_.trust;
    const fs::path root = svc_.bundle_root;
    svc_.run_async(
        [ops, extract, trust, root, room_id, result]
        {
            auto fail = [&](std::string msg)
            {
                result->kind = Result_::Kind::Fail;
                result->error = std::move(msg);
            };
            if (!ops.alive())
            {
                result->kind = Result_::Kind::Dropped;
                return;
            }
            const RoomWorldRead read = ops.room_world(room_id);
            if (!ops.alive())
            {
                result->kind = Result_::Kind::Dropped;
                return;
            }
            if (read.kind == RoomWorldRead::Kind::NoWorld)
            {
                result->kind = Result_::Kind::Meadow;
                return;
            }
            if (read.kind == RoomWorldRead::Kind::Error)
            {
                fail(read.format > kSupportedBundleFormat
                         ? needs_newer_text()
                         : tk::trf(tk::tr("Could not read the room's world: {0}"),
                                   {sanitize_world_text(read.error, kMaxReasonChars)}));
                return;
            }
            const RoomWorld& world = read.world;
            if (world.format > kSupportedBundleFormat)
            {
                fail(needs_newer_text());
                return;
            }
            const fs::path dir = bundle_dir_for(root, world.hash);
            if (dir.empty())
            {
                fail(tk::tr("The room's world is not valid."));
                return;
            }
            std::error_code ec;
            auto read_manifest = [&]() -> std::optional<std::string>
            {
                std::ifstream in(dir / "world.json", std::ios::binary);
                if (!in)
                {
                    return std::nullopt;
                }
                std::string text(kMaxStoredManifest + 1, '\0');
                in.read(text.data(), static_cast<std::streamsize>(text.size()));
                text.resize(static_cast<std::size_t>(in.gcount()));
                return text;
            };
            std::string manifest;
            if (auto m = read_manifest())
            {
                manifest = std::move(*m);
            }
            else
            {
                std::vector<std::uint8_t> bytes;
                const Result dl = ops.download(world, bytes);
                if (!ops.alive())
                {
                    result->kind = Result_::Kind::Dropped;
                    return;
                }
                if (!dl.ok)
                {
                    fail(tk::trf(tk::tr("Could not download the world: {0}"),
                                 {sanitize_world_text(dl.message, kMaxReasonChars)}));
                    return;
                }
                fs::create_directories(root, ec);
                // Straight into the final directory: extraction makes it atomically or not at
                // all, so two loads of one hash cannot leave a half-written one behind.
                const WorldBundleExtract ex = extract(bytes, dir.string());
                // Always the file on disk (bounded), never the JSON the extractor re-serialised:
                // a later load reads the file, and both must judge the same text. When extraction
                // failed, another load may have got there first.
                if (auto other = read_manifest())
                {
                    manifest = std::move(*other);
                }
                else
                {
                    fail(ex.ok ? tk::tr("The room's world is not valid.")
                               : tk::trf(tk::tr("The world is not valid: {0}"),
                                         {sanitize_world_text(ex.error, kMaxReasonChars)}));
                    return;
                }
            }
            // Mark the directory as used now, for the pruning at startup.
            fs::last_write_time(dir, fs::file_time_type::clock::now(), ec);

            std::string error;
            auto options = options_from_manifest(manifest, dir.string(), std::nullopt, &error);
            if (!options)
            {
                fail(error.find("newer") != std::string::npos
                         ? needs_newer_text()
                         : tk::trf(tk::tr("The world is not valid: {0}"),
                                   {sanitize_world_text(error, kMaxReasonChars)}));
                return;
            }
            const bool trusted = trust && trust->trusted(world.hash);
            const bool verified =
                !trusted && ops.sender_verified && ops.sender_verified(world.sender);
            if (!ops.alive())
            {
                result->kind = Result_::Kind::Dropped;
                return;
            }
            const WorldConsent consent = trust ? decide_consent(scripts_are_builtin(dir.string()),
                                                                world.hash, verified, *trust)
                                               : WorldConsent::Ask;
            result->kind = Result_::Kind::Bundle;
            result->options = std::move(*options);
            result->request = {world.name, world.sender, world.hash};
            result->ask = consent == WorldConsent::Ask;
        },
        [this, gen, result]
        {
            finish_(gen, result);
        });
}

void WorldLoader::loaded_now_(const std::string& hash, EngineScene::Options options)
{
    loaded_ = true;
    pending_ = false;
    loaded_hash_ = hash;
    cb_.load(std::move(options));
    if (changed_pending_)
    {
        changed_pending_ = false; // the world changed while this one was loading
        check_changed_();
    }
}

void WorldLoader::finish_(std::uint64_t gen, const std::shared_ptr<Result_>& r)
{
    if (gen != gen_)
    {
        return; // a newer request, or the end of the call, superseded this one
    }
    switch (r->kind)
    {
    case Result_::Kind::Dropped:
        pending_ = false;
        return;
    case Result_::Kind::Meadow:
        loaded_now_({}, svc_.meadow());
        return;
    case Result_::Kind::Fail:
        pending_ = false;
        changed_pending_ = false;
        cb_.fall_back_to_grid(r->error);
        return;
    case Result_::Kind::Bundle:
        break;
    }
    const std::string hash = r->request.hash;
    if (!r->ask || once_.count(hash))
    {
        loaded_now_(hash, std::move(r->options));
        return;
    }
    cb_.ask(r->request,
            [this, gen, r](ConsentChoice choice)
            {
                if (gen != gen_)
                {
                    return; // answered after a newer request or the end of the call
                }
                switch (choice)
                {
                case ConsentChoice::AlwaysRun:
                    if (svc_.trust)
                    {
                        svc_.trust->add(r->request.hash);
                    }
                    loaded_now_(r->request.hash, std::move(r->options));
                    return;
                case ConsentChoice::RunOnce:
                    once_.insert(r->request.hash);
                    loaded_now_(r->request.hash, std::move(r->options));
                    return;
                case ConsentChoice::DontRun:
                    pending_ = false;
                    changed_pending_ = false;
                    cb_.fall_back_to_grid(tk::tr("The world was not run."));
                    return;
                }
            });
}

void WorldLoader::on_changed(const std::string& room_id)
{
    if (room_id.empty() || room_id != room_id_)
    {
        return;
    }
    if (pending_)
    {
        changed_pending_ = true; // compared once the pending load has settled
        return;
    }
    if (!loaded_)
    {
        return; // the grid: the next request reads the world anew
    }
    check_changed_();
}

void WorldLoader::check_changed_()
{
    if (!loaded_ || room_id_.empty())
    {
        return;
    }
    const std::uint64_t gen = ++change_gen_;
    const std::string room_id = room_id_;
    auto latest = std::make_shared<std::optional<RoomWorldRead>>();
    const Ops ops = svc_.bind();
    svc_.run_async(
        [ops, room_id, latest]
        {
            if (ops.alive())
            {
                *latest = ops.room_world(room_id);
            }
        },
        [this, gen, latest]
        {
            if (gen != change_gen_ || !loaded_ || !*latest)
            {
                return;
            }
            const RoomWorldRead& r = **latest;
            std::string hash;
            WorldConsentRequest req;
            if (r.kind == RoomWorldRead::Kind::World)
            {
                hash = r.world.hash;
                req = {r.world.name, r.world.sender, r.world.hash};
            }
            else if (r.kind == RoomWorldRead::Kind::Error)
            {
                return; // no information (offline, a failed fetch): keep the world, no banner
            }
            if (hash == loaded_hash_)
            {
                return; // the same world again (the SDK reports a change more than once)
            }
            if (cb_.show_changed_banner)
            {
                cb_.show_changed_banner(std::move(req));
            }
        });
}

void WorldLoader::reload()
{
    if (!room_id_.empty())
    {
        request(room_id_);
    }
}

void WorldLoader::cancel()
{
    ++gen_;
    ++change_gen_;
    loaded_ = false;
    pending_ = false;
    changed_pending_ = false;
    loaded_hash_.clear();
    room_id_.clear();
    once_.clear();
}

} // namespace tesseract::views::world
