#include <catch2/catch_test_macros.hpp>
#include "tesseract/paths.h"
#include "tesseract/secret_store.h"
#include "tesseract/session_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test fixture: redirect SessionStore (config_dir + legacy SDK store) to a
// private temp directory so tests don't touch the real user config and clean
// up after themselves.
// ---------------------------------------------------------------------------
struct SessionFixture
{
    std::string dir;
#if defined(__APPLE__)
    std::string saved_home;
#endif

    // All user IDs written to the OS keychain by any test in this file.
    // Wiped in both constructor and destructor so concurrent test processes
    // that share the OS keychain do not interfere with each other.
    static void wipe_keychain()
    {
        for (const char* uid : {
                 "@alice:example.org",
                 "@bob:matrix.org",
                 "@carol:example.org",
                 "@dave:example.org",
                 "@erin:example.org",
                 "@stale:x",
             })
        {
            tesseract::SecretStore::remove(uid);
        }
    }

    SessionFixture()
    {
        wipe_keychain();

        // Use the OS PID so every ctest process (ctest -jN spawns one
        // process per test) gets a distinct temp directory even though the
        // in-process counter always starts at 0.
#if defined(_WIN32)
        const auto pid = static_cast<unsigned long>(_getpid());
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        dir = (fs::temp_directory_path() /
               ("tesseract_unit_tests_" + std::to_string(pid)))
                  .string();
        fs::create_directories(dir);
#if defined(_WIN32)
        _putenv_s("APPDATA", dir.c_str());
#elif defined(__APPLE__)
        // macOS config_dir() uses $HOME/Library/Application Support — redirect HOME.
        if (const char* h = std::getenv("HOME"))
        {
            saved_home = h;
        }
        setenv("HOME", dir.c_str(), 1);
#else
        // Account data lives under XDG_DATA_HOME (data_dir()), app config under
        // XDG_CONFIG_HOME (config_dir()). Keep them in *distinct* subdirs so the
        // data/config split is actually exercised. legacy_sdk_store_dir() also
        // consults XDG_DATA_HOME.
        const std::string cfg = (fs::path(dir) / "config").string();
        const std::string data = (fs::path(dir) / "data").string();
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        setenv("XDG_DATA_HOME", data.c_str(), 1);
#endif
    }

    ~SessionFixture()
    {
        wipe_keychain();

#if defined(_WIN32)
        _putenv_s("APPDATA", "");
#elif defined(__APPLE__)
        if (saved_home.empty())
        {
            unsetenv("HOME");
        }
        else
        {
            setenv("HOME", saved_home.c_str(), 1);
        }
#else
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
#endif
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Where the legacy single-account SDK store used to live. Mirrors the
    // platform branching in session_store.cpp's `legacy_sdk_store_dir()` so
    // the tests stay in lock-step with the production code path.
    fs::path legacy_store_dir() const
    {
#if defined(_WIN32)
        return fs::path(dir) / "tesseract" / "matrix-store";
#elif defined(__APPLE__)
        return fs::path(dir) / "Library" / "Application Support" / "tesseract" /
               "matrix-store";
#else
        // Mirrors legacy_sdk_store_dir(): $XDG_DATA_HOME/tesseract/matrix-store,
        // where XDG_DATA_HOME is the "data" subdir set in the constructor.
        return fs::path(dir) / "data" / "tesseract" / "matrix-store";
#endif
    }
};

// ---------------------------------------------------------------------------
// Legacy single-account API tests
// ---------------------------------------------------------------------------

TEST_CASE("path() ends with session.json", "[session_store]")
{
    SessionFixture f;
    auto p = tesseract::SessionStore::path();
    CHECK(p.ends_with("session.json"));
}

TEST_CASE("save + load round-trips content", "[session_store]")
{
    SessionFixture f;
    const std::string json = R"({"client_id":"test","token":"abc"})";

    REQUIRE(tesseract::SessionStore::save(json));
    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);
}

