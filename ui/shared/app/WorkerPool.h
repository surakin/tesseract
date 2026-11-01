#pragma once

// Fixed-size FIFO thread pool used by ShellBase (pool_, mut_pool_,
// media_prefetch_pool_). Split out of ShellBase.h unchanged.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tesseract
{

struct WorkerPool
{
    explicit WorkerPool(int threads);
    ~WorkerPool();

    // Enqueue fn for execution on the next free thread.
    void post(std::function<void()> fn);

    // Stop accepting new work, drop pending tasks, and join all threads.
    // Safe to call multiple times (no-op after the first call).
    void drain();

    // Block up to `timeout` for every currently-queued-or-executing task
    // to finish, without stopping the pool or joining its threads (unlike
    // drain(), new work posted afterward — e.g. a re-login in the same
    // process — still runs normally). Returns true if the pool went idle
    // in time, false on timeout (a genuinely stuck task just means the
    // caller proceeds anyway, same bounded-wait philosophy as
    // AccountManager::wait_until_drained).
    bool wait_idle(std::chrono::milliseconds timeout)
    {
        // Waits on idle_cv_, not cv_: a notify_one on cv_ meant for a worker
        // must never be spent on a waiter here, or the queued task stalls.
        std::unique_lock<std::mutex> lk(mu_);
        return idle_cv_.wait_for(lk, timeout,
                                 [this]
                                 {
                                     return in_flight_.load(std::memory_order_relaxed) == 0;
                                 });
    }

    // True when nothing is queued or executing. Unlike pending_count() (queue
    // only), this stays false while a task is still running, so it is the
    // right test before freeing something a running task may still be using.
    bool idle() const
    {
        return in_flight_.load(std::memory_order_relaxed) == 0;
    }

    // Number of tasks waiting in the queue (not yet executing).
    // Lock-free read; acceptable to see a slightly stale count for display.
    std::size_t pending_count() const
    {
        return pending_.load(std::memory_order_relaxed);
    }

    std::deque<std::function<void()>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;      // workers wait for queued tasks
    std::condition_variable idle_cv_; // wait_idle() waits for in_flight_ == 0
    bool stop_ = false;
    std::vector<std::thread> threads_;
    // Tracks tasks waiting in queue_. Mutated under mu_; readable lock-free.
    std::atomic<std::size_t> pending_{0};
    // Tracks tasks that are queued OR currently executing — unlike
    // pending_, only reaches 0 once a task has actually finished running
    // (see the worker loop), which is what wait_idle() needs: a task can
    // hold a stray shared_ptr<AccountSession> for as long as it's
    // executing, well after it left the queue.
    std::atomic<std::size_t> in_flight_{0};
    // Posted outside mu_ whenever pending_ changes. Cleared in drain().
    std::function<void()> on_change_;
};

} // namespace tesseract
