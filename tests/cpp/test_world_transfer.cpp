// Importing a world into a room and exporting the room's world: the logic behind the button bar,
// with the room played by a fake (the pack, extract and hash are the SDK's real ones).

#include <catch2/catch_test_macros.hpp>

#include "views/world/EngineScene.h"
#include "views/world/world_room.h"
#include "views/world/world_transfer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tesseract;
using namespace tesseract::views::world;

namespace world_transfer_test
{

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<unsigned> n{0};
        path = fs::temp_directory_path() /
               ("tess-transfer-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(n++));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The shipped meadow's bundle files, copied into `dir`; `name` replaces the manifest's name.
void make_world_dir(const fs::path& dir, const std::string& name = "Meadow")
{
    const fs::path src = EngineScene::default_assets_dir();
    std::string manifest = read_file(src / "world.json");
    for (const std::string& f : bundle_files(manifest))
    {
        fs::create_directories((dir / f).parent_path());
        fs::copy_file(src / f, dir / f, fs::copy_options::overwrite_existing);
    }
    const std::string needle = "\"name\": \"Meadow\"";
    const auto at = manifest.find(needle);
    REQUIRE(at != std::string::npos);
    manifest.replace(at, needle.size(), "\"name\": \"" + name + "\"");
    write_file(dir / "world.json", manifest);
}

std::set<std::string> files_under(const fs::path& dir)
{
    std::set<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
    {
        if (e.is_regular_file())
        {
            out.insert(fs::relative(e.path(), dir).generic_string());
        }
    }
    return out;
}

// A room that keeps what was published; dedup by hash like set_room_world.
struct FakeRoom
{
    std::vector<std::uint8_t> bytes;
    std::string name;
    std::string hash;
    std::vector<std::string> hashes_set;         // each set_room_world call
    std::vector<std::vector<std::uint8_t>> sent; // the bytes of each call
    int uploads = 0;
    std::set<std::string> uploaded;

    TransferOps ops(const fs::path& meadow = EngineScene::default_assets_dir())
    {
        TransferOps o = client_transfer_ops_real();
        o.set_room_world =
            [this](const std::string&, const std::string& n, const std::vector<std::uint8_t>& b)
        {
            const std::string h = world_bundle_hash(b);
            hashes_set.push_back(h);
            sent.push_back(b);
            if (uploaded.insert(h).second)
            {
                ++uploads;
            }
            bytes = b;
            name = n;
            hash = h;
            return Result{true, {}};
        };
        o.room_world = [this](const std::string&)
        {
            RoomWorldRead r;
            if (!bytes.empty())
            {
                r.kind = RoomWorldRead::Kind::World;
                r.world.name = name;
                r.world.hash = hash;
                r.world.sender = "@a:example.org";
                r.world.size = bytes.size();
                r.world.format = 1;
            }
            return r;
        };
        o.download = [this](const RoomWorld&, std::vector<std::uint8_t>& out)
        {
            out = bytes;
            return Result{true, {}};
        };
        o.meadow_dir = [meadow]
        {
            return meadow.string();
        };
        return o;
    }

    static TransferOps client_transfer_ops_real()
    {
        TransferOps o;
        o.pack = [](const std::string& d)
        {
            return pack_world_bundle(d);
        };
        o.extract = [](const std::vector<std::uint8_t>& b, const std::string& d)
        {
            return extract_world_bundle(b, d);
        };
        return o;
    }
};

} // namespace world_transfer_test

using namespace world_transfer_test;

TEST_CASE("import of a directory uploads one bundle and sets the event", "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Green field");
    FakeRoom room;
    std::string err;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), &err));
    CHECK(err.empty());
    CHECK(room.uploads == 1);
    REQUIRE(room.hashes_set.size() == 1);
    CHECK(room.name == "Green field");
    CHECK(room.hash.size() == 64);
}