TEST_CASE("load returns nullopt when no file exists", "[session_store]")
{
    SessionFixture f;
    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("save creates parent directories on first write", "[session_store]")
{
    SessionFixture f;
    fs::remove_all(f.dir); // wipe everything under the temp base

    REQUIRE(tesseract::SessionStore::save(R"({"ok":true})"));
    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == R"({"ok":true})");
}

TEST_CASE("clear removes the session file", "[session_store]")
{
    SessionFixture f;
    REQUIRE(tesseract::SessionStore::save(R"({"x":1})"));
    REQUIRE(tesseract::SessionStore::load().has_value());

    tesseract::SessionStore::clear();
    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("load returns nullopt for an empty file", "[session_store]")
{
    SessionFixture f;
    auto p = tesseract::SessionStore::path();
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream{p}.close(); // create an empty file

    CHECK_FALSE(tesseract::SessionStore::load().has_value());
}

TEST_CASE("save overwrites existing content", "[session_store]")
{
    SessionFixture f;
    REQUIRE(tesseract::SessionStore::save(R"({"v":1})"));
    REQUIRE(tesseract::SessionStore::save(R"({"v":2})"));

    auto loaded = tesseract::SessionStore::load();
    REQUIRE(loaded.has_value());
    CHECK(*loaded == R"({"v":2})");
}

// ---------------------------------------------------------------------------
// Multi-account API tests
// ---------------------------------------------------------------------------

TEST_CASE("sanitize_user_id replaces awkward characters",
          "[session_store][accounts]")
{
    using S = tesseract::SessionStore;
    // '.' is replaced too so a malicious user_id can't smuggle ".." into
    // the account directory path.
    CHECK(S::sanitize_user_id("@alice:example.org") == "alice_example_org");
    CHECK(S::sanitize_user_id("@bob:matrix.org") == "bob_matrix_org");
    CHECK(S::sanitize_user_id("@../../etc:evil").find("..") ==
          std::string::npos);
    // Leading underscores from the @ are stripped so the dir name doesn't
    // start with one — purely cosmetic, but it keeps `ls` readable.
    CHECK_FALSE(S::sanitize_user_id("@x:y").empty());
    CHECK(S::sanitize_user_id("").empty());
}

TEST_CASE("save_account + load_account round-trip", "[session_store][accounts][keychain]")
{
    SessionFixture f;
    const std::string uid = "@alice:example.org";
    const std::string body = R"({"user_id":"@alice:example.org","token":"x"})";

    REQUIRE(tesseract::SessionStore::save_account(uid, body));
    auto loaded = tesseract::SessionStore::load_account(uid);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == body);

    // sdk_store_dir is a sibling of session.json, inside the account dir.
    auto session_path =
        tesseract::SessionStore::account_dir(uid) / "session.json";
    auto store_path = tesseract::SessionStore::sdk_store_dir(uid);
    CHECK(fs::exists(session_path));
    CHECK(store_path.parent_path() == session_path.parent_path());
    CHECK(store_path.filename() == "matrix-store");
}

TEST_CASE("clear_account removes the entire account directory",
          "[session_store][accounts][keychain]")
{
    SessionFixture f;
    const std::string uid = "@alice:example.org";

    REQUIRE(tesseract::SessionStore::save_account(uid, R"({"v":1})"));
    fs::create_directories(tesseract::SessionStore::sdk_store_dir(uid));
    std::ofstream{tesseract::SessionStore::sdk_store_dir(uid) / "db.sqlite"}
        << "x";

    tesseract::SessionStore::clear_account(uid);
    CHECK_FALSE(fs::exists(tesseract::SessionStore::account_dir(uid)));
}

TEST_CASE("save_index + load_index round-trip", "[session_store][accounts]")
{
    SessionFixture f;
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = "@bob:matrix.org";
    idx.user_ids = {"@alice:example.org", "@bob:matrix.org"};

    REQUIRE(tesseract::SessionStore::save_index(idx));
    auto loaded = tesseract::SessionStore::load_index();
    CHECK(loaded.active_user_id == idx.active_user_id);
    CHECK(loaded.user_ids == idx.user_ids);
}

