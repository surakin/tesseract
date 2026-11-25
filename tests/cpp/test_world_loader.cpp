// The world loader's decisions with fakes for the client, the extraction and the dialog: no real
// Client, no engine. Files live under the isolation listener's temporary dirs.

#include <catch2/catch_test_macros.hpp>

#include "views/WorldConsentDialog.h"
#include "views/world/WorldLoader.h"

#include <tesseract/paths.h>

#include <filesystem>
#include <fstream>

using namespace tesseract;
using namespace tesseract::views;
using namespace tesseract::views::world;
namespace fs = std::filesystem;

namespace
{
const std::string kHashA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kHashB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

const char* kManifest =
    R"({"format":1,"name":"T","field_asset":"f.glb","user_asset":"r.glb","user_script":{"file":"robot.lua","attach":"robot"}})";

struct Fixture
{
    fs::path root = data_dir() / "loader_test" / "bundles";
    fs::path trust_file = data_dir() / "loader_test" / "trusted.json";
    WorldTrustStore trust{trust_file};
    std::optional<RoomWorld> world;
    std::string read_error;         // non-empty: the event cannot be read
    std::uint32_t error_format = 0; // what an unreadable event said its format was
    bool verified = false;
    bool download_ok = true;
    bool extract_ok = true;
    bool peer_wins = false; // a failing extract leaves a valid directory: another load made it
    bool account_alive = true;
    std::string disk_manifest = kManifest; // what extraction leaves in world.json
    std::string returned_manifest;         // what it reports; empty: the same as the file
    int calls_on_gone_account = 0;
    int downloads = 0, extracts = 0, loads = 0, asks = 0, grid = 0, banners = 0;
    std::string grid_reason, loaded_assets;
    std::function<void(ConsentChoice)> answer;
    WorldConsentRequest asked;
    WorldConsentRequest banner;
    bool deferred = false; // run_async queues work instead of running it at once
    std::vector<std::pair<std::function<void()>, std::function<void()>>> queue;
    std::unique_ptr<WorldLoader> loader;

    // Runs the queued work on "the worker", then (all) the completions on "the UI thread".
    void drain()
    {
        auto q = std::move(queue);
        queue.clear();
        for (auto& [work, done] : q)
        {
            work();
        }
        for (auto& [work, done] : q)
        {
            done();
        }
    }

    Fixture()
    {
        fs::remove_all(data_dir() / "loader_test");
        fs::create_directories(root);
        WorldLoader::Services s;
        s.bind = [this]
        {
            WorldLoader::Ops o;
            o.alive = [this]
            {
                return account_alive;
            };
            o.room_world = [this](const std::string&)
            {
                if (!account_alive)
                {
                    ++calls_on_gone_account;
                }
                RoomWorldRead r;
                if (!read_error.empty())
                {
                    r.kind = RoomWorldRead::Kind::Error;
                    r.error = read_error;
                    r.format = error_format;
                }
                else if (world)
                {
                    r.kind = RoomWorldRead::Kind::World;
                    r.world = *world;
                }
                return r;
            };
            o.download = [this](const RoomWorld&, std::vector<std::uint8_t>& out)
            {
                if (!account_alive)
                {
                    ++calls_on_gone_account;
                }
                ++downloads;
                out = {1, 2, 3};
                return download_ok ? Result{true, {}}
                                   : Result{false, "the bundle's hash does not match"};
            };
            o.sender_verified = [this](const std::string&)
            {
                if (!account_alive)
                {
                    ++calls_on_gone_account;
                }
                return verified;
            };
            return o;
        };
        s.extract = [this](const std::vector<std::uint8_t>&, const std::string& dest)
        {
            ++extracts;
            if (!extract_ok)
            {
                if (peer_wins)
                {
                    fs::create_directories(dest);
                    std::ofstream(fs::path(dest) / "world.json") << kManifest;
                }
                return WorldBundleExtract{false, "refused", {}};
            }
            fs::create_directories(dest);
            std::ofstream(fs::path(dest) / "world.json") << disk_manifest;
            return WorldBundleExtract{
                true, {}, returned_manifest.empty() ? disk_manifest : returned_manifest};
        };
        s.run_async = [this](std::function<void()> work, std::function<void()> done)
        {
            if (deferred)
            {
                queue.emplace_back(std::move(work), std::move(done));
                return;
            }
            work();
            done();
        };
        s.meadow = []
        {
            EngineScene::Options o;
            o.field_asset = "meadow-field";
            return o;
        };
        s.trust = &trust;
        s.bundle_root = root;
        WorldLoader::Callbacks c;
        c.load = [this](EngineScene::Options o)
        {
            ++loads;
            loaded_assets = o.field_asset;
        };
        c.fall_back_to_grid = [this](std::string why)
        {
            ++grid;
            grid_reason = std::move(why);
        };
        c.ask = [this](WorldConsentRequest r, std::function<void(ConsentChoice)> a)
        {
            ++asks;
            asked = std::move(r);
            answer = std::move(a);
        };
        c.show_changed_banner = [this](WorldConsentRequest r)
        {
            ++banners;
            banner = std::move(r);
        };
        loader = std::make_unique<WorldLoader>(std::move(s), std::move(c));
    }

