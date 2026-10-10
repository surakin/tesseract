// logout_all_accounts() (client/src/maintenance.cpp), the headless --logoutall
// path. Runs against the per-test isolated config/data dirs; tagged [keychain]
// because stored sessions go through the OS secret store.

#include <catch2/catch_test_macros.hpp>

#include "settings_guard.h"
#include "tesseract/maintenance.h"
#include "tesseract/paths.h"
#include "tesseract/secret_store.h"
#include "tesseract/session_store.h"
#include "tesseract/settings.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using tesseract::SessionStore;

namespace
{

const char* const kUid = "@maint-test:example.invalid";

struct MaintFixture
{
    tesseract::test::SettingsGuard settings_guard;
    MaintFixture() { clean(); }
    ~MaintFixture()
    {
        clean();
    }
    static void clean()
    {
        // The per-process data dir is shared by every test case: don't leave
        // (or inherit) an account index, least of all a corrupt one.
        std::error_code ec;
        for (const char* f : {"accounts.json", "accounts.json.bak",
                              "accounts.json.corrupt"})
        {
            fs::remove(tesseract::data_dir() / f, ec);
        }
        tesseract::SecretStore::remove(kUid);
        tesseract::SecretStore::remove_recovery_key(kUid);
        SessionStore::clear_account(kUid);
    }
};

} // namespace

TEST_CASE("logout_all_accounts with no accounts reports nothing and writes an "
          "empty index",
          "[maintenance][keychain]")
{
    MaintFixture f;
    auto report = tesseract::logout_all_accounts();
    CHECK_FALSE(report.index_corrupt);
    CHECK(report.accounts.empty());
    CHECK(SessionStore::load_index().user_ids.empty());
}

TEST_CASE("logout_all_accounts refuses to touch an unreadable index",
          "[maintenance][keychain]")
{
    MaintFixture f;
    const fs::path idx = tesseract::data_dir() / "accounts.json";
    fs::create_directories(idx.parent_path());
    {
        std::ofstream(idx) << "{ this is not json";
    }
    auto report = tesseract::logout_all_accounts();
    CHECK(report.index_corrupt);
    CHECK(report.accounts.empty());
    // Nothing was rewritten.
    std::ifstream in(idx);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "{ this is not json");
}

TEST_CASE("logout_all_accounts wipes an account that has no stored session",
          "[maintenance][keychain]")
{
    MaintFixture f;
    SessionStore::AccountIndex index;
    index.user_ids = {kUid};
    index.active_user_id = kUid;
    REQUIRE(SessionStore::save_index(index));

    auto report = tesseract::logout_all_accounts();
    REQUIRE(report.accounts.size() == 1);
    CHECK(report.accounts[0].user_id == kUid);
    CHECK_FALSE(report.accounts[0].server_ok);
    CHECK(report.accounts[0].error == "no stored session");
    CHECK(SessionStore::load_index().user_ids.empty());
}

TEST_CASE("logout_all_accounts wipes locally even when the session cannot be "
          "restored",
          "[maintenance][keychain]")
{
    MaintFixture f;
    SessionStore::AccountIndex index;
    index.user_ids = {kUid};
    REQUIRE(SessionStore::save_index(index));
    REQUIRE(SessionStore::save_account(kUid, "{\"not\":\"a session\"}"));

    auto report = tesseract::logout_all_accounts();
    REQUIRE(report.accounts.size() == 1);
    CHECK_FALSE(report.accounts[0].server_ok);
    CHECK_FALSE(report.accounts[0].error.empty());
    CHECK_FALSE(SessionStore::load_account(kUid).has_value());
    CHECK(SessionStore::load_index().user_ids.empty());
}

TEST_CASE("logout_all_accounts drops reminders unless the recovery key is unsaved",
          "[maintenance][keychain]")
{
    MaintFixture f;
    const std::string kept = "@maint-kept:example.invalid";
    auto& st = tesseract::Settings::instance();
    st.save_key_reminder_dismissals[kUid] = 2;
    st.save_key_reminder_snoozed_until[kUid] = 99;
    st.save_key_reminder_dismissals[kept] = 3;
    st.save_key_reminder_snoozed_until[kept] = 77;
    st.recovery_key_unsaved.insert(kept);
    st.save_to_disk(tesseract::config_dir());

    SessionStore::AccountIndex index;
    index.user_ids = {kUid, kept};
    REQUIRE(SessionStore::save_index(index));

    auto report = tesseract::logout_all_accounts();
    CHECK(report.accounts.size() == 2);

    // The function reloads Settings from disk and saves its edit back.
    auto& after = tesseract::Settings::instance();
    CHECK(after.save_key_reminder_dismissals.count(kUid) == 0);
    CHECK(after.save_key_reminder_snoozed_until.count(kUid) == 0);
    CHECK(after.save_key_reminder_dismissals.count(kept) == 1);
    CHECK(after.save_key_reminder_snoozed_until.count(kept) == 1);

    tesseract::SecretStore::remove(kept);
    tesseract::SecretStore::remove_recovery_key(kept);
    SessionStore::clear_account(kept);
}