TEST_CASE("load_index returns empty index when missing",
          "[session_store][accounts]")
{
    SessionFixture f;
    auto loaded = tesseract::SessionStore::load_index();
    CHECK(loaded.active_user_id.empty());
    CHECK(loaded.user_ids.empty());
    // A genuinely absent file is NOT corruption — distinguishing the two is
    // the whole point: an empty-because-missing index is fine, an
    // empty-because-truncated index would silently drop every account.
    CHECK_FALSE(loaded.corrupt);
}

TEST_CASE("load_index round-trips a valid index without flagging corruption",
          "[session_store][accounts]")
{
    SessionFixture f;
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = "@bob:matrix.org";
    idx.user_ids = {"@alice:example.org", "@bob:matrix.org"};
    REQUIRE(tesseract::SessionStore::save_index(idx));

    auto loaded = tesseract::SessionStore::load_index();
    CHECK_FALSE(loaded.corrupt);
    CHECK(loaded.active_user_id == "@bob:matrix.org");
    REQUIRE(loaded.user_ids.size() == 2);
    CHECK(loaded.user_ids[0] == "@alice:example.org");
    CHECK(loaded.user_ids[1] == "@bob:matrix.org");
}

TEST_CASE("load_index flags a corrupt accounts.json instead of silently "
          "returning an empty index",
          "[session_store][accounts]")
{
    SessionFixture f;

    // A truncated index: valid prefix, no closing bracket/brace. The old
    // hand-rolled scanner returned a perfectly empty AccountIndex here —
    // indistinguishable from "no accounts" — so every account would silently
    // be dropped and forced to re-login.
    const fs::path p = tesseract::data_dir() / "accounts.json";
    fs::create_directories(p.parent_path());
    { std::ofstream(p, std::ios::binary) << R"({"user_ids": ["@a:b.com")"; }

    auto loaded = tesseract::SessionStore::load_index();
    CHECK(loaded.corrupt);
}

