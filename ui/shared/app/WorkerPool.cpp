#include "app/WorkerPool.h"

#include <cassert>
#include <cstdio>
#include <exception>

namespace tesseract
{

WorkerPool::WorkerPool(int threads)
{
    for (int i = 0; i < threads; ++i)
    {
        threads_.emplace_back(
            [this]
            {
                for (;;)
                {
                    std::function<void()> task;
                    std::function<void()> notify;
                    {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait(lk,
                                 [this]
                                 {
                                     return stop_ || !queue_.empty();
                                 });
                        if (stop_ && queue_.empty())
                        {
                            return;
                        }
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        pending_.fetch_sub(1, std::memory_order_relaxed);
                        notify = on_change_;
                    }
                    if (notify)
                    {
                        notify();
                    }
                    // A throwing task must not take the process down, and must
                    // not skip the in_flight_ decrement below.
                    try
                    {
                        task();
                    }
                    catch (const std::exception& e)
                    {
                        std::fprintf(stderr, "[worker] task threw: %s\n", e.what());
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[worker] task threw a non-standard exception\n");
                    }
                    // task() may have captured a shared_ptr<AccountSession>
                    // (or similar) that only releases when it returns here —
                    // wait_idle() waits on in_flight_, not pending_, exactly
                    // so it observes that release rather than just "left the
                    // queue".
                    if (in_flight_.fetch_sub(1, std::memory_order_relaxed) == 1)
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        idle_cv_.notify_all();
                    }
                }
            });
    }
}

void WorkerPool::drain()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        // Disable change notifications before clearing the queue so no
        // spurious UI updates fire during or after shutdown.
        on_change_ = nullptr;
        // Clear the pending queue so threads don't start new work after the
        // stop flag is set — matching the previous shutting_down_ guard.
        // These queued-but-not-started tasks are being dropped, not run, so
        // their in_flight_ contribution goes with them here; a task already
        // dequeued and executing on a worker thread is not in queue_ and
        // decrements in_flight_ itself once it finishes (see the worker
        // loop), so it must not be touched here.
        in_flight_.fetch_sub(queue_.size(), std::memory_order_relaxed);
        queue_.clear();
        pending_.store(0, std::memory_order_relaxed);
    }
    cv_.notify_all();
    idle_cv_.notify_all();
    for (auto& t : threads_)
    {
        // Joining a pool thread from one of its own tasks would deadlock.
        assert(!t.joinable() || t.get_id() != std::this_thread::get_id());
        if (t.joinable())
        {
            t.join();
        }
    }
}

WorkerPool::~WorkerPool()
{
    drain();
}

void WorkerPool::post(std::function<void()> fn)
{
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_)
        {
            return;
        }
        pending_.fetch_add(1, std::memory_order_relaxed);
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        queue_.push_back(std::move(fn));
        notify = on_change_;
    }
    cv_.notify_one();
    if (notify)
    {
        notify();
    }
}

} // namespace tesseract
