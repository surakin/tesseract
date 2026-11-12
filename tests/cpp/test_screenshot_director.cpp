#include <catch2/catch_test_macros.hpp>

#include "app/ScreenshotDirector.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using tesseract::screenshot::Scene;
using tesseract::screenshot::ScreenshotDirector;
using tesseract::screenshot::ScreenshotHost;
using tesseract::screenshot::ScreenshotTheme;
using tesseract::screenshot::screenshot_filename;

namespace
{

struct FakeHost : ScreenshotHost
{
    std::vector<std::string>* log = nullptr;
    std::vector<std::string> saved;
    std::function<void()> pending;
    int max_pending = 0;
    int finish_calls = 0;
    bool finish_ok = false;
    std::string fail_on; // filename whose save returns false

    void apply_theme(ScreenshotTheme t) override
    {
        log->emplace_back(t == ScreenshotTheme::Light ? "theme:light" : "theme:dark");
    }
    void refresh() override
    {
        log->emplace_back("refresh");
    }
    bool save_png(const std::string& f) override
    {
        log->push_back("save:" + f);
        saved.push_back(f);
        return f != fail_on;
    }
    void run_after(int, std::function<void()> fn) override
    {
        if (pending)
        {
            max_pending = 2;
        }
        else if (max_pending == 0)
        {
            max_pending = 1;
        }
        pending = std::move(fn);
    }
    void finish(bool ok) override
    {
        ++finish_calls;
        finish_ok = ok;
        log->emplace_back(ok ? "finish:ok" : "finish:fail");
    }
    // Drain like an event loop: run pending callbacks one at a time.
    void drain()
    {
        while (pending)
        {
            auto fn = std::move(pending);
            pending = nullptr;
            fn();
        }
    }
};

// A host that writes real files, so the director's stable-frame check has
// something to compare. `content(name, call)` returns the bytes for the call-th
// save of that file name (0-based).
struct FileHost : FakeHost
{
    std::filesystem::path dir;
    std::function<std::string(const std::string&, int)> content;
    std::vector<int> calls_per_name_;
    std::vector<std::string> names_;

    std::filesystem::path output_dir() const override
    {
        return dir;
    }
    bool save_png(const std::string& f) override
    {
        FakeHost::save_png(f);
        int index = -1;
        for (std::size_t i = 0; i < names_.size(); ++i)
        {
            if (names_[i] == f)
            {
                index = static_cast<int>(i);
            }
        }
        if (index < 0)
        {
            names_.push_back(f);
            calls_per_name_.push_back(0);
            index = static_cast<int>(names_.size()) - 1;
        }
        std::ofstream(dir / f, std::ios::binary) << content(f, calls_per_name_[index]++);
        return true;
    }
};

struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        path = std::filesystem::temp_directory_path() / "tesseract-screenshot-director-test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string read_all(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<Scene> scenes(std::vector<std::string>& log, std::vector<std::string> names)
{
    std::vector<Scene> out;
    out.reserve(names.size());
    for (auto& n : names)
    {
        out.push_back({n,
                       [&log, n]
                       {
                           log.push_back("setup:" + n);
                       },
                       [&log, n]
                       {
                           log.push_back("teardown:" + n);
                       }});
    }
    return out;
}

} // namespace

TEST_CASE("screenshot_filename keeps legacy names for main", "[screenshot]")
{
    CHECK(screenshot_filename("qt6", "main", ScreenshotTheme::Light) == "qt6-light.png");
    CHECK(screenshot_filename("win", "main", ScreenshotTheme::Dark) == "win-dark.png");
    CHECK(screenshot_filename("gtk4", "room-info", ScreenshotTheme::Dark) ==
          "gtk4-room-info-dark.png");
}

TEST_CASE("director saves every scene in light then dark, in order", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, scenes(log, {"main", "thread", "room-info", "emoji", "settings"}),
                         "qt6");
    d.start();
    host.drain();

    CHECK(host.saved == std::vector<std::string>{
                            "qt6-light.png", "qt6-dark.png", "qt6-thread-light.png",
                            "qt6-thread-dark.png", "qt6-room-info-light.png",
                            "qt6-room-info-dark.png", "qt6-emoji-light.png", "qt6-emoji-dark.png",
                            "qt6-settings-light.png", "qt6-settings-dark.png"});
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
}