    void publish(const std::string& hash, std::uint32_t format = 1)
    {
        world = RoomWorld{"Green field", hash, "mxc://s/x", "@alice:s", 3, format};
    }
};
} // namespace

TEST_CASE("world loader: no event loads the meadow without asking", "[world][world loader]")
{
    Fixture f;
    f.loader->request("!r:s");
    CHECK(f.loads == 1);
    CHECK(f.loaded_assets == "meadow-field");
    CHECK(f.asks == 0);
    CHECK(f.grid == 0);
    CHECK(f.downloads == 0);
}

TEST_CASE("world loader: trusted hash loads without asking", "[world][world loader]")
{
    Fixture f;
    f.trust.add(kHashA);
    f.publish(kHashA);
    f.loader->request("!r:s");
    CHECK(f.loads == 1);
    CHECK(f.loaded_assets == "f.glb");
    CHECK(f.asks == 0);
    CHECK(f.loader->loaded_hash() == kHashA);
}

TEST_CASE("world loader: verified sender loads without asking", "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    CHECK(f.loads == 1);
    CHECK(f.asks == 0);
    CHECK_FALSE(f.trust.trusted(kHashA));
}

TEST_CASE("world loader: unknown sender asks, RunOnce loads and does not remember",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.asks == 1);
    CHECK(f.loads == 0); // nothing runs before the answer
    CHECK(f.asked.sender == "@alice:s");
    CHECK(f.asked.name == "Green field");
    CHECK(f.asked.hash == kHashA);
    f.answer(ConsentChoice::RunOnce);
    CHECK(f.loads == 1);
    CHECK_FALSE(f.trust.trusted(kHashA));
    CHECK(f.grid == 0);
}

TEST_CASE("world loader: AlwaysRun remembers", "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.asks == 1);
    f.answer(ConsentChoice::AlwaysRun);
    CHECK(f.loads == 1);
    CHECK(f.trust.trusted(kHashA));
    f.loader->request("!r:s");
    CHECK(f.asks == 1); // not asked again
    CHECK(f.loads == 2);
}

TEST_CASE("world loader: DontRun falls back to grid", "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.asks == 1);
    f.answer(ConsentChoice::DontRun);
    CHECK(f.loads == 0);
    CHECK(f.grid == 1);
    CHECK_FALSE(f.trust.trusted(kHashA));
}

TEST_CASE("world loader: hash mismatch falls back to grid with an error", "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.download_ok = false;
    f.loader->request("!r:s");
    CHECK(f.loads == 0);
    CHECK(f.asks == 0);
    CHECK(f.grid == 1);
    CHECK(f.grid_reason.find("hash does not match") != std::string::npos);
    CHECK(f.extracts == 0);
}

TEST_CASE("world loader: newer format says needs a newer Tesseract and falls back",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA, 2);
    f.loader->request("!r:s");
    CHECK(f.loads == 0);
    CHECK(f.grid == 1);
    CHECK(f.grid_reason.find("newer Tesseract") != std::string::npos);
    CHECK(f.downloads == 0);
}

