#include <catch2/catch_test_macros.hpp>
#include "tesseract/secret_store.h"
#include "tesseract/session_store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
static int setenv(const char* n, const char* v, int) { return _putenv_s(n, v); }
static int unsetenv(const char* n) { return _putenv_s(n, ""); }
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

struct TmpConfig
{
    fs::path dir;
    TmpConfig()
        : dir(fs::temp_directory_path() /
              ("tss_test_" + std::to_string(::getpid())))
    {
        fs::create_directories(dir);
        ::setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
        // Account data (account_dir / save_account / load_account) lives under
        // XDG_DATA_HOME now; point it at the temp dir too so these tests stay
        // isolated from the real ~/.local/share.
        ::setenv("XDG_DATA_HOME", dir.string().c_str(), 1);
    }
    ~TmpConfig()
    {
        ::unsetenv("XDG_CONFIG_HOME");
        ::unsetenv("XDG_DATA_HOME");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

TEST_CASE("SecretStore stub: save does not crash", "[session][secret][stub][keychain]")
{
    const std::string uid = "@stub_test:example.org";
    // Result depends on whether a secret service daemon is running.
    // Just confirm the call compiles and doesn't crash.
    bool ok = tesseract::SecretStore::save(uid, R"({"test":1})");
    if (ok)
        tesseract::SecretStore::remove(uid); // cleanup if it actually saved
    (void)ok;
}

TEST_CASE("SecretStore stub: load returns nullopt for unknown user",
          "[session][secret][stub][keychain]")
{
    auto result = tesseract::SecretStore::load("@nonexistent_xyz:example.org");
    CHECK(!result.has_value());
}

TEST_CASE("SecretStore stub: remove is a no-op", "[session][secret][stub][keychain]")
{
    tesseract::SecretStore::remove("@nobody:example.org");
}

TEST_CASE("SecretStore round-trip: save and load", "[session][secret][roundtrip][keychain]")
{
    const std::string uid  = "@roundtrip_test:example.org";
    const std::string json =
        R"({"user_id":"@roundtrip_test:example.org","access_token":"t0ken"})";

    bool saved = tesseract::SecretStore::save(uid, json);
    if (!saved)
        return; // skip: backend not available (no running secret service daemon)

    auto loaded = tesseract::SecretStore::load(uid);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);

    // Cleanup
    tesseract::SecretStore::remove(uid);

    auto after = tesseract::SecretStore::load(uid);
    CHECK(!after.has_value());
}

TEST_CASE("SessionStore: save_account and load_account round-trip",
          "[session][integration][keychain]")
{
    TmpConfig cfg;
    const std::string uid  = "@alice:example.org";
    const std::string json =
        R"({"user_id":"@alice:example.org","access_token":"tok_abc"})";

    REQUIRE(tesseract::SessionStore::save_account(uid, json));
    auto loaded = tesseract::SessionStore::load_account(uid);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);

    tesseract::SecretStore::remove(uid);
    tesseract::SessionStore::clear_account(uid);
}

TEST_CASE("SessionStore: load_account migrates legacy plaintext to SecretStore",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid  = "@bob:matrix.org";
    const std::string json =
        R"({"user_id":"@bob:matrix.org","access_token":"tok_xyz"})";

    // Pre-clear any stale SecretStore entry left by other tests using the
    // same UID, so we exercise the plaintext-fallback + migration path.
    tesseract::SecretStore::remove(uid);

    auto acct_dir = tesseract::SessionStore::account_dir(uid);
    fs::create_directories(acct_dir);
    {
        std::ofstream f(acct_dir / "session.json");
        f << json;
    }

    auto result = tesseract::SessionStore::load_account(uid);
    REQUIRE(result.has_value());
    CHECK(*result == json);

    auto sec = tesseract::SecretStore::load(uid);
    if (sec.has_value())
    {
        CHECK(*sec == json);
        std::ifstream fin(acct_dir / "session.json");
        std::string file_content((std::istreambuf_iterator<char>(fin)), {});
        CHECK(file_content == "{\"v\":2}");
    }

    tesseract::SecretStore::remove(uid);
    tesseract::SessionStore::clear_account(uid);
}

TEST_CASE("SessionStore: clear_account removes SecretStore entry",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid  = "@carol:server.net";
    const std::string json =
        R"({"user_id":"@carol:server.net","access_token":"tok_qrs"})";

    REQUIRE(tesseract::SessionStore::save_account(uid, json));
    tesseract::SessionStore::clear_account(uid);

    CHECK(!tesseract::SessionStore::load_account(uid).has_value());
    CHECK(!tesseract::SecretStore::load(uid).has_value());
}

TEST_CASE("SessionStore: sentinel file suppresses stale plaintext reads",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid = "@dave:homeserver.tld";

    auto acct_dir = tesseract::SessionStore::account_dir(uid);
    fs::create_directories(acct_dir);
    {
        std::ofstream f(acct_dir / "session.json");
        f << "{\"v\":2}";
    }

    auto result = tesseract::SessionStore::load_account(uid);
    CHECK(!result.has_value());
}