TEST_CASE("import of an invalid zip sets nothing", "[world][world transfer]")
{
    TempDir t;
    write_file(t.path / "bad.zip", "this is not a zip file at all");
    FakeRoom room;
    std::string err;
    CHECK_FALSE(import_world(room.ops(), "!r:x", (t.path / "bad.zip").string(), &err));
    CHECK_FALSE(err.empty());
    CHECK(room.hashes_set.empty());

    // A folder with no manifest is no world either.
    write_file(t.path / "empty" / "a.txt", "x");
    err.clear();
    CHECK_FALSE(import_world(room.ops(), "!r:x", (t.path / "empty").string(), &err));
    CHECK_FALSE(err.empty());
    CHECK(room.hashes_set.empty());

    // Neither is something that is not there, or not a folder or a zip.
    CHECK_FALSE(import_world(room.ops(), "!r:x", (t.path / "nothing").string(), &err));
    CHECK_FALSE(import_world(room.ops(), "!r:x", (t.path / "empty" / "a.txt").string(), &err));
    CHECK(room.hashes_set.empty());
}

TEST_CASE("import of the same directory twice hands set_room_world the same bundle both times",
          "[world][world transfer]")
{
    // The dedup itself is Client::set_room_world's (it keys on this hash); what import controls is
    // that an unchanged directory gives identical bytes, so the same hash.
    TempDir t;
    make_world_dir(t.path / "w");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    REQUIRE(room.sent.size() == 2);
    CHECK(room.sent[0] == room.sent[1]);
    CHECK(room.hashes_set[0] == room.hashes_set[1]);
}

TEST_CASE("import refuses a symbolic link", "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w");
    std::error_code ec;
    fs::create_directory_symlink(t.path / "w", t.path / "link", ec);
    if (ec)
    {
        SKIP("symbolic links are not available here");
    }
    FakeRoom room;
    std::string err;
    CHECK_FALSE(import_world(room.ops(), "!r:x", (t.path / "link").string(), &err));
    CHECK_FALSE(err.empty());
    CHECK(room.hashes_set.empty());
}

TEST_CASE("export writes the manifest and every file", "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Green field");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    fs::create_directories(t.path / "out");
    std::string err, out;
    REQUIRE(export_world(room.ops(), "!r:x", (t.path / "out").string(), &err, &out));
    CHECK(fs::path(out) == t.path / "out" / "Green field");
    CHECK(files_under(out) == files_under(t.path / "w"));
    CHECK(read_file(fs::path(out) / "world.json") == read_file(t.path / "w" / "world.json"));
}

TEST_CASE("export refuses to overwrite an existing directory", "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Green field");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    write_file(t.path / "out" / "Green field" / "mine.txt", "keep me");
    std::string err;
    CHECK_FALSE(export_world(room.ops(), "!r:x", (t.path / "out").string(), &err));
    CHECK_FALSE(err.empty());
    CHECK(read_file(t.path / "out" / "Green field" / "mine.txt") == "keep me");
    CHECK(files_under(t.path / "out" / "Green field").size() == 1);

    // An existing EMPTY directory of that name is refused too (the SDK would replace it).
    fs::create_directories(t.path / "empty_out" / "Green field");
    err.clear();
    CHECK_FALSE(export_world(room.ops(), "!r:x", (t.path / "empty_out").string(), &err));
    CHECK_FALSE(err.empty());
    fs::create_directories(t.path / "empty_out2" / "Meadow");
    FakeRoom none_empty;
    err.clear();
    CHECK_FALSE(export_world(none_empty.ops(), "!r:x", (t.path / "empty_out2").string(), &err));
    CHECK_FALSE(err.empty());
    CHECK(fs::is_empty(t.path / "empty_out2" / "Meadow"));

    // The meadow's directory is protected the same way.
    FakeRoom none;
    write_file(t.path / "out2" / "Meadow" / "mine.txt", "keep me");
    CHECK_FALSE(export_world(none.ops(), "!r:x", (t.path / "out2").string(), &err));
    CHECK(files_under(t.path / "out2" / "Meadow").size() == 1);
}

