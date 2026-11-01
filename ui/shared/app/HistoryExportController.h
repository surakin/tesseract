#pragma once

#include "tesseract/client.h"
#include "tk/weak_self.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{

/// Orchestrates a room-history export: the target-folder dialog, the
/// resume-checkpoint probe, and the `Client::start_room_export_async`
/// call, then relays progress/completion back to whichever view is
/// currently showing it. Modeled on `SettingsController`'s injection
/// style (a bag of `std::function`s, no `ShellBase*`) so it's testable
/// standalone.
///
/// The SDK enforces "only one export runs app-wide at a time" — this
/// controller mirrors that check synchronously via `active()`/
/// `active_room_id()` so the UI can react (e.g. show a "busy elsewhere"
/// state) without a round trip to Rust.
class HistoryExportController : public tk::EnableWeakSelf<HistoryExportController>
{
public:
    enum class Format
    {
        Text,
        Html,
    };

    struct Request
    {
        std::string room_id;
        std::string room_display_name;
        Format format = Format::Html;
        bool include_images = false; // HTML only
        bool zip_output = false;
        /// Oldest event to include, Unix ms. 0 = walk all the way back to
        /// the room's creation.
        std::uint64_t stop_at_ts_ms = 0;
        /// Non-empty when resuming a prior checkpoint (its `oldest_event_id`).
        std::string resume_from_event_id;
    };

    HistoryExportController(tesseract::Client* client,
                            std::function<void(std::function<void()>)> post_to_ui,
                            std::function<void(std::function<void()>)> run_async,
                            std::shared_ptr<void> client_owner = {});
    ~HistoryExportController();

    /// Rebind to another account's client (or nullptr on logout).
    /// `client_owner` is whatever keeps `client` alive (the shell's
    /// AccountSession); worker jobs hold a copy so the client can't be
    /// destroyed under a blocking call. Switching to a different client
    /// cancels any export running on the old one and reports it via
    /// `on_finished(cancelled=true)`, so its late progress/completion
    /// events are ignored.
    void set_client(tesseract::Client* client, std::shared_ptr<void> client_owner = {});

    // Native folder-picker, installed by ShellBase::ensure_history_export_controller_
    // (via pick_folder_). Invoked with a suggested folder name; the callback
    // receives the chosen path, or empty on cancel.
    std::function<void(std::string suggested_name, std::function<void(std::string)> cb)>
        show_save_folder_dialog;

    // Full flow: `show_save_folder_dialog` → build options (labels via
    // `tk::tr`/`tk::trf`, built in `build_labels_()` — the only place this
    // controller composes prose) → `run_async_` → `Client::start_room_export_async`.
    // No-op if an export is already active (for this room or another) or
    // `show_save_folder_dialog` is unset.
    void begin(Request req);

    // Cooperatively cancels the active export, if any. Discards the
    // in-flight document.
    void cancel();

    // Cooperatively stops the active export, if any, at its next safe
    // point: finishes normally (full assembly/zip) with whatever was
    // gathered so far, unlike `cancel()`.
    void stop();

    bool active() const
    {
        return active_;
    }
    const std::string& active_room_id() const
    {
        return active_room_id_;
    }
    const tesseract::RoomExportProgress& last_progress() const
    {
        return last_progress_;
    }

    // Async checkpoint probe for `room_id`; result delivered via
    // `on_resume_available` on the UI thread. `on_resume_available` fires
    // with `exists=false` when there is no checkpoint, so callers don't
    // need a separate "no checkpoint" callback.
    void query_resume(std::string room_id);

    // UI-thread outputs.
    std::function<void(std::string room_id, std::string out_path)> on_started;
    std::function<void(const tesseract::RoomExportProgress&)> on_progress;
    std::function<void(bool ok, bool cancelled, std::string out_path, std::uint64_t events_written,
                       std::string error)>
        on_finished;
    std::function<void(tesseract::RoomExportCheckpoint)> on_resume_available;

    // Called by `ShellBase`'s marshalled `IEventHandler` overrides (already
    // on the UI thread).
    void handle_progress(const tesseract::RoomExportProgress& progress);
    void handle_complete(std::uint64_t request_id, bool ok, bool cancelled, bool reached_start,
                         std::string out_path, std::uint64_t events_written,
                         std::uint64_t bytes_written, std::string message);

    // Pure: no Client, no UI calls — safe to unit-test directly (see
    // ThreadPanelController::compute_transition for the same pattern).
    static std::vector<std::string> build_labels();
    static std::string suggested_folder_name(const Request& req);
    static std::string extension_for(Format format);

private:
    tesseract::Client* client_ = nullptr;
    std::shared_ptr<void> client_owner_;
    std::function<void(std::function<void()>)> post_to_ui_;
    std::function<void(std::function<void()>)> run_async_;

    std::uint64_t next_request_id_ = 0;
    bool active_ = false;
    std::uint64_t active_request_id_ = 0;
    std::string active_room_id_;
    std::string active_out_path_;
    tesseract::RoomExportProgress last_progress_;
};

} // namespace tesseract
