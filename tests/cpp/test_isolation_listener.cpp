// Keeps the test binary away from the user's real Tesseract config and data.
//
// Code under test persists settings with Settings::save_to_disk(config_dir()),
// accounts under data_dir() and media under cache_dir(); without this, a test
// that reaches one of those paths overwrites the developer's own
// app_settings.json / accounts / caches. Every test case starts with all three
// pointing into a per-process temporary directory. Re-applied before each test case because
// some fixtures point them elsewhere and then unset the variables, which
// would otherwise fall back to the real home directory.

#include <tesseract/settings.h>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

namespace fs = std::filesystem;

class TestIsolationListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
#if defined(_WIN32)
        const auto pid = _getpid();
#else
        const auto pid = ::getpid();
#endif
        root_ = fs::temp_directory_path() / ("tesseract-tests-" + std::to_string(pid));
        std::error_code ec;
        fs::create_directories(root_, ec);
        apply_();
    }

    void testCaseStarting(const Catch::TestCaseInfo&) override
    {
        // Start every test case with empty config/data/cache so on-disk state
        // (accounts index, caches) can't leak from one case into the next.
        std::error_code ec;
        for (const char* sub : {"config", "data", "cache", "appdata",
                                "localappdata", "home"})
            fs::remove_all(root_ / sub, ec);
        apply_();
    }

    void testRunEnded(const Catch::TestRunStats&) override
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

private:
    static void set_env_(const char* name, const fs::path& value)
    {
#if defined(_WIN32)
        _putenv_s(name, value.string().c_str());
#else
        ::setenv(name, value.c_str(), 1);
#endif
    }

    // The Settings singleton outlives test cases, so a test that sets a field
    // without a SettingsGuard would change the outcome of whichever test runs
    // next (order-dependent failures under --order rand). Loading an empty
    // settings file puts every field back to its default.
    void reset_settings_() const
    {
        const fs::path dir = root_ / "settings-defaults";
        std::error_code ec;
        fs::create_directories(dir, ec);
        {
            std::ofstream f(dir / "app_settings.json", std::ios::trunc);
            f << "{}";
        }
        tesseract::Settings::instance().load_from_disk(dir);
    }

    void apply_() const
    {
        reset_settings_();
#if defined(_WIN32)
        set_env_("APPDATA", root_ / "appdata");
        set_env_("LOCALAPPDATA", root_ / "localappdata");
#elif defined(__APPLE__)
        set_env_("HOME", root_ / "home");
#else
        set_env_("XDG_CONFIG_HOME", root_ / "config");
        set_env_("XDG_DATA_HOME", root_ / "data");
        set_env_("XDG_CACHE_HOME", root_ / "cache");
#endif
    }

    fs::path root_;
};

} // namespace

CATCH_REGISTER_LISTENER(TestIsolationListener)
