#include <catch2/catch_test_macros.hpp>

#include "app/SmokeDirector.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using tesseract::smoke::SmokeDirector;
using tesseract::smoke::SmokeHost;
using tesseract::smoke::SmokeResult;
using tesseract::smoke::Step;
using tesseract::smoke::smoke_result_json;

namespace
{

struct FakeSmokeHost : SmokeHost
{
    std::function<void()> pending;
    int max_pending = 0;
    int pending_count = 0;
    int finish_calls = 0;
    bool finish_ok = false;
    /// Fake time: advances by `ms` on every run_after, like a prompt event loop.
    std::chrono::milliseconds now{0};

    SmokeDirector::Clock clock()
    {
        return [this]
        {
            return now;
        };
    }

    void run_after(int ms, std::function<void()> fn) override
    {
        now += std::chrono::milliseconds(ms);
        ++pending_count;
        max_pending = std::max(max_pending, pending_count);
        pending = std::move(fn);
    }

    void finish(bool ok) override
    {
        ++finish_calls;
        finish_ok = ok;
    }

    /// Fires queued ticks until the director stops asking for more.
    void drain(int limit = 100000)
    {
        while (pending && limit-- > 0)
        {
            auto fn = std::move(pending);
            pending = nullptr;
            --pending_count;
            fn();
        }
    }
};

Step ready_after(std::string name, int polls, std::vector<std::string>& log, int timeout_ms = 15000)
{
    auto count = std::make_shared<int>(0);
    auto poll = [count, polls, &log, n = name]
    {
        if (++*count <= polls)
        {
            return false;
        }
        log.push_back(n);
        return true;
    };
    return Step{std::move(name), std::move(poll), timeout_ms, {}};
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("SmokeDirector: steps run in order and the run passes")
{
    FakeSmokeHost host;
    std::vector<std::string> log;
    SmokeDirector d(host,
                    {ready_after("discover", 0, log), ready_after("login", 3, log),
                     ready_after("send", 1, log)},
                    "", 100, 120000, host.clock());
    d.start();
    host.drain();

    CHECK(log == std::vector<std::string>{"discover", "login", "send"});
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
    CHECK(d.result().ok);
    CHECK(d.result().failed_step.empty());
    CHECK(d.result().step_ms.count("login") == 1);
    CHECK(d.result().step_ms.at("login") == 300);
    CHECK(host.max_pending <= 1);
}

TEST_CASE("SmokeDirector: a step timeout names the failed step and stops")
{
    FakeSmokeHost host;
    std::vector<std::string> log;
    Step never{"room",
               []
               {
                   return false;
               },
               500,
               {}};
    SmokeDirector d(host, {ready_after("login", 0, log), never, ready_after("send", 0, log)}, "",
                    100, 120000, host.clock());
    d.start();
    host.drain();

    CHECK(log == std::vector<std::string>{"login"});
    CHECK(host.finish_calls == 1);
    CHECK_FALSE(host.finish_ok);
    CHECK_FALSE(d.result().ok);
    CHECK(d.result().failed_step == "room");
    CHECK(d.result().error.find("500") != std::string::npos);
}

TEST_CASE("SmokeDirector: describe_failure supplies the error text")
{
    FakeSmokeHost host;
    Step s{"receive",
           []
           {
               return false;
           },
           100,
           []
           {
               return std::string("PING not in timeline");
           }};
    SmokeDirector d(host, {s}, "", 50, 120000, host.clock());
    d.start();
    host.drain();

    CHECK(d.result().failed_step == "receive");
    CHECK(d.result().error == "PING not in timeline");
}

TEST_CASE("SmokeDirector: the whole-run cap fails the current step")
{
    FakeSmokeHost host;
    Step slow{"login",
              []
              {
                  return false;
              },
              1000000,
              {}};
    SmokeDirector d(host, {slow}, "", 100, 1000, host.clock());
    d.start();
    host.drain();

    CHECK(host.finish_calls == 1);
    CHECK(d.result().failed_step == "login");
    CHECK(d.result().error == "whole-run timeout");
}

TEST_CASE("SmokeDirector: no steps passes immediately")
{
    FakeSmokeHost host;
    SmokeDirector d(host, {}, "");
    d.start();
    CHECK(host.finish_calls == 1);
    CHECK(host.finish_ok);
}

TEST_CASE("SmokeDirector: writes the result file")
{
    const auto path = std::filesystem::temp_directory_path() / "tesseract-smoke-director-test.json";
    std::filesystem::remove(path);

    FakeSmokeHost host;
    std::vector<std::string> log;
    Step never{"echo",
               []
               {
                   return false;
               },
               100,
               {}};
    SmokeDirector d(host, {ready_after("send", 0, log), never}, path.string(), 50, 120000,
                    host.clock());
    d.start();
    host.drain();

    const auto json = nlohmann::json::parse(read_file(path));
    CHECK(json.at("ok") == false);
    CHECK(json.at("failed_step") == "echo");
    CHECK(json.at("step_ms").at("send") == 0);
    std::filesystem::remove(path);
}

TEST_CASE("SmokeDirector: budgets follow the clock, not the poll count")
{
    FakeSmokeHost host;
    // A stalled UI thread: each poll finds 5 s passed although the poll
    // interval is 100 ms, so the 1 s whole-run cap trips after one poll.
    int polls = 0;
    Step slow{"login",
              [&polls, &host]
              {
                  ++polls;
                  host.now += std::chrono::milliseconds(5000);
                  return false;
              },
              1000000,
              {}};
    SmokeDirector d(host, {slow}, "", 100, 1000, host.clock());
    d.start();
    host.drain();

    CHECK(polls == 1);
    CHECK(d.result().error == "whole-run timeout");
}

TEST_CASE("SmokeDirector: step_ms reports real elapsed time")
{
    FakeSmokeHost host;
    int polls = 0;
    Step s{"login",
           [&polls, &host]
           {
               host.now += std::chrono::milliseconds(700);
               return ++polls == 2;
           },
           15000,
           {}};
    SmokeDirector d(host, {s}, "", 100, 120000, host.clock());
    d.start();
    host.drain();

    CHECK(d.result().ok);
    // two polls (700 ms of work each) around one 100 ms wait
    CHECK(d.result().step_ms.at("login") == 1500);
}

TEST_CASE("smoke_result_json: escapes quotes, backslashes and control bytes")
{
    SmokeResult r;
    r.ok = false;
    r.failed_step = "login";
    r.error = "bad \"x\"\\\n\x01";
    r.step_ms["send"] = 12;
    const auto j = nlohmann::json::parse(smoke_result_json(r));
    CHECK(j.at("ok") == false);
    CHECK(j.at("failed_step") == "login");
    CHECK(j.at("error").get<std::string>() == r.error);
    CHECK(j.at("step_ms").at("send") == 12);
}

TEST_CASE("smoke_result_json: invalid UTF-8 is replaced, not thrown")
{
    SmokeResult r;
    r.error = "bad \xff byte";
    std::string out;
    REQUIRE_NOTHROW(out = smoke_result_json(r));
    const auto j = nlohmann::json::parse(out);
    CHECK(j.at("error").get<std::string>() == "bad \xEF\xBF\xBD byte");
    CHECK(j.at("step_ms").is_object());
}
