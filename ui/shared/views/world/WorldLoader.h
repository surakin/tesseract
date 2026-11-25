#pragma once

// Gets the room's world to the engine: reads the room's `dev.tesseract.world` event, downloads
// and extracts its bundle into `<bundle_root>/<hash>/`, decides whether its scripts may run
// (built-in, trusted hash, verified sender, else ask), and hands the result to `load`. Anything
// that goes wrong, or a "Don't run", ends in `fall_back_to_grid`, never in the built-in meadow
// (users must not end up in mismatching worlds). A room with no world gets the meadow.
//
// Everything outside the UI is injected, so the loader is tested without a client or an engine.
// The blocking services (room_world, download, extract, sender_verified) run inside `run_async`'s
// `work`; callbacks and the dialog run on the UI thread inside its `done`. Nothing from a bundle
// runs before the consent decision: it is only parsed.

#include "views/WorldConsentDialog.h"
#include "views/world/EngineScene.h"

#include <tesseract/client.h>
#include <tesseract/world_trust.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tesseract::views::world
{

// The newest bundle format this build reads.
constexpr std::uint32_t kSupportedBundleFormat = 1;

// Bundle directories unused this long are removed at startup.
constexpr std::chrono::hours kBundleMaxAge{24 * 30};

// `<root>/<hash>`; empty unless `hash` is 64 lowercase hex characters (it names a directory).
std::filesystem::path bundle_dir_for(const std::filesystem::path& root, const std::string& hash);

// Removes the directories of `root` last used more than `max_age` before `now`. Returns how many.
std::size_t prune_world_bundles(
    const std::filesystem::path& root, std::chrono::hours max_age,
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now());

class WorldLoader
{
public:
    struct Callbacks
    {
        std::function<void(EngineScene::Options)> load;
        std::function<void(std::string)> fall_back_to_grid; // translated reason
        std::function<void(WorldConsentRequest, std::function<void(ConsentChoice)>)> ask;
        // "The world changed": the room's world is now `request` (name empty when removed).
        std::function<void(WorldConsentRequest)> show_changed_banner;
        std::function<void()> hide_changed_banner;
    };

    // The blocking services, bound to the account of the call. The worker never holds the account
    // between calls: each one locks it for its own duration, and `alive` says whether it is still
    // there (when not, the work is abandoned and the result dropped).
    struct Ops
    {
        std::function<bool()> alive;
        std::function<RoomWorldRead(const std::string& room_id)> room_world;
        std::function<Result(const RoomWorld&, std::vector<std::uint8_t>&)> download;
        std::function<bool(const std::string& user_id)> sender_verified;
    };

    struct Services
    {
        // UI thread, once per request/on_changed: the Ops of the call's account now.
        std::function<Ops()> bind;
        // Worker thread.
        std::function<WorldBundleExtract(const std::vector<std::uint8_t>&, const std::string& dest)>
            extract;
        // Runs `work` off the UI thread, then `done` on it.
        std::function<void(std::function<void()> work, std::function<void()> done)> run_async;
        // The shipped meadow.
        std::function<EngineScene::Options()> meadow;
        WorldTrustStore* trust = nullptr; // outlives the loader
        std::filesystem::path bundle_root;
    };

    WorldLoader(Services services, Callbacks callbacks);

    // Loads the world of `room_id` (UI thread). A newer request makes older answers stale.
    void request(const std::string& room_id);
    // The room's world event changed (the SDK calls this more than once per change): raises the
    // banner when the room's world differs from the loaded one; never loads.
    void on_changed(const std::string& room_id);
    // The banner's Reload: the same checks as request().
    void reload();
    // The call is over: pending work and an open question are void (their answers change nothing,
    // nothing is persisted), and "Run once" answers are forgotten.
    void cancel();

    bool loaded() const
    {
        return loaded_;
    }
    const std::string& loaded_hash() const
    {
        return loaded_hash_;
    }

private:
    struct Result_;
    void finish_(std::uint64_t gen, const std::shared_ptr<Result_>& r);
    void loaded_now_(const std::string& hash, EngineScene::Options options);
    void check_changed_();

    Services svc_;
    Callbacks cb_;
    std::string room_id_;
    std::uint64_t gen_ = 0;        // request() generation
    std::uint64_t change_gen_ = 0; // on_changed() generation
    bool loaded_ = false;
    bool pending_ = false;         // a request is in flight or its question is open
    bool changed_pending_ = false; // the world changed while pending: compare once settled
    std::set<std::string> once_;   // hashes answered "Run once" in this call
    std::string loaded_hash_;      // empty for the meadow
};

} // namespace tesseract::views::world