TEST_CASE("world loader: a bad hash in the event never names a directory", "[world][world loader]")
{
    Fixture f;
    f.publish("../../etc");
    f.loader->request("!r:s");
    CHECK(f.grid == 1);
    CHECK(f.downloads == 0);
}

TEST_CASE("world loader: an extracted bundle is reused", "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.loader->request("!r:s");
    CHECK(f.loads == 2);
    CHECK(f.downloads == 1);
    CHECK(f.extracts == 1);
    CHECK(fs::exists(f.root / kHashA / "world.json"));
}

TEST_CASE("world loader: world change raises the banner and keeps the current world running",
          "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.loads == 1);

    f.loader->on_changed("!r:s"); // the same world: the SDK reports a change more than once
    CHECK(f.banners == 0);

    f.publish(kHashB);
    f.loader->on_changed("!r:s");
    CHECK(f.banners == 1);
    CHECK(f.banner.hash == kHashB);
    CHECK(f.loads == 1); // nothing switched
    CHECK(f.grid == 0);
    CHECK(f.loader->loaded_hash() == kHashA);
}

TEST_CASE("world loader: a change with no world loaded only waits for the next request",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->on_changed("!r:s");
    CHECK(f.banners == 0);
}

TEST_CASE("world loader: reload from the banner runs the consent check again",
          "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.verified = false; // the new world is by someone we do not know
    f.publish(kHashB);
    f.loader->on_changed("!r:s");
    REQUIRE(f.banners == 1);
    f.loader->reload();
    CHECK(f.asks == 1);
    CHECK(f.asked.hash == kHashB);
    CHECK(f.loads == 1);
    f.answer(ConsentChoice::RunOnce);
    CHECK(f.loads == 2);
    CHECK(f.loader->loaded_hash() == kHashB);
}

TEST_CASE("world loader: a world removed during the call raises the banner",
          "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.world.reset();
    f.loader->on_changed("!r:s");
    CHECK(f.banners == 1);
    CHECK(f.banner.hash.empty());
}

TEST_CASE("world loader: an answer to an old request is ignored", "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.asks == 1);
    auto old_answer = f.answer;
    f.world.reset();
    f.loader->request("!r:s"); // meadow now
    CHECK(f.loads == 1);
    old_answer(ConsentChoice::AlwaysRun);
    CHECK(f.loads == 1);
    CHECK_FALSE(f.trust.trusted(kHashA));
}

TEST_CASE("world loader: sanitize_world_text strips control and bidi characters and clamps",
          "[world][world loader]")
{
    CHECK(sanitize_world_text("a\nb\tc\x01") == "abc");
    CHECK(sanitize_world_text("x\xE2\x80\xAE"
                              "evil") == "xevil");                    // U+202E
    CHECK(sanitize_world_text("x\xE2\x81\xA6y\xE2\x81\xA9") == "xy"); // U+2066, U+2069
    CHECK(sanitize_world_text("a\xE2\x80\x8B"
                              "b") == "ab"); // zero width space
    CHECK(sanitize_world_text("caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(sanitize_world_text("a\xFF"
                              "b") == "a\xEF\xBF\xBD"
                                      "b");
    const std::string clamped = sanitize_world_text(std::string(200, 'x'));
    CHECK(clamped == std::string(64, 'x') + "\xE2\x80\xA6");
    CHECK(sanitize_world_text(std::string(64, 'x')) == std::string(64, 'x'));
}

TEST_CASE("world loader: bundles unused for 30 days are pruned", "[world][world loader]")
{
    Fixture f;
    fs::create_directories(f.root / kHashA);
    fs::create_directories(f.root / kHashB);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(f.root / kHashA, now - std::chrono::hours(24 * 31));
    CHECK(prune_world_bundles(f.root, kBundleMaxAge, now) == 1);
    CHECK_FALSE(fs::exists(f.root / kHashA));
    CHECK(fs::exists(f.root / kHashB));
}