TEST_CASE("save_index does not clobber a corrupt accounts.json on the same run",
          "[session_store][accounts]")
{
    SessionFixture f;

    // Plant a corrupt index on disk.
    const fs::path p = tesseract::data_dir() / "accounts.json";
    fs::create_directories(p.parent_path());
    const std::string corrupt_body = R"({"user_ids": ["@a:b.com")";
    { std::ofstream(p, std::ios::binary) << corrupt_body; }

    // The startup restore loop observes corruption...
    auto loaded = tesseract::SessionStore::load_index();
    REQUIRE(loaded.corrupt);

    // ...and a subsequent save (e.g. of the now-empty in-memory account set)
    // must NOT silently overwrite the recoverable corrupt file with the empty
    // set. The corrupt bytes must remain recoverable on disk.
    tesseract::SessionStore::AccountIndex empty;
    tesseract::SessionStore::save_index(empty);

    // The original corrupt file is preserved (moved aside, not destroyed):
    // either accounts.json still holds the corrupt bytes, or they were quarantined.
    bool recoverable = false;
    {
        std::ifstream in(p, std::ios::binary);
        if (in)
        {
            std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            if (body == corrupt_body)
                recoverable = true;
        }
    }
    const fs::path quarantine = tesseract::data_dir() / "accounts.json.corrupt";
    if (fs::exists(quarantine))
    {
        std::ifstream in(quarantine, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (body == corrupt_body)
            recoverable = true;
    }
    CHECK(recoverable);
}

// ---------------------------------------------------------------------------
// migrate_legacy_layout: every branch from the plan's state machine
// ---------------------------------------------------------------------------

TEST_CASE("migrate_legacy_layout is a no-op on a fresh install",
          "[session_store][migration][keychain]")
{
    SessionFixture f;
    CHECK(tesseract::SessionStore::migrate_legacy_layout());
    auto idx = tesseract::SessionStore::load_index();
    CHECK(idx.active_user_id.empty());
    CHECK(idx.user_ids.empty());
}

TEST_CASE("migrate_legacy_layout moves session.json and matrix-store into the "
          "account directory",
          "[session_store][migration][keychain]")
{
    SessionFixture f;

    // Plant a legacy session.json containing a user_id.
    const std::string legacy_body =
        R"({"client_id":"abc","user_id":"@alice:example.org","token":"x"})";
    REQUIRE(tesseract::SessionStore::save(legacy_body));

    // Plant a legacy SDK store with one file inside to confirm it survives
    // the move bit-for-bit.
    auto legacy_store = f.legacy_store_dir();
    fs::create_directories(legacy_store);
    {
        std::ofstream out(legacy_store / "matrix.sqlite", std::ios::binary);
        out << "sentinel-bytes";
    }

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    // Legacy paths are gone.
    CHECK_FALSE(fs::exists(tesseract::SessionStore::path()));
    CHECK_FALSE(fs::exists(legacy_store));

    // New paths exist with the expected content.
    const std::string uid = "@alice:example.org";
    auto migrated = tesseract::SessionStore::load_account(uid);
    REQUIRE(migrated.has_value());
    CHECK(*migrated == legacy_body);
    CHECK(fs::exists(tesseract::SessionStore::sdk_store_dir(uid) /
                     "matrix.sqlite"));

    // Index points at the migrated account.
    auto idx = tesseract::SessionStore::load_index();
    CHECK(idx.active_user_id == uid);
    REQUIRE(idx.user_ids.size() == 1);
    CHECK(idx.user_ids[0] == uid);
}

TEST_CASE("migrate_legacy_layout handles session-only legacy installs",
          "[session_store][migration][keychain]")
{
    SessionFixture f;

    const std::string legacy_body =
        R"({"client_id":"abc","user_id":"@bob:matrix.org","token":"y"})";
    REQUIRE(tesseract::SessionStore::save(legacy_body));
    // Deliberately do not create the legacy matrix-store/ directory.

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    CHECK_FALSE(fs::exists(tesseract::SessionStore::path()));
    CHECK_FALSE(fs::exists(f.legacy_store_dir()));

    const std::string uid = "@bob:matrix.org";
    auto migrated = tesseract::SessionStore::load_account(uid);
    REQUIRE(migrated.has_value());
    CHECK(*migrated == legacy_body);

    auto idx = tesseract::SessionStore::load_index();
    CHECK(idx.active_user_id == uid);
}

TEST_CASE("migrate_legacy_layout deletes corrupt legacy files",
          "[session_store][migration][keychain]")
{
    SessionFixture f;
    // No "user_id" key → unparseable for our purposes.
    REQUIRE(tesseract::SessionStore::save(R"({"definitely":"not-a-session"})"));
    auto legacy_store = f.legacy_store_dir();
    fs::create_directories(legacy_store);
    std::ofstream{legacy_store / "stale.dat"} << "x";

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    // Corrupt blob + its store are removed; no index is written so the next
    // launch ends up at the initial LoginView.
    CHECK_FALSE(fs::exists(tesseract::SessionStore::path()));
    CHECK_FALSE(fs::exists(legacy_store));
    CHECK(tesseract::SessionStore::load_index().active_user_id.empty());
}

TEST_CASE(
    "migrate_legacy_layout deletes an orphan store when no session is present",
    "[session_store][migration][keychain]")
{
    SessionFixture f;
    auto legacy_store = f.legacy_store_dir();
    fs::create_directories(legacy_store);
    std::ofstream{legacy_store / "stale.dat"} << "x";

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());
    CHECK_FALSE(fs::exists(legacy_store));
    CHECK(tesseract::SessionStore::load_index().active_user_id.empty());
}