TEST_CASE("export of a room without a world copies exactly the meadow's manifest files (no .py)",
          "[world][world transfer]")
{
    TempDir t;
    const fs::path assets = EngineScene::default_assets_dir();
    const auto expected_list = bundle_files(read_file(assets / "world.json"));
    REQUIRE_FALSE(expected_list.empty());
    // The installed folder really holds things that are not part of the bundle.
    REQUIRE(fs::exists(assets / "generate_assets.py"));

    FakeRoom none;
    fs::create_directories(t.path / "out");
    std::string err, out;
    REQUIRE(export_world(none.ops(), "!r:x", (t.path / "out").string(), &err, &out));
    const std::set<std::string> expected(expected_list.begin(), expected_list.end());
    CHECK(files_under(out) == expected);
    CHECK(read_file(fs::path(out) / "world.json") == read_file(assets / "world.json"));
}

TEST_CASE("export then import round-trips to the same hash", "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Round trip");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    const std::string first = room.hash;
    fs::create_directories(t.path / "out");
    std::string out;
    REQUIRE(export_world(room.ops(), "!r:x", (t.path / "out").string(), nullptr, &out));
    REQUIRE(import_world(room.ops(), "!r:x", out, nullptr));
    CHECK(room.hash == first);
    CHECK(room.uploads == 1);
}