TEST_CASE("world loader: an event that cannot be read falls back to grid, never the meadow",
          "[world][world loader]")
{
    Fixture f;
    f.read_error = "could not read the room's state";
    f.loader->request("!r:s");
    CHECK(f.loads == 0);
    CHECK(f.grid == 1);
    CHECK(f.grid_reason.find("room's state") != std::string::npos);

    Fixture g; // a malformed event of a newer format
    g.read_error = "world event has no size";
    g.error_format = 2;
    g.loader->request("!r:s");
    CHECK(g.loads == 0);
    CHECK(g.grid == 1);
    CHECK(g.grid_reason.find("newer Tesseract") != std::string::npos);
}

TEST_CASE("world loader: the account going away while work is pending drops the result",
          "[world][world loader]")
{
    Fixture f;
    f.deferred = true;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.account_alive = false;
    f.drain();
    CHECK(f.calls_on_gone_account == 0);
    CHECK(f.downloads == 0);
    CHECK(f.loads == 0);
    CHECK(f.grid == 0);
    CHECK(f.asks == 0);
}

TEST_CASE("world loader: a stale worker result is dropped", "[world][world loader]")
{
    Fixture f;
    f.deferred = true;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.loader->request("!r:s");
    REQUIRE(f.queue.size() == 2);
    // Only the second request's completion may act: the first is stale.
    auto first = f.queue[0];
    auto second = f.queue[1];
    f.queue.clear();
    first.first();
    first.second();
    CHECK(f.loads == 0);
    second.first();
    second.second();
    CHECK(f.loads == 1);
}

TEST_CASE("world loader: after cancel an answer persists and loads nothing",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.asks == 1);
    f.loader->cancel();
    f.answer(ConsentChoice::AlwaysRun);
    CHECK_FALSE(f.trust.trusted(kHashA));
    CHECK(f.loads == 0);

    f.loader->request("!r:s");
    REQUIRE(f.asks == 2);
    f.loader->cancel();
    f.answer(ConsentChoice::RunOnce);
    CHECK(f.loads == 0);
    CHECK(f.grid == 0);
}

TEST_CASE("world loader: cancel drops pending work and forgets the loaded world",
          "[world][world loader]")
{
    Fixture f;
    f.deferred = true;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.loader->cancel();
    f.drain();
    CHECK(f.loads == 0);
    CHECK_FALSE(f.loader->loaded());
    f.loader->on_changed("!r:s"); // no call: nothing to compare with
    f.drain();
    CHECK(f.banners == 0);
}

TEST_CASE("world loader: a change while the load is pending shows the banner once it settles",
          "[world][world loader]")
{
    Fixture f;
    f.deferred = true;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.publish(kHashB); // changed in the room meanwhile
    f.loader->on_changed("!r:s");
    f.drain(); // the load (still reads the new world: A is replaced before the worker ran)
    f.drain(); // the comparison
    // The worker read B, so the loaded world is B and there is nothing to warn about.
    CHECK(f.loader->loaded_hash() == kHashB);
    CHECK(f.banners == 0);

    Fixture g;
    g.verified = false;
    g.publish(kHashA);
    g.loader->request("!r:s");
    REQUIRE(g.asks == 1);
    g.deferred = true;
    g.publish(kHashB);
    g.loader->on_changed("!r:s"); // while the question is open
    g.drain();
    CHECK(g.banners == 0);
    g.answer(ConsentChoice::RunOnce);
    g.drain();
    CHECK(g.loads == 1);
    CHECK(g.banners == 1);
    CHECK(g.banner.hash == kHashB);
}

TEST_CASE("world loader: Run once is remembered for the call, not forever", "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.loader->request("!r:s");
    f.answer(ConsentChoice::RunOnce);
    f.loader->request("!r:s"); // Grid -> World again
    CHECK(f.asks == 1);
    CHECK(f.loads == 2);
    f.loader->cancel(); // the call ends
    f.loader->request("!r:s");
    CHECK(f.asks == 2);
}

TEST_CASE("world loader: two loads of one hash share the extraction", "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.extract_ok = false;
    f.peer_wins = true; // extract refuses, but the other load's directory is there and valid
    f.loader->request("!r:s");
    CHECK(f.extracts == 1);
    CHECK(f.loads == 1);
    CHECK(f.grid == 0);

    Fixture g; // an extraction that fails with nothing there is a failure
    g.verified = true;
    g.extract_ok = false;
    g.publish(kHashA);
    g.loader->request("!r:s");
    CHECK(g.extracts == 1);
    CHECK(g.grid == 1);
    CHECK(g.loads == 0);
}

