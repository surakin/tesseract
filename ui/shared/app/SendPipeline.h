#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace tesseract
{

// Per-room ordered send queue with an optional slow "prepare" step, plus a
// per-room busy signal for the composer's send-button spinner.
//
// Some sends need work before the event can be built (today: generating
// MSC4095 bundled URL previews, which can take seconds). That work must not
// run on the single `&mut` FFI worker, or every other mutating call (room
// switches, reactions, edits) would queue behind it. So a send is split in
// two: `prepare` runs on the read pool, and `send` (given prepare's result)
// runs on the `&mut` worker. Sends for one room are handed to the `&mut`
// worker strictly in submission order: a later send whose prepare finished
// first (or that needs none) waits for the earlier ones. Rooms never wait
// for each other.
//
// A room is reported busy once it has had a send outstanding (preparing,
// queued, or sending) for `kBusyDelayMs`, and idle again when its last send
// finishes, so ordinary sends never flash the spinner.
//
// UI-thread only, except that `prepare`/`send` run on the workers the hooks
// dispatch them to. The owner's `post_ui` / `post_ui_after` hooks must be
// liveness-guarded (continuations posted after the owner dies must no-op).
class SendPipeline
{
public:
    struct Hooks
    {
        // Run fn on the read worker pool (prepare steps; any parallelism).
        std::function<void(std::function<void()>)> run_prepare;
        // Run fn on the single `&mut` FFI worker (sends; FIFO).
        std::function<void(std::function<void()>)> run_send;
        // Post fn to the UI thread.
        std::function<void(std::function<void()>)> post_ui;
        // Post fn to the UI thread after `ms` milliseconds.
        std::function<void(int ms, std::function<void()>)> post_ui_after;
        // A room's busy state flipped (UI thread).
        std::function<void(const std::string& room_id, bool busy)> on_busy_changed;
    };

    // Runs on the read pool; the returned string is handed to `Send`.
    using Prepare = std::function<std::string()>;
    // Runs on the `&mut` worker with the prepare result (empty when there
    // was no prepare step).
    using Send = std::function<void(const std::string& prepared)>;

    static constexpr int kBusyDelayMs = 200;

    explicit SendPipeline(Hooks hooks);

    // Queue a send for `room_id`. `prepare` may be empty (no prepare step);
    // the send still keeps its place in the room's order.
    void submit(const std::string& room_id, Prepare prepare, Send send);

    // True while `room_id` is reported busy (see class comment).
    bool is_busy(const std::string& room_id) const;

    // Sends outstanding for `room_id` (preparing, queued or sending).
    std::size_t outstanding(const std::string& room_id) const;

private:
    struct Entry
    {
        std::uint64_t id = 0;
        bool ready = false;
        std::string prepared;
        Send send;
    };

    struct RoomState
    {
        std::deque<Entry> queue; // not yet handed to the `&mut` worker
        std::size_t outstanding = 0;
        bool busy = false;
        // Identifies the current burst of sends (set when the room goes
        // from idle to non-idle), so a busy-delay timer armed for an earlier
        // burst can't mark a later burst busy early.
        std::uint64_t epoch = 0;
    };

    void on_prepared_(const std::string& room_id, std::uint64_t id, std::string prepared);
    void pump_(const std::string& room_id);
    void on_sent_(const std::string& room_id);

    Hooks hooks_;
    std::unordered_map<std::string, RoomState> rooms_;
    std::uint64_t next_id_ = 1;
    // Global, not per room: a room's RoomState is erased when it goes idle,
    // so a per-room counter would restart and let a stale timer match.
    std::uint64_t next_epoch_ = 1;
};

} // namespace tesseract
