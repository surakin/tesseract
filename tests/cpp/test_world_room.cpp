// The room's options from a world manifest (world.json), and the shipped meadow bundle.

#include <catch2/catch_test_macros.hpp>

#include "views/world/world_room.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

using namespace tesseract::views::world;
namespace fs = std::filesystem;

namespace
{

std::string manifest_with_script(const std::string& file, const std::string& attach)
{
    return R"({"format":1,"name":"T","field_asset":"f.glb","user_asset":"r.glb","scripts":[{"file":")" +
           file + R"(","attach":")" + attach +
           R"("}],"user_script":{"file":"robot.lua","attach":"robot"},"events_out":["send"]})";
}

double param_number(const ScriptSpec& s, const std::string& name)
{
    for (const auto& [k, v] : s.params)
    {
        if (k == name)
        {
            return std::get<double>(v);
        }
    }
    return -999.0;
}

} // namespace

TEST_CASE("meadow manifest reproduces the previous options", "[world][world room]")
{
    const EngineScene::Options o = room_options();
    CHECK(o.field_asset == "green_field.glb");
    CHECK(o.user_asset == "robot.glb");
    REQUIRE(o.scripts.size() == 3);
    CHECK(o.scripts[0].file == "player.lua");
    CHECK(o.scripts[0].behavior == "local_player");
    CHECK(o.scripts[0].on_camera);
    CHECK(o.scripts[0].params.empty());
    CHECK(o.scripts[1].file == "daylight.lua");
    CHECK(o.scripts[1].behavior == "daylight");
    CHECK_FALSE(o.scripts[1].on_camera);
    CHECK(param_number(o.scripts[1], "clock") == -1.0);
    CHECK(o.scripts[2].file == "world.lua");
    CHECK(o.scripts[2].behavior == "world");
    CHECK_FALSE(o.scripts[2].on_camera);
    CHECK(o.scripts[2].params.empty());
    CHECK(o.scripts[1].params.size() == 1);
    CHECK(o.user_script.params.empty());
    CHECK(o.user_script.file == "robot.lua");
    CHECK(o.user_script.behavior == "robot");
    CHECK_FALSE(o.user_script.on_camera);
    CHECK(o.events_out == std::vector<std::string>{"pose", "send"});
    CHECK(o.assets_dir == EngineScene::default_assets_dir());

    CHECK(param_number(room_options(720.0).scripts[1], "clock") == 720.0);
}

TEST_CASE("options_from_manifest takes any sane script attach name", "[world][world room]")
{
    std::string error;
    CHECK(options_from_manifest(manifest_with_script("a.lua", "go_kart"), "/b", std::nullopt,
                                &error));
    CHECK(options_from_manifest(manifest_with_script("a.lua", std::string(63, 'a')), "/b",
                                std::nullopt, &error));
    for (const std::string& bad : {std::string(), std::string(64, 'a'),
                                   std::string("world\\u0000x"), std::string("wor\\nld")})
    {
        error.clear();
        CHECK_FALSE(
            options_from_manifest(manifest_with_script("a.lua", bad), "/b", std::nullopt, &error));
        CHECK(error.find("attach") != std::string::npos);
    }
}

TEST_CASE("options_from_manifest rejects a path outside the bundle", "[world][world room]")
{
    std::string error;
    for (const char* bad :
         {"../x.lua", "/etc/x.lua", "a/../../x.lua", "a\\\\x.lua", "C:/x.lua", "", "CON.lua",
          "a/nul.txt", "x.lua.", "x.lua ", "a<b.lua", "a|b.lua", "a?b.lua", "a*b.lua", "a\\\"b.lua",
          "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q.lua"})
    {
        error.clear();
        CHECK_FALSE(
            options_from_manifest(manifest_with_script(bad, "world"), "/b", std::nullopt, &error));
        CHECK(error.find("inside the bundle") != std::string::npos);
    }
    error.clear();
    CHECK_FALSE(options_from_manifest(
        R"({"format":1,"name":"T","field_asset":"../f.glb","user_asset":"r.glb","user_script":{"file":"robot.lua","attach":"robot"}})",
        "/b", std::nullopt, &error));
    CHECK(error.find("inside the bundle") != std::string::npos);
    CHECK(options_from_manifest(manifest_with_script("sub/a.lua", "world"), "/b", std::nullopt,
                                &error));
}

