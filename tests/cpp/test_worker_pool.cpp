#include <catch2/catch_test_macros.hpp>

#include "app/WorkerPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("a task that throws neither stops the pool nor blocks wait_idle", "[worker_pool]")
{
    tesseract::WorkerPool pool(1);
    std::atomic<bool> ran{false};
    pool.post(
        []
        {
            throw std::runtime_error("boom");
        });
    pool.post(
        [&]
        {
            ran = true;
        });

    CHECK(pool.wait_idle(2000ms));
    CHECK(ran.load());
    CHECK(pool.idle());
}

TEST_CASE("wait_idle does not take the wake-up meant for a worker", "[worker_pool]")
{
    // One worker is held by a blocking task while another thread waits in
    // wait_idle(). The second worker is idle, so the new task must run on it
    // even though the first is still blocked.
    tesseract::WorkerPool pool(2);

    std::mutex m;
    std::condition_variable release_cv;
    bool release = false;
    pool.post(
        [&]
        {
            std::unique_lock<std::mutex> lk(m);
            release_cv.wait(lk,
                            [&]
                            {
                                return release;
                            });
        });

    std::thread waiter(
        [&]
        {
            (void)pool.wait_idle(5000ms);
        });
    std::this_thread::sleep_for(50ms);

    std::atomic<bool> ran{false};
    pool.post(
        [&]
        {
            ran = true;
        });

    const auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (!ran.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(ran.load());

    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    release_cv.notify_all();
    waiter.join();
    CHECK(pool.wait_idle(2000ms));
}
