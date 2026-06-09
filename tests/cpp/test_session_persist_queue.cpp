// Focused tests for SessionPersistQueue's coalescing / last-write-wins logic.
//
// The queue's blocking writer is injected (Writer function pointer), so these
// tests exercise the ordering and coalescing semantics WITHOUT touching the
// real credential store or disk — no [keychain] tag, fully parallel-safe.

#include "session_persist_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using tesseract::SessionPersistQueue;

namespace
{
// A single recording sink shared by the injected writer. The queue's Writer is
// a plain function pointer (no captures), so the sink is process-global; the
// tests run serially within this TU but we guard it anyway and reset per-test.
struct Sink
{
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> calls; // (user, json) in order
    std::unordered_map<std::string, std::string> latest;    // user -> last json
    int total = 0;
    // Deterministic gate: when `gate_first` is set, the writer signals
    // `entered` as it begins the FIRST job and then blocks until `release` is
    // set. This lets a test reliably keep job #1 in-flight while it enqueues a
    // burst that must coalesce, with no reliance on sleeps/timing.
    bool gate_first = false;
    bool entered = false;
    bool release = false;
    int started = 0;

    void reset()
    {
        std::lock_guard<std::mutex> lk(m);
        calls.clear();
        latest.clear();
        total = 0;
        gate_first = false;
        entered = false;
        release = false;
        started = 0;
    }
};

Sink& sink()
{
    static Sink s;
    return s;
}

void recording_writer(const std::string& user, const std::string& json)
{
    std::unique_lock<std::mutex> lk(sink().m);
    const bool gate = sink().gate_first && sink().started == 0;
    ++sink().started;
    if (gate)
    {
        sink().entered = true;
        sink().cv.notify_all();
        sink().cv.wait(lk, [] { return sink().release; });
    }
    sink().calls.emplace_back(user, json);
    sink().latest[user] = json;
    ++sink().total;
    sink().cv.notify_all();
}
} // namespace

TEST_CASE("SessionPersistQueue writes a single enqueued job",
          "[session_persist_queue]")
{
    sink().reset();
    SessionPersistQueue q(&recording_writer);
    q.enqueue("@a:hs", "tok1");
    q.drain();

    std::lock_guard<std::mutex> lk(sink().m);
    REQUIRE(sink().calls.size() == 1);
    REQUIRE(sink().calls[0].first == "@a:hs");
    REQUIRE(sink().calls[0].second == "tok1");
}

TEST_CASE("SessionPersistQueue coalesces pending jobs for the same user "
          "(last-write-wins)",
          "[session_persist_queue]")
{
    sink().reset();
    {
        std::lock_guard<std::mutex> lk(sink().m);
        sink().gate_first = true; // pin job #1 in-flight until we release it
    }

    SessionPersistQueue q(&recording_writer);
    q.enqueue("@a:hs", "tok1"); // worker picks this up and blocks in-writer

    // Wait until the writer is actually inside job #1, so the burst below is
    // guaranteed to queue *behind* it and coalesce into a single pending job.
    {
        std::unique_lock<std::mutex> lk(sink().m);
        sink().cv.wait(lk, [] { return sink().entered; });
    }
    q.enqueue("@a:hs", "tok2");
    q.enqueue("@a:hs", "tok3");
    q.enqueue("@a:hs", "tok4"); // tok2/tok3 coalesced away; only tok4 survives

    // Release job #1 so the worker can drain the coalesced tail.
    {
        std::lock_guard<std::mutex> lk(sink().m);
        sink().release = true;
    }
    sink().cv.notify_all();
    q.drain();

    std::lock_guard<std::mutex> lk(sink().m);
    // Exactly two writes: the in-flight tok1, then the single coalesced tail.
    REQUIRE(sink().total == 2);
    REQUIRE(sink().calls.size() == 2);
    REQUIRE(sink().calls[0].second == "tok1");
    REQUIRE(sink().calls[1].second == "tok4");
    // The authoritative last value for the user is the newest token.
    REQUIRE(sink().latest["@a:hs"] == "tok4");
}

TEST_CASE("SessionPersistQueue keeps distinct users independent",
          "[session_persist_queue]")
{
    sink().reset();
    {
        std::lock_guard<std::mutex> lk(sink().m);
        sink().gate_first = true;
    }

    SessionPersistQueue q(&recording_writer);
    q.enqueue("@a:hs", "a1"); // in-flight (gated)
    {
        std::unique_lock<std::mutex> lk(sink().m);
        sink().cv.wait(lk, [] { return sink().entered; });
    }
    // While @a's first write is pinned, queue work for both users. @b coalesces
    // b1 -> b2; @a's a2 queues behind the in-flight a1.
    q.enqueue("@b:hs", "b1");
    q.enqueue("@b:hs", "b2");
    q.enqueue("@a:hs", "a2");
    {
        std::lock_guard<std::mutex> lk(sink().m);
        sink().release = true;
    }
    sink().cv.notify_all();
    q.drain();

    std::lock_guard<std::mutex> lk(sink().m);
    // Each user's newest token must be the persisted one.
    REQUIRE(sink().latest["@a:hs"] == "a2");
    REQUIRE(sink().latest["@b:hs"] == "b2");
}

TEST_CASE("SessionPersistQueue drains all pending work on destruction",
          "[session_persist_queue]")
{
    sink().reset();
    {
        SessionPersistQueue q(&recording_writer);
        q.enqueue("@a:hs", "x");
        q.enqueue("@b:hs", "y");
        // No explicit drain — the destructor must finish queued work.
    }
    std::lock_guard<std::mutex> lk(sink().m);
    REQUIRE(sink().total == 2);
}
