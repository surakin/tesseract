// WorldAssetLoader: cache hit, miss and fallback paths against the real engine library.

#include <catch2/catch_test_macros.hpp>

#include "views/world/AssetCacheStore.h"
#include "views/world/EngineScene.h"
#include "views/world/WorldAssetLoader.h"

#include <tesseract/hash.h>

#include <engine/engine.h>
#include <engine/engine_abi_version.h>
#include <engine/engine_loader.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace tesseract::views::world;
namespace eh = engine_host;
namespace fs = std::filesystem;

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("tess_wal_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<std::uint8_t> read_bytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

int twa_count(const fs::path& dir, const std::string& prefix = "")
{
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        const std::string f = e.path().filename().string();
        if (e.path().extension() == ".twa" && f.rfind(prefix, 0) == 0)
        {
            ++n;
        }
    }
    return n;
}

fs::path first_twa(const fs::path& dir, const std::string& prefix)
{
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".twa" && e.path().filename().string().rfind(prefix, 0) == 0)
        {
            return e.path();
        }
    }
    return {};
}

// Owns the loaded library and one engine at a time (the library allows one engine per process).
struct EngineFixture
{
    engine_loader loader{};
    bool loaded = false;
    eh::Engine engine;
    std::uint32_t caps = 0;
    std::string skip_reason;
    std::string library = EngineScene::default_library_path();

    EngineFixture()
    {
        if (engine_load(library.c_str(), ENGINE_ABI_VERSION_MAJOR, ENGINE_ABI_VERSION_MINOR,
                        &loader) != ENGINE_OK)
        {
            skip_reason = std::string("no engine: ") + loader.message;
            return;
        }
        loaded = true;
        if (!create())
        {
            return;
        }
        if ((caps & ENGINE_CAP_RESOURCE_CACHE) == 0 || (caps & ENGINE_CAP_IMPORT_GLTF) == 0)
        {
            skip_reason = "the engine has no resource cache or importer";
        }
    }
    ~EngineFixture()
    {
        destroy();
        if (loaded)
        {
            loader.api->thread_detach();
            (void)engine_unload(&loader);
        }
    }
    bool create()
    {
        auto info = eh::Init<engine_create_info>();
        info.flags = ENGINE_CREATE_LOG_SUPPRESS_CONSOLE;
        engine_info about;
        eh::Status made = eh::Engine::Create(loader.api, info, &engine);
        if (made.Ok())
        {
            made = engine.Info(&about);
        }
        if (!made.Ok())
        {
            skip_reason = "no engine: cannot create one";
            return false;
        }
        caps = about.caps;
        return true;
    }
    void destroy()
    {
        engine = eh::Engine();
    }
    void recreate()
    {
        destroy();
        REQUIRE(create());
    }
};

engine_import_options robot_options(std::uint64_t seed = 20261117)
{
    auto o = eh::Init<engine_import_options>();
    o.flags = ENGINE_IMPORT_EMIT_LEVEL;
    o.id_seed = seed;
    o.path_prefix = eh::Str("/robot");
    return o;
}

std::size_t spawn_total(const eh::Engine& engine, std::uint64_t level)
{
    eh::World world;
    REQUIRE(engine.CreateWorld(&world).Ok());
    auto place = eh::Init<engine_spawn_params>();
    place.rotation[3] = 1.0f;
    place.scale = 1.0f;
    std::uint64_t root = 0;
    std::size_t total = 0;
    REQUIRE(world.SpawnLevel(level, place, &root, nullptr, 0, &total).Ok());
    return total;
}

#define REQUIRE_ENGINE(fx)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (!(fx).skip_reason.empty())                                                             \
        {                                                                                          \
            SKIP((fx).skip_reason);                                                                \
        }                                                                                          \
    } while (0)

engine_import_options field_options()
{
    auto o = robot_options(20261118);
    o.path_prefix = eh::Str("/field");
    return o;
}

const std::string kRobotPath = EngineScene::default_assets_dir() + "/robot.glb";
const std::string kFieldPath = EngineScene::default_assets_dir() + "/green_field.glb";

} // namespace

TEST_CASE("assetloader: import then cache hit gives the same asset", "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const AssetCacheStore store(dir.path);
    const auto glb = read_bytes(kRobotPath);
    REQUIRE(!glb.empty());

    ImportedAsset first, second;
    AssetLoadReport r1, r2;
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &first, &r1));
    }
    CHECK(r1.source == AssetLoadReport::Source::Imported);
    CHECK(r1.stored);
    CHECK(twa_count(dir.path) == 1);
    CHECK(!first.ids.empty());
    CHECK(first.level_id == first.ids.back());
    const std::size_t imported_total = spawn_total(fx.engine, first.level_id);
    CHECK(imported_total > 0);

    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &second, &r2));
    }
    CHECK(r2.source == AssetLoadReport::Source::Cache);
    CHECK(second.ids == first.ids);
    CHECK(second.types == first.types);
    CHECK(second.paths == first.paths);
    CHECK(second.level_id == first.level_id);
    CHECK(second.summary == first.summary);
    // The hit's level spawns the same entities as the import path's.
    CHECK(spawn_total(fx.engine, second.level_id) == imported_total);
}

TEST_CASE("assetloader: different bytes are a miss and the old entry is replaced",
          "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const AssetCacheStore store(dir.path);
    const auto robot = read_bytes(kRobotPath);
    const auto field = read_bytes(kFieldPath);

    ImportedAsset a;
    AssetLoadReport r;
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", robot, robot_options(), &a, &r));
    }
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", field, robot_options(), &a, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
    CHECK(twa_count(dir.path, "robot-") == 1);
}