TEST_CASE("migrate_legacy_layout is a no-op when accounts.json already exists",
          "[session_store][migration][keychain]")
{
    SessionFixture f;

    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = "@carol:example.org";
    idx.user_ids = {"@carol:example.org"};
    REQUIRE(tesseract::SessionStore::save_index(idx));

    // Plant a stale legacy session that should NOT be touched on a second
    // launch (cleanup of those is deferred to a follow-up pass).
    REQUIRE(tesseract::SessionStore::save(R"({"user_id":"@stale:x"})"));

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    auto loaded = tesseract::SessionStore::load_index();
    CHECK(loaded.active_user_id == "@carol:example.org");
    REQUIRE(loaded.user_ids.size() == 1);
    CHECK(loaded.user_ids[0] == "@carol:example.org");

    // The legacy session.json is still on disk (we don't silently delete it
    // on a normal startup).
    CHECK(fs::exists(tesseract::SessionStore::path()));
}

TEST_CASE("migrate_legacy_layout relocates a config-dir accounts tree into the "
          "data dir",
          "[session_store][migration][keychain]")
{
    SessionFixture f;
    if (tesseract::data_dir() == tesseract::config_dir())
    {
        SUCCEED("config and data dirs coincide on this platform; the "
                "config→data relocation is a no-op");
        return;
    }

    const std::string uid = "@dave:example.org";
    const std::string body = R"({"user_id":"@dave:example.org","token":"z"})";

    // Plant a multi-account layout under config_dir() — where builds that
    // predate the data/config split used to store it.
    const fs::path cfg_accounts = tesseract::config_dir() / "accounts";
    const fs::path cfg_acct =
        cfg_accounts / tesseract::SessionStore::sanitize_user_id(uid);
    fs::create_directories(cfg_acct / "matrix-store");
    { std::ofstream(cfg_acct / "session.json", std::ios::binary) << body; }
    {
        std::ofstream(cfg_acct / "matrix-store" / "matrix.sqlite",
                      std::ios::binary)
            << "sentinel-bytes";
    }
    {
        std::ofstream(tesseract::config_dir() / "accounts.json",
                      std::ios::binary)
            << R"({"active_user_id":"@dave:example.org",)"
               R"("user_ids":["@dave:example.org"]})";
    }

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    // The config-dir copies are gone.
    CHECK_FALSE(fs::exists(cfg_accounts));
    CHECK_FALSE(fs::exists(tesseract::config_dir() / "accounts.json"));

    // The data-dir layout is populated bit-for-bit.
    auto migrated = tesseract::SessionStore::load_account(uid);
    REQUIRE(migrated.has_value());
    CHECK(*migrated == body);
    CHECK(fs::exists(tesseract::SessionStore::sdk_store_dir(uid) /
                     "matrix.sqlite"));

    auto idx = tesseract::SessionStore::load_index();
    CHECK(idx.active_user_id == uid);
    REQUIRE(idx.user_ids.size() == 1);
    CHECK(idx.user_ids[0] == uid);
}

TEST_CASE("migrate_legacy_layout is a no-op when the data dir is already "
          "populated, leaving a stale config tree untouched",
          "[session_store][migration][keychain]")
{
    SessionFixture f;
    if (tesseract::data_dir() == tesseract::config_dir())
    {
        SUCCEED("config and data dirs coincide on this platform");
        return;
    }

    // Canonical data-dir index already present.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = "@erin:example.org";
    idx.user_ids = {"@erin:example.org"};
    REQUIRE(tesseract::SessionStore::save_index(idx));

    // A stale config-dir accounts.json from a half-finished older migration.
    fs::create_directories(tesseract::config_dir());
    {
        std::ofstream(tesseract::config_dir() / "accounts.json",
                      std::ios::binary)
            << R"({"active_user_id":"@stale:x","user_ids":["@stale:x"]})";
    }

    REQUIRE(tesseract::SessionStore::migrate_legacy_layout());

    // The data-dir index wins and is unchanged; the stale config copy is left
    // alone (step 0 short-circuits before the config→data relocation).
    auto loaded = tesseract::SessionStore::load_index();
    CHECK(loaded.active_user_id == "@erin:example.org");
    REQUIRE(loaded.user_ids.size() == 1);
    CHECK(loaded.user_ids[0] == "@erin:example.org");
    CHECK(fs::exists(tesseract::config_dir() / "accounts.json"));
}
