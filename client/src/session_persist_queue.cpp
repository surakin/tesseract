#include "session_persist_queue.h"

#include "tesseract/session_store.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

namespace tesseract
{

bool save_with_retries(const std::function<bool()>& attempt, int attempts,
                       std::chrono::milliseconds first_backoff,
                       const std::function<bool()>& cancelled)
{
    auto backoff = first_backoff;
    for (int i = 1; i <= attempts; ++i)
    {
        if (cancelled && cancelled())
        {
            return false;
        }
        try
        {
            if (attempt())
            {
                return true;
            }
        }
        catch (...)
        {
            // Counted as a failed attempt like a false return.
        }
        if (i < attempts)
        {
            // Sliced so a cancel (the account signing out) is noticed promptly.
            const auto until = std::chrono::steady_clock::now() + backoff;
            while (std::chrono::steady_clock::now() < until)
            {
                if (cancelled && cancelled())
                {
                    return false;
                }
                std::this_thread::sleep_for(
                    std::min(std::chrono::milliseconds(25),
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 until - std::chrono::steady_clock::now()) +
                                 std::chrono::milliseconds(1)));
            }
            backoff *= 2;
        }
    }
    return false;
}

namespace
{
std::mutex g_save_failed_mutex;
std::function<void(const std::string&)> g_save_failed_handler;
} // namespace

void set_session_save_failed_handler(std::function<void(const std::string& user_id)> handler)
{
    std::lock_guard<std::mutex> lk(g_save_failed_mutex);
    g_save_failed_handler = std::move(handler);
}

void report_session_save_failed(const std::string& user_id)
{
    std::lock_guard<std::mutex> lk(g_save_failed_mutex);
    if (g_save_failed_handler)
    {
        g_save_failed_handler(user_id);
    }
}

SessionPersistQueue::SessionPersistQueue(Writer writer) : writer_(writer)
{
    thread_ = std::thread(
        [this]
        {
            run();
        });
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
        if (tombstones_.contains(user_id))
        {
            return; // logged out: a late refresh must not recreate the account
        }
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
        cv_.wait(lk,
                 [this]
                 {
                     return stop_ || !queue_.empty();
                 });
        if (queue_.empty())
        {
            // stop_ with nothing left to write.
            return;
        }

        Job job = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        busy_user_ = job.user_id;
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
            std::fprintf(stderr, "[tesseract] session persist write threw non-exception "
                                 "(dropped)\n");
        }

        lk.lock();
        busy_ = false;
        busy_user_.clear();
        cv_.notify_all(); // wake any drain() / discard() waiters
    }
}

void SessionPersistQueue::drain()
{
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.notify_all();
    cv_.wait(lk,
             [this]
             {
                 return queue_.empty() && !busy_;
             });
}

std::size_t SessionPersistQueue::pending()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

void SessionPersistQueue::discard(const std::string& user_id)
{
    std::unique_lock<std::mutex> lk(mutex_);
    tombstones_.insert(user_id);
    for (auto it = queue_.begin(); it != queue_.end();)
    {
        if (it->user_id == user_id)
        {
            it = queue_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    cv_.wait(lk,
             [this, &user_id]
             {
                 return !(busy_ && busy_user_ == user_id);
             });
}

void SessionPersistQueue::revive(const std::string& user_id)
{
    std::lock_guard<std::mutex> lk(mutex_);
    tombstones_.erase(user_id);
}

bool SessionPersistQueue::is_discarded(const std::string& user_id)
{
    std::lock_guard<std::mutex> lk(mutex_);
    return tombstones_.contains(user_id);
}

namespace
{
constexpr int kPersistAttempts = 4;
constexpr std::chrono::milliseconds kPersistFirstBackoff{500};

void default_writer(const std::string& user_id, const std::string& json)
{
    // save_session_update, not save_account: this fires on every OAuth
    // token refresh, which must not silently strip a store-encryption key
    // persisted at login time (see SessionStore::save_session_update).
    //
    // A rotated refresh token is single-use on the server, so an unsaved one
    // is lost. Retry transient failures (a locked keyring, a full disk)
    // before giving up. This runs on the writer thread, so the SDK is not
    // blocked, and jobs queued meanwhile still coalesce.
    const bool saved = save_with_retries(
        [&]
        {
            return SessionStore::save_session_update(user_id, json);
        },
        kPersistAttempts, kPersistFirstBackoff,
        // A sign-out waits in discard() for this write, so stop retrying for it.
        [&]
        {
            return session_persist_queue().is_discarded(user_id);
        });
    if (!saved && !session_persist_queue().is_discarded(user_id))
    {
        std::fprintf(stderr,
                     "[tesseract] could not save the session for %s after %d "
                     "attempts; the next launch may need a sign-in\n",
                     user_id.c_str(), kPersistAttempts);
        report_session_save_failed(user_id);
    }
}
} // namespace

SessionPersistQueue& session_persist_queue()
{
    // Leaked on purpose — see header. `new` (never deleted) gives us a stable
    // worker thread for the whole process lifetime without risking a join
    // during static destruction while the SDK runtime may still be live.
    static SessionPersistQueue* instance = new SessionPersistQueue(&default_writer);
    return *instance;
}

} // namespace tesseract