TEST_CASE("assetloader: other id_seed or engine stamp is a miss", "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const AssetCacheStore store(dir.path);
    const auto glb = read_bytes(kRobotPath);
    ImportedAsset a;
    AssetLoadReport r;

    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    }
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(777), &a, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "other-stamp");
        REQUIRE(loader.load("robot", glb, robot_options(777), &a, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
}

TEST_CASE("assetloader: a corrupted entry falls back to import and is rewritten",
          "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const AssetCacheStore store(dir.path);
    const auto glb = read_bytes(kRobotPath);
    ImportedAsset a;
    AssetLoadReport r;

    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    }
    const fs::path file = first_twa(dir.path, "robot-");
    REQUIRE(!file.empty());
    {
        auto bytes = read_bytes(file.string());
        REQUIRE(bytes.size() > 16);
        bytes[bytes.size() / 2] ^= 0xFF;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
        CHECK(r.source == AssetLoadReport::Source::Imported);
        CHECK(r.stored);
    }
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
        CHECK(r.source == AssetLoadReport::Source::Cache);
    }
}

TEST_CASE("assetloader: a cache directory that cannot be created does not fail the load",
          "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const fs::path blocker = dir.path / "file";
    {
        std::ofstream(blocker) << "x";
    }
    const AssetCacheStore store(blocker / "sub");
    const auto glb = read_bytes(kRobotPath);
    ImportedAsset a;
    AssetLoadReport r;
    WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
    REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    CHECK(r.source == AssetLoadReport::Source::Imported);
    CHECK_FALSE(r.stored);
    CHECK_FALSE(r.note.empty());
    CHECK(!a.ids.empty());
}

TEST_CASE("assetloader: no store means always import and no files", "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const auto glb = read_bytes(kRobotPath);
    ImportedAsset a;
    AssetLoadReport r;
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, nullptr, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
    CHECK_FALSE(r.stored);
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, nullptr, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
    CHECK(twa_count(dir.path) == 0);
}

TEST_CASE("assetloader: an entry whose level cannot load falls back to import",
          "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    TempDir dir;
    const AssetCacheStore store(dir.path);
    const auto glb = read_bytes(kRobotPath);
    ImportedAsset a, b;
    AssetLoadReport r;

    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &a, &r));
    }
    // Drop a resource the level needs and re-store the entry under the same key.
    const std::string key = make_cache_key(tesseract::sha256(glb.data(), glb.size()),
                                           options_fingerprint(robot_options()),
                                           eh::Engine::ImportFormatVersion(fx.loader.api), "stamp");
    auto entry = store.load("robot", key);
    REQUIRE(entry.has_value());
    REQUIRE(entry->resources.size() >= 2);
    // The level references the mesh by id, so a level without it cannot load (an animation it
    // only finds by name would not stop it).
    const auto mesh = std::find_if(entry->resources.begin(), entry->resources.end(),
                                   [](const CachedResource& c)
                                   {
                                       return c.type == ENGINE_RESOURCE_MESH;
                                   });
    REQUIRE(mesh != entry->resources.end());
    entry->resources.erase(mesh);
    REQUIRE(store.store("robot", key, *entry));

    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &b, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Imported);
    CHECK(b.ids == a.ids);
    CHECK(r.stored);
    // The rewritten entry is whole again.
    fx.recreate();
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, &store, "stamp");
        REQUIRE(loader.load("robot", glb, robot_options(), &b, &r));
    }
    CHECK(r.source == AssetLoadReport::Source::Cache);
}

TEST_CASE("assetloader: a decimal-comma locale does not break the import", "[world][assetloader]")
{
    EngineFixture fx;
    REQUIRE_ENGINE(fx);
    // Qt and GTK take the numeric locale from the environment; the importer must not care.
    struct Numeric
    {
        std::string saved;
        bool set = false;
        explicit Numeric(const char* name)
        {
            const char* old = std::setlocale(LC_NUMERIC, nullptr);
            saved = old ? old : "C";
            for (const char* n : {name, "de_DE.UTF-8", "fr_FR.UTF-8"})
            {
                if (std::setlocale(LC_NUMERIC, n))
                {
                    set = true;
                    break;
                }
            }
        }
        ~Numeric()
        {
            std::setlocale(LC_NUMERIC, saved.c_str());
        }
    };

    const auto robot = read_bytes(kRobotPath);
    const auto field = read_bytes(kFieldPath);
    REQUIRE(!robot.empty());
    REQUIRE(!field.empty());

    ImportedAsset plain_robot, plain_field;
    AssetLoadReport report;
    {
        WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, nullptr, "stamp");
        REQUIRE(loader.load("robot", robot, robot_options(), &plain_robot, &report));
        REQUIRE(loader.load("field", field, field_options(), &plain_field, &report));
    }
    fx.recreate();

    Numeric comma("nb_NO.utf8");
    if (!comma.set)
    {
        SKIP("no decimal-comma locale installed");
    }
    char probe[16];
    std::snprintf(probe, sizeof probe, "%.1f", 0.5);
    REQUIRE(std::string(probe) == "0,5"); // the locale really is in effect

    ImportedAsset robot_in_comma, field_in_comma;
    WorldAssetLoader loader(fx.loader.api, fx.engine, fx.caps, nullptr, "stamp");
    REQUIRE(loader.load("robot", robot, robot_options(), &robot_in_comma, &report));
    REQUIRE(loader.load("field", field, field_options(), &field_in_comma, &report));
    CHECK(robot_in_comma.summary == plain_robot.summary);
    CHECK(field_in_comma.summary == plain_field.summary);
}