TEST_CASE("options_from_manifest checks name, event names and size", "[world][world room]")
{
    std::string error;
    const std::string tail =
        R"("field_asset":"f","user_asset":"r","user_script":{"file":"a","attach":"robot"}})";
    CHECK_FALSE(options_from_manifest(R"({"format":1,)" + tail, "/b", std::nullopt, &error));
    CHECK_FALSE(
        options_from_manifest(R"({"format":1,"name":"",)" + tail, "/b", std::nullopt, &error));
    CHECK_FALSE(options_from_manifest(R"({"format":1,"name":"T","events_out":[""],)" + tail, "/b",
                                      std::nullopt, &error));
    CHECK_FALSE(options_from_manifest(R"({"format":1,"name":"T","events_out":["a\u0000b"],)" + tail,
                                      "/b", std::nullopt, &error));
    CHECK(options_from_manifest(R"({"format":1,"name":"T",)" + tail, "/b", std::nullopt, &error));
    CHECK_FALSE(
        options_from_manifest(R"({"format":1,"name":"T",)" + tail + std::string(256 * 1024, ' '),
                              "/b", std::nullopt, &error));
    CHECK(error.find("large") != std::string::npos);
}

TEST_CASE("options_from_manifest rejects a newer format and unknown shapes", "[world][world room]")
{
    std::string error;
    CHECK_FALSE(options_from_manifest(
        R"({"format":2,"name":"T","field_asset":"f","user_asset":"r","user_script":{"file":"a","attach":"robot"}})",
        "/b", std::nullopt, &error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK_FALSE(options_from_manifest("not json", "/b", std::nullopt, &error));
    CHECK_FALSE(options_from_manifest(
        R"({"format":1,"name":"T","field_asset":"f","user_asset":"r","user_script":{"file":"a","attach":"robot"},"extra":1})",
        "/b", std::nullopt, &error));
}

TEST_CASE("options_from_manifest applies the day clock by param name", "[world][world room]")
{
    const std::string json =
        R"({"format":1,"name":"T","field_asset":"f","user_asset":"r","scripts":[
            {"file":"a.lua","attach":"daylight","params":{"clock":-1,"tag":"x"}},
            {"file":"b.lua","attach":"world","params":{"other":5}}],
            "user_script":{"file":"robot.lua","attach":"robot"}})";
    std::string error;
    const auto o = options_from_manifest(json, "/b", 300.0, &error);
    REQUIRE(o);
    CHECK(param_number(o->scripts[0], "clock") == 300.0);
    CHECK(param_number(o->scripts[1], "other") == 5.0);
    CHECK(o->scripts[1].params.size() == 1);
    CHECK(o->assets_dir == "/b");
}

TEST_CASE("scripts_are_builtin is false for any other directory", "[world][world room]")
{
    CHECK(scripts_are_builtin(EngineScene::default_assets_dir()));
    CHECK(scripts_are_builtin(EngineScene::default_assets_dir() + "/."));
    CHECK_FALSE(scripts_are_builtin(fs::temp_directory_path().string()));
    CHECK_FALSE(scripts_are_builtin("/nonexistent/tess_world"));
}

TEST_CASE("bundle_files lists the manifest's files and nothing else", "[world][world room]")
{
    std::ifstream in(fs::path(EngineScene::default_assets_dir()) / "world.json");
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto files = bundle_files(json);
    CHECK(files == std::vector<std::string>{"world.json", "green_field.glb", "robot.glb",
                                            "player.lua", "daylight.lua", "world.lua",
                                            "robot.lua"});
    CHECK(bundle_files("garbage").empty());
}

TEST_CASE("options_from_manifest rejects bidi and invisible characters in a path and quotes safely",
          "[world][world room]")
{
    const std::string base =
        R"({"format":1,"name":"T","field_asset":"f.glb","user_asset":"r.glb","user_script":{"file":"robot.lua","attach":"robot"}})";
    for (const char* bad :
         {"\xE2\x80\xAE", "\xE2\x80\x8B", "\xEF\xBB\xBF", "\xE2\x81\xA0", "\xC2\xAD",
          "\xEF\xBE\xA0", "\xE3\x85\xA4", "\xE2\xA0\x80", "\xE1\xA0\x8E"})
    {
        std::string m = base;
        m.replace(m.find("f.glb"), 5, std::string("notes") + bad + "txt.glb");
        std::string err;
        CHECK_FALSE(options_from_manifest(m, "/tmp/x", std::nullopt, &err).has_value());
        CHECK(err.find(bad) == std::string::npos);
    }
    // A path over 255 bytes is refused, 255 is not.
    std::string m = base;
    m.replace(m.find("f.glb"), 5, std::string(252, 'a') + ".glb");
    CHECK_FALSE(options_from_manifest(m, "/tmp/x", std::nullopt, nullptr).has_value());
    m = base;
    m.replace(m.find("f.glb"), 5, std::string(251, 'a') + ".glb");
    CHECK(options_from_manifest(m, "/tmp/x", std::nullopt, nullptr).has_value());
}
