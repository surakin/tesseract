#pragma once

#include <tesseract/launch_args.h>

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

// What a losing launch hands the running instance. Every field may be empty.
struct ActivationRequest
{
    std::string token;   // XDG_ACTIVATION_TOKEN issued to the new launch
    std::string uri;     // matrix: / matrix.to link to open
    std::string action;  // launch option id, e.g. "open-settings", "open-room"
    std::string room_id; // target of "open-room"
};

// Wire format: newline-delimited, one field per line in the order above.
// Older peers wrote only the first two lines; missing trailing lines parse
// as empty. Embedded newlines are stripped when formatting.
std::string format_activation_payload(const ActivationRequest& req);
ActivationRequest parse_activation_payload(const std::string& payload);

// The request a losing launch forwards: its launch intents plus the
// XDG_ACTIVATION_TOKEN from its environment.
ActivationRequest activation_request_for(const tesseract::LaunchArgs& args);

// Try to become the primary Tesseract instance for this Unix user and the
// active --profile (tesseract::profile_suffix() is part of the lock and
// socket paths, so different profiles run side by side). On
// success, holds the lock open for the remaining lifetime of the process
// (the OS releases it automatically on exit or crash) and returns
// acquired = true. On failure (another Tesseract process — either backend —
// already holds it) returns acquired = false; the caller should
// forward_activation_request() and exit without starting its UI.
SingleInstanceLock acquire_single_instance_lock();

// PID of the process holding this profile's lock (it writes it into the lock
// file on acquiring), or 0 when unknown. Used by the macOS shell to activate
// exactly the right running instance when several profiles share a bundle id.
int single_instance_owner_pid();

// Best-effort: connect to whichever process currently holds the lock and
// hand it `request` (see ActivationRequest). Returns
// false if no listener was reachable within a short timeout — the caller
// should exit either way, there is nothing more useful to do.
bool forward_activation_request(const ActivationRequest& request);

// Listens for activation requests forwarded by later launches of either
// backend. Only meaningful after acquire_single_instance_lock() returned
// acquired = true. Deliberately toolkit-agnostic: exposes a raw fd for the
// caller to watch for readability on its own event loop (GLib:
// g_unix_fd_add; Qt: QSocketNotifier) and call on_readable() when it fires.
class ActivationListener
{
public:
    // Invoked once a peer's request has been read in full.
    using Callback = std::function<void(ActivationRequest request)>;

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