TEST_CASE("teardown precedes next setup and last teardown precedes finish", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, scenes(log, {"a", "b"}), "p");
    d.start();
    host.drain();

    CHECK(log == std::vector<std::string>{"setup:a", "theme:light", "refresh", "save:p-a-light.png",
                                          "theme:dark", "refresh", "save:p-a-dark.png",
                                          "teardown:a", "setup:b", "theme:light", "refresh",
                                          "save:p-b-light.png", "theme:dark", "refresh",
                                          "save:p-b-dark.png", "teardown:b", "finish:ok"});
}

TEST_CASE("failed save stops the run", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    host.fail_on = "p-b-light.png";
    ScreenshotDirector d(host, scenes(log, {"a", "b", "c"}), "p");
    d.start();
    host.drain();

    CHECK(host.saved.back() == "p-b-light.png");
    CHECK(host.saved.size() == 3);
    CHECK(host.finish_calls == 1);
    CHECK_FALSE(host.finish_ok);
}

TEST_CASE("never more than one pending run_after", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, scenes(log, {"a", "b", "c"}), "p");
    d.start();
    host.drain();
    CHECK(host.max_pending == 1);
}

TEST_CASE("empty scene list finishes ok without saving", "[screenshot]")
{
    std::vector<std::string> log;
    FakeHost host;
    host.log = &log;
    ScreenshotDirector d(host, {}, "p");
    d.start();
    host.drain();
    CHECK(host.saved.empty());
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
}

TEST_CASE("a frame that matches its recheck is saved once and the recheck removed", "[screenshot]")
{
    TempDir tmp;
    std::vector<std::string> log;
    FileHost host;
    host.log = &log;
    host.dir = tmp.path;
    host.content = [](const std::string&, int)
    {
        return std::string("frame");
    };
    ScreenshotDirector d(host, scenes(log, {"a"}), "p");
    d.start();
    host.drain();

    CHECK(host.saved == std::vector<std::string>{"p-a-light.png", "check-p-a-light.png",
                                                 "p-a-dark.png", "check-p-a-dark.png"});
    CHECK(host.finish_ok);
    CHECK(read_all(tmp.path / "p-a-light.png") == "frame");
    CHECK_FALSE(std::filesystem::exists(tmp.path / "check-p-a-light.png"));
    CHECK_FALSE(std::filesystem::exists(tmp.path / "check-p-a-dark.png"));
}

TEST_CASE("a frame that is still changing is rechecked until it settles", "[screenshot]")
{
    TempDir tmp;
    std::vector<std::string> log;
    FileHost host;
    host.log = &log;
    host.dir = tmp.path;
    // The first light frame is half-drawn; the recheck and the one after match.
    host.content = [](const std::string& name, int call)
    {
        if (name == "p-a-light.png")
        {
            return std::string("partial");
        }
        if (name == "check-p-a-light.png")
        {
            return std::string(call == 0 ? "settling" : "final");
        }
        return std::string("dark");
    };
    ScreenshotDirector d(host, scenes(log, {"a"}), "p");
    d.start();
    host.drain();

    // partial != settling, settling != final, then final matches the recheck.
    CHECK(host.finish_ok);
    CHECK(read_all(tmp.path / "p-a-light.png") == "final");
    CHECK_FALSE(std::filesystem::exists(tmp.path / "check-p-a-light.png"));
}

TEST_CASE("a frame that never settles fails the run", "[screenshot]")
{
    TempDir tmp;
    std::vector<std::string> log;
    FileHost host;
    host.log = &log;
    host.dir = tmp.path;
    int counter = 0;
    host.content = [&counter](const std::string&, int)
    {
        return std::to_string(counter++);
    };
    ScreenshotDirector d(host, scenes(log, {"a"}), "p");
    d.start();
    host.drain();

    CHECK(host.finish_calls == 1);
    CHECK_FALSE(host.finish_ok);
    CHECK_FALSE(std::filesystem::exists(tmp.path / "check-p-a-light.png"));
    // No dark capture happens after the light one gave up.
    for (const auto& saved : host.saved)
    {
        CHECK(saved.find("dark") == std::string::npos);
    }
}
