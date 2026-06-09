#include "session_persist_queue.h"

#include "tesseract/session_store.h"

#include <cstdio>
#include <exception>
#include <utility>

namespace tesseract
{

SessionPersistQueue::SessionPersistQueue(Writer writer) : writer_(writer)
{
    thread_ = std::thread([this] { run(); });
}

SessionPersistQueue::~SessionPersistQueue()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void SessionPersistQueue::enqueue(std::string user_id, std::string json)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // Coalesce: if a job for this user_id is already pending (not yet
        // picked up by the writer), replace its json so only the newest token
        // is written. The in-flight job — if any — is left alone; a job that is
        // currently being written keeps writing its (now-older) blob, and this
        // newer blob is what lands afterwards, so last-write-wins still holds.
        for (auto& j : queue_)
        {
            if (j.user_id == user_id)
            {
                j.json = std::move(json);
                cv_.notify_one();
                return;
            }
        }
        queue_.push_back(Job{std::move(user_id), std::move(json)});
    }
    cv_.notify_one();
}

void SessionPersistQueue::run()
{
    std::unique_lock<std::mutex> lk(mutex_);
    for (;;)
    {
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty())
        {
            // stop_ with nothing left to write.
            return;
        }

        Job job = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        lk.unlock();

        // The write itself may block (credential store / fsync); that's the
        // whole point of doing it here and not on the sync callback thread.
        // Mirror persist_session's guard(): a failed write must never escape
        // and kill the writer thread.
        try
        {
            writer_(job.user_id, job.json);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr,
                         "[tesseract] session persist write threw: %s "
                         "(dropped)\n",
                         e.what());
        }
        catch (...)
        {
            std::fprintf(stderr,
                         "[tesseract] session persist write threw non-exception "
                         "(dropped)\n");
        }

        lk.lock();
        busy_ = false;
        cv_.notify_all(); // wake any drain() waiters
    }
}

void SessionPersistQueue::drain()
{
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.notify_all();
    cv_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

namespace
{
void default_writer(const std::string& user_id, const std::string& json)
{
    SessionStore::save_account(user_id, json);
}
} // namespace

SessionPersistQueue& session_persist_queue()
{
    // Leaked on purpose — see header. `new` (never deleted) gives us a stable
    // worker thread for the whole process lifetime without risking a join
    // during static destruction while the SDK runtime may still be live.
    static SessionPersistQueue* instance =
        new SessionPersistQueue(&default_writer);
    return *instance;
}

} // namespace tesseract
