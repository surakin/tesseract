#pragma once
/// A dedicated single-thread writer that offloads session persistence off the
/// matrix-sdk sync/worker thread.
///
/// Why this exists: matrix-sdk's save_session_callback (reached via the
/// persist_session FFI) runs synchronously on a runtime worker thread. The
/// underlying SessionStore::save_account does a platform credential-store write
/// (Keychain / libsecret / Windows Credential Manager — any of which can be
/// slow or even *prompt* the user) plus an fsync'd atomic file write. Doing
/// that work inline can stall Matrix sync. This queue lets persist_session
/// enqueue the job and return promptly.
///
/// Correctness: multiple refreshes for the SAME user_id may arrive in quick
/// succession; only the newest token must survive. The queue therefore
/// coalesces per user_id (last-write-wins): a pending job for a user_id has its
/// json replaced rather than appended, which also bounds the queue under rapid
/// token rotation. Distinct user_ids are independent and preserve arrival order
/// among themselves.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace tesseract
{

/// Run `attempt` until it returns true, at most `attempts` times, sleeping
/// `first_backoff` before the second try and doubling it after each failure.
/// An exception counts as a failure. Returns whether an attempt succeeded.
/// `cancelled`, when set, is polled before each attempt and during each
/// backoff; once it returns true the retries stop and the result is false.
bool save_with_retries(const std::function<bool()>& attempt, int attempts,
                       std::chrono::milliseconds first_backoff,
                       const std::function<bool()>& cancelled = {});

/// Called (on the writer thread) with the account's user id when a session
/// write still failed after every retry. The handler must be quick: it runs
/// under the lock that set_session_save_failed_handler() takes, so clearing
/// the handler also waits for a call in progress. Pass an empty function to
/// clear.
void set_session_save_failed_handler(std::function<void(const std::string& user_id)> handler);

/// Invokes the handler set above, if any.
void report_session_save_failed(const std::string& user_id);

class SessionPersistQueue
{
public:
    /// The function that actually performs the (blocking) persistence for one
    /// job. Pulled out as a parameter purely so the coalescing/ordering logic
    /// is unit-testable without touching the real credential store / disk.
    using Writer = void (*)(const std::string& user_id, const std::string& json);

    explicit SessionPersistQueue(Writer writer);
    ~SessionPersistQueue();

    SessionPersistQueue(const SessionPersistQueue&) = delete;
    SessionPersistQueue& operator=(const SessionPersistQueue&) = delete;

    /// Enqueue (or coalesce) a persistence job and return immediately. If a job
    /// for `user_id` is already pending, its json is replaced (last-write-wins);
    /// otherwise a new job is appended. Never blocks on I/O.
    void enqueue(std::string user_id, std::string json);

    /// Block until every currently-pending job has been written. Used by
    /// SessionStore::flush_session_updates() for the synchronous
    /// (refresh / shutdown) path, and by tests.
    void drain();

    /// Drop every pending job for `user_id` and, if one is being written
    /// right now, wait for it to finish. Called before an account's
    /// credentials are cleared on logout, so a queued write can't land
    /// afterwards and recreate them. Later enqueue() calls for `user_id` are
    /// refused until revive() is called.
    void discard(const std::string& user_id);

    /// Accept enqueue() calls for `user_id` again. Called on a fresh login,
    /// after discard() had refused that account's writes.
    void revive(const std::string& user_id);

    /// Whether `user_id` is between discard() and revive().
    bool is_discarded(const std::string& user_id);

    /// Number of jobs waiting to be picked up (excludes one in flight).
    /// Test seam.
    std::size_t pending();

private:
    struct Job
    {
        std::string user_id;
        std::string json;
    };

    void run();

    Writer writer_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_; // distinct user_ids, FIFO among themselves
    bool busy_ = false;     // a job is currently being written
    std::string busy_user_; // user_id of that job, when busy_
    // Users discard()ed and not yet revive()d. A late token refresh for one of
    // them must not recreate its credentials.
    std::set<std::string> tombstones_;
    bool stop_ = false;
    std::thread thread_;
};

/// Process-wide singleton used by persist_session(). Constructed on first use
/// and intentionally leaked (function-local static with a leaked thread) to
/// sidestep static-destruction-order hazards: the matrix-sdk runtime may still
/// be firing save callbacks while C++ statics are being torn down at exit, and
/// joining the writer thread there would deadlock or use freed state. Draining
/// is best-effort at exit only.
SessionPersistQueue& session_persist_queue();

} // namespace tesseract
