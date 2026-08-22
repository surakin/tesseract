#pragma once

// Options → progress overlay for a room's full-history export. Modeled on
// ConfirmDialog for the card+backdrop shell, but unlike ConfirmDialog it is
// dismissible without cancelling whatever it's showing (the export keeps
// running after the dialog is closed — see ShellBase's status-bar re-entry
// hook), and it carries no direct reference to HistoryExportController:
// whoever mounts this dialog (ShellBase, via RoomView) wires its
// on_* request callbacks to the actual controller and feeds results back
// through set_resume_checkpoint()/show_progress()/show_finished(), the same
// decoupled-from-the-app-layer shape RoomInfoPanel and ConfirmDialog already
// use.

#include "tesseract/client.h"
#include "tk/canvas.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class ExportHistoryDialog : public tk::Widget
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
        Format format = Format::Html;
        bool include_images = false;
        bool zip_output = false;
        std::string resume_from_event_id; // non-empty when resuming
    };

    ExportHistoryDialog();
    ~ExportHistoryDialog() override = default;

    // Opens into the Options state for `room_id` (or BusyElsewhere if
    // `busy_other_room_id` is non-empty — i.e. a different room's export
    // is already active) and fires on_query_resume(room_id) so the caller
    // can populate set_resume_checkpoint() once the async probe returns.
    void open(std::string room_id, std::string room_display_name);

    // Skips straight to BusyElsewhere: another room's export is active.
    void open_busy_elsewhere(std::string busy_room_id,
                             std::string busy_room_display_name,
                             const tesseract::RoomExportProgress& last_progress);

    // Skips straight to InProgress for `room_id`, seeded from
    // `last_progress` — used when re-opening (room-info button or status
    // bar click) while this room's own export is still running.
    void open_in_progress(std::string room_id, std::string room_display_name,
                          const tesseract::RoomExportProgress& last_progress);

    // Hides the dialog WITHOUT cancelling an in-progress export — the
    // export keeps running; control moves to the status bar.
    void close();
    bool is_open() const { return open_; }

    // Populates the Options state's resume-vs-new choice. No-op if the
    // dialog isn't currently showing Options for the room the checkpoint
    // is for (e.g. the user already navigated away).
    void set_resume_checkpoint(tesseract::RoomExportCheckpoint checkpoint);

    // Progress tick for the room currently shown (Options→InProgress
    // transition on the first tick after Export/Resume is clicked, or a
    // running update while already in InProgress).
    void show_progress(const tesseract::RoomExportProgress& progress);

    // Terminal state. Shows a brief result with a Close button.
    void show_finished(bool ok, bool cancelled, std::string out_path,
                       std::uint64_t events_written, std::string error);

    // Fires when the dialog opens/closes/changes size, so the shell can
    // re-query native-overlay rects (mirrors ConfirmDialog::on_layout_changed).
    std::function<void()> on_layout_changed;

    // Fired once per open() with the room to probe a checkpoint for.
    std::function<void(std::string room_id)> on_query_resume;
    // Fired when the user clicks Export/Resume in the Options state.
    std::function<void(Request)> on_export_requested;
    // Fired when the user clicks Cancel in the InProgress state.
    std::function<void()> on_cancel_requested;
    // Fired from the BusyElsewhere state's "Go to that export" action.
    std::function<void(std::string room_id)> on_go_to_other_export;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

private:
    enum class State
    {
        Options,
        InProgress,
        BusyElsewhere,
        Done,
    };

    void reset_for_room_(std::string room_id, std::string room_display_name);
    void enter_in_progress_();
    void update_child_visibility_();
    void set_format_(Format f);

    bool open_ = false;
    State state_ = State::Options;

    std::string room_id_;
    std::string room_display_name_;
    tesseract::RoomExportCheckpoint checkpoint_;
    bool resume_available_ = false;

    Format format_ = Format::Html;
    bool include_images_ = false;
    bool zip_output_ = false;

    tesseract::RoomExportProgress last_progress_;
    bool finished_ok_ = false;
    bool finished_cancelled_ = false;
    std::string finished_out_path_;
    std::uint64_t finished_events_ = 0;
    std::string finished_error_;

    std::string busy_room_id_;
    std::string busy_room_display_name_;

    // Options-state controls.
    tk::ComboBox* format_combo_ = nullptr;
    tk::CheckButton* include_images_btn_ = nullptr;
    tk::CheckButton* zip_output_btn_ = nullptr;
    tk::Button* export_btn_ = nullptr;
    tk::Button* resume_btn_ = nullptr;
    tk::Button* start_new_btn_ = nullptr;

    // InProgress-state controls.
    tk::ProgressBar* progress_bar_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;

    // BusyElsewhere-state controls.
    tk::Button* go_to_other_btn_ = nullptr;

    // Done-state controls.
    tk::Button* close_btn_ = nullptr;

    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};
    bool press_backdrop_ = false;

    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> body_layout_;

    static constexpr float kCardW    = 400.0f;
    static constexpr float kCardPad  = 20.0f;
    static constexpr float kRowH     = 32.0f;
    static constexpr float kRowGap   = 8.0f;
    static constexpr float kBtnH     = 36.0f;
    static constexpr float kTitleH   = 22.0f;
    static constexpr float kTitleGap = 12.0f;
};

} // namespace tesseract::views
