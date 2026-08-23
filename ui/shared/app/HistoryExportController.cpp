#include "HistoryExportController.h"
#include "tk/i18n.h"

namespace tesseract
{

HistoryExportController::HistoryExportController(
    tesseract::Client* client,
    std::function<void(std::function<void()>)> post_to_ui,
    std::function<void(std::function<void()>)> run_async)
    : client_(client), post_to_ui_(std::move(post_to_ui)), run_async_(std::move(run_async))
{
}

HistoryExportController::~HistoryExportController()
{
    invalidate_weak_self();
}

void HistoryExportController::set_client(tesseract::Client* client)
{
    client_ = client;
}

std::string HistoryExportController::extension_for(Format format)
{
    return format == Format::Html ? "html" : "txt";
}

std::string HistoryExportController::suggested_folder_name(const Request& req)
{
    std::string base = req.room_display_name.empty() ? std::string("room")
                                                      : req.room_display_name;
    std::string sanitized;
    sanitized.reserve(base.size());
    for (char c : base)
    {
        switch (c)
        {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            sanitized.push_back('_');
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                sanitized.push_back('_');
            else
                sanitized.push_back(c);
        }
    }
    return sanitized + "-history";
}

// Fixed-order label templates for room-history export prose. Order MUST
// match `history_export::labels::ExportLabel` on the Rust side exactly —
// Rust only substitutes `{0}`/`{1}` placeholders in these; it never
// composes English text itself. Both sides carry a count-assert test
// against drift (this file's Catch2 test + labels.rs's own COUNT check).
//
// The membership entries (indices 10-31) intentionally reuse the exact
// wording from MessageListView.cpp's membership_expanded_phrase(),
// including its by-actor/no-actor split, so translators aren't asked to
// translate the same sentiment twice and an exported room reads
// consistently with what the live view already showed. One exception:
// "knock_retracted" drops the live view's possessive-pronoun clause
// ("{0} withdrew {1} request to join") since this export pipeline has no
// pronoun data to supply — it falls back to a pronoun-free passive form.
std::vector<std::string> HistoryExportController::build_labels()
{
    return {
        tk::tr("History of {0}"),
        tk::tr("Exported {0}"),
        tk::tr("Unable to decrypt this message"),
        tk::tr("Message deleted"),
        tk::tr("(edited)"),
        tk::tr("Attachment: {0}"),
        tk::tr("Attachment too large: {0}"),
        tk::tr("Image unavailable: {0}"),
        tk::tr("In reply to {0}"),
        tk::tr("Reactions: {0}"),
        tk::tr("{0} joined the room"),
        tk::tr("{0} left the room"),
        tk::tr("{0} was banned by {1}"),
        tk::tr("{0} was banned"),
        tk::tr("{0} was unbanned by {1}"),
        tk::tr("{0} is no longer banned"),
        tk::tr("{0} was removed by {1}"),
        tk::tr("{0} was removed"),
        tk::tr("{0} was invited by {1}"),
        tk::tr("{0} received an invitation"),
        tk::tr("{0} was removed and banned by {1}"),
        tk::tr("{0} was kicked and banned"),
        tk::tr("{0} has accepted the invitation"),
        tk::tr("{0} has rejected the invitation"),
        tk::tr("{0}'s invitation was revoked by {1}"),
        tk::tr("{0}'s invitation was revoked"),
        tk::tr("{0} requested to join"),
        tk::tr("{0}'s request to join was approved by {1}"),
        tk::tr("{0}'s join request was approved"),
        tk::tr("{0}'s request to join was withdrawn"),
        tk::tr("{0}'s request to join was denied by {1}"),
        tk::tr("{0}'s join request was denied"),
    };
}

void HistoryExportController::begin(Request req)
{
    if (!show_save_folder_dialog || active_)
        return;

    show_save_folder_dialog(
        suggested_folder_name(req),
        guarded(
            [this, req = std::move(req)](std::string path) mutable
            {
                if (path.empty())
                    return;

                tesseract::RoomExportOptions options;
                options.out_path              = path;
                options.format                = extension_for(req.format);
                options.include_images        = req.include_images && req.format == Format::Html;
                options.zip_output            = req.zip_output;
                options.labels                = build_labels();
                options.resume_from_event_id  = req.resume_from_event_id;

                const std::uint64_t request_id = ++next_request_id_;
                active_          = true;
                active_request_id_ = request_id;
                active_room_id_   = req.room_id;
                active_out_path_  = path;
                last_progress_    = tesseract::RoomExportProgress{};

                if (on_started)
                    on_started(req.room_id, path);

                auto* c = client_;
                // A "fresh" request (no explicit resume point) must not
                // silently resume from a stale checkpoint — Rust's
                // start_room_export_async auto-matches any existing
                // checkpoint by (room_id, format, out_path) regardless of
                // caller intent, so the dialog's "Start new export" choice
                // (which only changes local UI state, see
                // ExportHistoryDialog::start_new_btn_) would otherwise
                // inherit an old, unrelated run's events_written/
                // oldest_ts_ms baseline. Clearing first, in the same
                // async call, guarantees this happens before Rust's own
                // checkpoint lookup — no race between a separate clear
                // call and the start call landing on different threads.
                const bool is_fresh_request = options.resume_from_event_id.empty();
                run_async_(guarded(
                    [c, request_id, room_id = req.room_id, options = std::move(options), is_fresh_request]() mutable
                    {
                        if (!c)
                            return;
                        if (is_fresh_request)
                            c->clear_room_export_checkpoint(room_id);
                        c->start_room_export_async(request_id, room_id, options);
                    }));
            }));
}

void HistoryExportController::cancel()
{
    if (!active_ || !client_)
        return;
    client_->cancel_room_export(active_request_id_);
}

void HistoryExportController::query_resume(std::string room_id)
{
    auto* c = client_;
    if (!c)
    {
        if (on_resume_available)
            on_resume_available(tesseract::RoomExportCheckpoint{});
        return;
    }
    run_async_(guarded(
        [this, c, room_id = std::move(room_id)]() mutable
        {
            auto cp = c->room_export_checkpoint(room_id);
            post_to_ui_(guarded(
                [this, cp = std::move(cp)]() mutable
                {
                    if (on_resume_available)
                        on_resume_available(std::move(cp));
                }));
        }));
}

void HistoryExportController::handle_progress(const tesseract::RoomExportProgress& progress)
{
    if (!active_ || progress.request_id != active_request_id_)
        return;
    last_progress_ = progress;
    if (on_progress)
        on_progress(progress);
}

void HistoryExportController::handle_complete(std::uint64_t request_id, bool ok, bool cancelled,
                                              bool reached_start, std::string out_path,
                                              std::uint64_t events_written,
                                              std::uint64_t bytes_written, std::string message)
{
    (void)reached_start;
    (void)bytes_written;
    if (request_id != active_request_id_)
        return;
    active_ = false;
    active_room_id_.clear();
    active_out_path_.clear();
    if (on_finished)
        on_finished(ok, cancelled, std::move(out_path), events_written, std::move(message));
}

} // namespace tesseract