TEST_CASE("the exported directory name is safe whatever the world is called",
          "[world][world transfer]")
{
    CHECK(safe_world_dir_name("Green field") == "Green field");
    CHECK(safe_world_dir_name("../../etc/passwd") == "etcpasswd");
    CHECK(safe_world_dir_name("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij");
    CHECK(safe_world_dir_name(std::string("a\0b\x01\x1f", 5)) == "ab");
    CHECK(safe_world_dir_name("..") == "world");
    CHECK(safe_world_dir_name("") == "world");
    CHECK(safe_world_dir_name("   ") == "world");
    CHECK(safe_world_dir_name("name. . ") == "name");
    CHECK(safe_world_dir_name(".hidden") == "hidden");
    CHECK(safe_world_dir_name("CON") == "_CON");
    CHECK(safe_world_dir_name("nul.txt") == "_nul.txt");
    CHECK(safe_world_dir_name("com1") == "_com1");
    CHECK(safe_world_dir_name("Lpt9") == "_Lpt9");
    CHECK(safe_world_dir_name("COM10") == "COM10");
    CHECK(safe_world_dir_name("CONIN$") == "_CONIN$");
    CHECK(safe_world_dir_name("conout$") == "_conout$");
    CHECK(safe_world_dir_name("COM\xC2\xB9") == "_COM\xC2\xB9");
    CHECK(safe_world_dir_name("lpt\xC2\xB2.txt") == "_lpt\xC2\xB2.txt");
    CHECK(safe_world_dir_name("COM\xC2\xB4") == "COM\xC2\xB4"); // not a port
    // A lead byte without its continuation is dropped; what follows stands on its own.
    CHECK(safe_world_dir_name("a\xC0/b") == "ab");
    CHECK(safe_world_dir_name("a\xE2/b") == "ab");
    CHECK(safe_world_dir_name("a\xF0\x9F/b") == "ab");
    CHECK(safe_world_dir_name("a\xFF"
                              "\xFE"
                              "b") == "ab");
    // Direction overrides go (a name that reads backwards), accents stay.
    CHECK(safe_world_dir_name("a\xe2\x80\xae"
                              "b") == "ab");
    CHECK(safe_world_dir_name("Pr\xc3\xa9"
                              "cis") == "Pr\xc3\xa9"
                                        "cis");
    // Clamped without cutting a character in half.
    const std::string long_name(200, 'x');
    CHECK(safe_world_dir_name(long_name).size() == 64);
    std::string accents;
    for (int i = 0; i < 100; ++i)
    {
        accents += "\xc3\xa9";
    }
    const std::string clamped = safe_world_dir_name(accents);
    CHECK(clamped.size() <= 64);
    CHECK(clamped.size() % 2 == 0);
}

TEST_CASE("a world named with path separators exports into a safe directory",
          "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "..\\\\..\\\\evil/..");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    fs::create_directories(t.path / "out");
    std::string out;
    REQUIRE(export_world(room.ops(), "!r:x", (t.path / "out").string(), nullptr, &out));
    CHECK(fs::path(out).parent_path() == t.path / "out");
    CHECK(fs::exists(fs::path(out) / "world.json"));
}

TEST_CASE("export reports a room whose world cannot be read or downloaded",
          "[world][world transfer]")
{
    TempDir t;
    FakeRoom room;
    TransferOps ops = room.ops();
    ops.room_world = [](const std::string&)
    {
        RoomWorldRead r;
        r.kind = RoomWorldRead::Kind::Error;
        r.error = "boom";
        return r;
    };
    std::string err;
    CHECK_FALSE(export_world(ops, "!r:x", t.path.string(), &err));
    CHECK(err.find("boom") != std::string::npos);

    make_world_dir(t.path / "w");
    FakeRoom good;
    REQUIRE(import_world(good.ops(), "!r:x", (t.path / "w").string(), nullptr));
    TransferOps down = good.ops();
    down.download = [](const RoomWorld&, std::vector<std::uint8_t>&)
    {
        return Result{false, "offline"};
    };
    fs::create_directories(t.path / "out");
    CHECK_FALSE(export_world(down, "!r:x", (t.path / "out").string(), &err));
    CHECK(err.find("offline") != std::string::npos);
    CHECK(fs::is_empty(t.path / "out")); // nothing left behind
}

TEST_CASE("export of the meadow works when the loaded bundle directory was pruned",
          "[world][world transfer]")
{
    // The export reads the room's bundle from the server, never the loader's cache directory.
    TempDir t;
    make_world_dir(t.path / "w", "Pruned");
    FakeRoom room;
    REQUIRE(import_world(room.ops(), "!r:x", (t.path / "w").string(), nullptr));
    fs::remove_all(t.path / "w"); // the source (and any cache of it) is gone
    fs::create_directories(t.path / "out");
    std::string out;
    REQUIRE(export_world(room.ops(), "!r:x", (t.path / "out").string(), nullptr, &out));
    CHECK(fs::exists(fs::path(out) / "world.json"));
}

TEST_CASE("import and export turn a throwing client or a non-UTF-8 path into an error",
          "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w");
    FakeRoom room;
    std::string err;

    // A path the SDK cannot take as text: an error, not an exception.
    const std::string raw = (t.path / "w").string() + "\xff\xfe";
    CHECK_NOTHROW(import_world(room.ops(), "!r:x", raw, &err));
    CHECK_FALSE(import_world(room.ops(), "!r:x", raw, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(export_world(room.ops(), "!r:x", raw, &err));
    CHECK(room.hashes_set.empty());

    // A client call that throws (what the FFI does with bad text, or a failed allocation).
    TransferOps boom = room.ops();
    boom.pack = [](const std::string&) -> WorldBundlePack
    {
        throw std::invalid_argument("bad text");
    };
    err.clear();
    CHECK_FALSE(import_world(boom, "!r:x", (t.path / "w").string(), &err));
    CHECK(err.find("bad text") != std::string::npos);

    TransferOps boom2 = room.ops();
    boom2.room_world = [](const std::string&) -> RoomWorldRead
    {
        throw std::bad_alloc();
    };
    fs::create_directories(t.path / "out");
    err.clear();
    CHECK_FALSE(export_world(boom2, "!r:x", (t.path / "out").string(), &err));
    CHECK_FALSE(err.empty());
}

#ifndef _WIN32
TEST_CASE("import reports a missing temporary directory instead of throwing",
          "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w");
    FakeRoom room;
    const char* old = std::getenv("TMPDIR");
    const std::string saved = old ? old : "";
    setenv("TMPDIR", (t.path / "w" / "world.json").string().c_str(), 1); // a file, not a directory
    std::string err;
    bool ok = true;
    CHECK_NOTHROW(ok = import_world(room.ops(), "!r:x", (t.path / "w").string(), &err));
    if (old)
    {
        setenv("TMPDIR", saved.c_str(), 1);
    }
    else
    {
        unsetenv("TMPDIR");
    }
    CHECK_FALSE(ok);
    CHECK_FALSE(err.empty());
    CHECK(room.hashes_set.empty());
}
#endif

TEST_CASE("client_transfer_ops with a gone client says the call ended", "[world][world transfer]")
{
    const TransferOps ops = client_transfer_ops(
        [](const std::function<void(Client&)>&)
        {
            return false;
        });
    CHECK_FALSE(ops.set_room_world("!r:x", "n", {1}).ok);
    CHECK(ops.set_room_world("!r:x", "n", {1}).message == "The call has ended.");
    const RoomWorldRead read = ops.room_world("!r:x");
    CHECK(read.kind == RoomWorldRead::Kind::Error);
    CHECK(read.error == "The call has ended.");
    std::vector<std::uint8_t> out;
    CHECK(ops.download({}, out).message == "The call has ended.");
}

TEST_CASE("import trusts the published bundle's hash, and only on success",
          "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Green field");
    FakeRoom room;
    std::vector<std::string> trusted;
    TransferOps ops = room.ops();
    ops.trust_published = [&](const std::string& h)
    {
        trusted.push_back(h);
    };
    REQUIRE(import_world(ops, "!r:x", (t.path / "w").string(), nullptr));
    REQUIRE(trusted.size() == 1);
    CHECK(trusted[0] == room.hash);

    write_file(t.path / "bad.zip", "not a zip");
    CHECK_FALSE(import_world(ops, "!r:x", (t.path / "bad.zip").string(), nullptr));
    CHECK(trusted.size() == 1);
}

