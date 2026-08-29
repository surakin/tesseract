#pragma once

#include <functional>
#include <string>

namespace tk
{

// Cross-backend single-instance guard for the two Linux UIs (Qt6, GTK4).
// Both binaries contend for the *same* flock path and speak the *same*
// tiny newline-delimited Unix-socket protocol below, so launching one while
// the other is already running is detected and forwarded instead of both
// processes opening the matrix-sdk store concurrently.

struct SingleInstanceLock
{
    bool acquired = false;
};

// Try to become the primary Tesseract instance for this Unix user. On
// success, holds the lock open for the remaining lifetime of the process
// (the OS releases it automatically on exit or crash) and returns
// acquired = true. On failure (another Tesseract process — either backend —
// already holds it) returns acquired = false; the caller should
// forward_activation_request() and exit without starting its UI.
SingleInstanceLock acquire_single_instance_lock();

// Best-effort: connect to whichever process currently holds the lock and
// hand it an activation request. `token` is an XDG_ACTIVATION_TOKEN (may be
// empty); `uri` is an optional matrix: URI to open (may be empty). Returns
// false if no listener was reachable within a short timeout — the caller
// should exit either way, there is nothing more useful to do.
bool forward_activation_request(const std::string& token, const std::string& uri);

// Listens for activation requests forwarded by later launches of either
// backend. Only meaningful after acquire_single_instance_lock() returned
// acquired = true. Deliberately toolkit-agnostic: exposes a raw fd for the
// caller to watch for readability on its own event loop (GLib:
// g_unix_fd_add; Qt: QSocketNotifier) and call on_readable() when it fires.
class ActivationListener
{
public:
    // Invoked with (token, uri) once a peer's request has been read in
    // full. `uri` is empty when the peer sent none.
    using Callback = std::function<void(std::string token, std::string uri)>;

    explicit ActivationListener(Callback on_activate);
    ~ActivationListener();
    ActivationListener(const ActivationListener&) = delete;
    ActivationListener& operator=(const ActivationListener&) = delete;

    // -1 if listening failed to start. This is a best-effort feature (it
    // only means later launches can't raise this window) — callers should
    // tolerate -1 rather than treat it as fatal.
    int fd() const
    {
        return listen_fd_;
    }

    // Call when fd() becomes readable: accepts the one pending connection,
    // reads its small bounded payload synchronously, and invokes the
    // callback.
    void on_readable();

private:
    int listen_fd_ = -1;
    Callback on_activate_;
};

} // namespace tk