TEST_CASE("world loader: an unreadable event after loading is no information: no banner",
          "[world][world loader]")
{
    Fixture f;
    f.verified = true;
    f.publish(kHashA);
    f.loader->request("!r:s");
    REQUIRE(f.loads == 1);
    f.read_error = "could not read the room's state"; // offline
    f.loader->on_changed("!r:s");
    CHECK(f.banners == 0);
    CHECK(f.loader->loaded_hash() == kHashA);
}

TEST_CASE("world loader: sanitiser drops invisible characters and keeps long ids",
          "[world][world loader]")
{
    CHECK(sanitize_world_text("a\xC2\xAD"
                              "b") == "ab"); // U+00AD
    CHECK(sanitize_world_text("a\xCD\x8F"
                              "b") == "ab"); // U+034F
    CHECK(sanitize_world_text("a\xE3\x85\xA4"
                              "b") == "ab"); // U+3164
    CHECK(sanitize_world_text("a\xEF\xBF\xB9"
                              "b") == "ab"); // U+FFF9
    CHECK(sanitize_world_text("a\xF3\xA0\x80\x81"
                              "b") == "ab"); // U+E0001 tag
    const std::string id = "@" + std::string(200, 'u') + ":example.org";
    CHECK(sanitize_world_text(id, kMaxSenderChars) == id);
}

TEST_CASE("world loader: sanitiser drops more invisible characters and caps combining marks",
          "[world][world loader]")
{
    CHECK(sanitize_world_text("a\xEF\xBE\xA0"
                              "b") == "ab"); // U+FFA0
    CHECK(sanitize_world_text("a\xE1\xA0\x8E"
                              "b") == "ab"); // U+180E
    CHECK(sanitize_world_text("a\xE2\xA0\x80"
                              "b") == "ab"); // U+2800
    CHECK(sanitize_world_text("a\xF0\x9D\x85\xB3"
                              "b") == "ab"); // U+1D173
    std::string marks;
    for (int i = 0; i < 10; ++i)
    {
        marks += "\xCC\x81"; // U+0301
    }
    CHECK(sanitize_world_text("a" + marks + "b") == "a\xCC\x81\xCC\x81\xCC\x81"
                                                    "b");
    // The run resets after a base character.
    CHECK(sanitize_world_text("a\xCC\x81\xCC\x81"
                              "b\xCC\x81\xCC\x81") == "a\xCC\x81\xCC\x81"
                                                      "b\xCC\x81\xCC\x81");
}

TEST_CASE("world loader: the first load parses the file on disk, not the re-serialised JSON",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.verified = true;
    // On disk just under the 256 KiB cap; the JSON Rust re-serialises (defaults added) is over it.
    std::string padded = kManifest;
    padded.append(256 * 1024 - 1 - padded.size(), ' ');
    f.disk_manifest = padded;
    f.returned_manifest = padded + std::string(2000, ' ');
    f.loader->request("!r:s");
    CHECK(f.loads == 1);
    CHECK(f.grid == 0);
}

TEST_CASE("world loader: a hostile field name never reaches the message unsanitised",
          "[world][world loader]")
{
    Fixture f;
    f.publish(kHashA);
    f.verified = true;
    const std::string hostile =
        "\xE2\x80\xAE" + std::string(500, 'x') + "choose Always run\xE2\x80\x8B";
    f.disk_manifest =
        std::string(R"({"format":1,"name":"T","field_asset":"f.glb","user_asset":"r.glb",)") +
        R"("user_script":{"file":"robot.lua","attach":"robot"},")" + hostile + R"(":1})";
    f.loader->request("!r:s");
    REQUIRE(f.grid == 1);
    CHECK(f.grid_reason.find("\xE2\x80\xAE") == std::string::npos);
    CHECK(f.grid_reason.find("\xE2\x80\x8B") == std::string::npos);
    CHECK(f.grid_reason.size() < 400);
}