TEST_CASE("import refuses a bundle no client could load, before uploading",
          "[world][world transfer]")
{
    TempDir t;
    make_world_dir(t.path / "w", "Green field");
    // Valid for the SDK, but its script attaches under a name the C++ side refuses is hard to
    // make; an events_out name with a control character is refused by both. Use a manifest the
    // SDK accepts and the C++ check rejects through the extract fake instead.
    FakeRoom room;
    TransferOps ops = room.ops();
    const auto real = ops.extract;
    ops.extract = [real](const std::vector<std::uint8_t>& b, const std::string& d)
    {
        WorldBundleExtract r = real(b, d);
        r.manifest_json =
            R"({"format":1,"name":"T","field_asset":"../x.glb","user_asset":"r.glb"})";
        std::ofstream(fs::path(d) / "world.json") << r.manifest_json;
        return r;
    };
    std::string err;
    CHECK_FALSE(import_world(ops, "!r:x", (t.path / "w").string(), &err));
    CHECK(err.find("not inside the bundle") != std::string::npos);
    CHECK(room.hashes_set.empty());
}

TEST_CASE("safe_world_dir_name drops zero-width and filler characters", "[world][world transfer]")
{
    CHECK(safe_world_dir_name("a\xE2\x81\xA0"
                              "b\xEF\xBB\xBF"
                              "c\xC2\xAD"
                              "d\xE3\x85\xA4"
                              "e\xE2\xA0\x80"
                              "f\xEF\xBE\xA0"
                              "g\xE1\x85\x9F"
                              "h\xE1\xA0\x8E"
                              "i") == "abcdefghi");
}
