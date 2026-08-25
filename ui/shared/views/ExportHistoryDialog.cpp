#include "ExportHistoryDialog.h"

#include "media_utils.h" // rect_contains
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace tesseract::views
{

ExportHistoryDialog::ExportHistoryDialog()
{
    include_images_btn_ = add_child(tk::create_widget<tk::CheckButton>(this, tk::tr("Include images")));
    include_images_btn_->on_change = [this](bool on) { include_images_ = on; };

    zip_output_btn_ = add_child(tk::create_widget<tk::CheckButton>(this, tk::tr("Package as a .zip file")));
    zip_output_btn_->on_change = [this](bool on) { zip_output_ = on; };

    export_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Export"), std::function<void()>{}, tk::Button::Variant::Primary));
    export_btn_->set_on_click([this]() {
        Request req;
        req.room_id = room_id_;
        req.format = format_;
        req.include_images = include_images_ && format_ == Format::Html;
        req.zip_output = zip_output_;
        if (on_export_requested) on_export_requested(req);
    });

    resume_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Resume export"), std::function<void()>{}, tk::Button::Variant::Primary));
    resume_btn_->set_on_click([this]() {
        Request req;
        req.room_id = room_id_;
        req.format = format_;
        req.include_images = include_images_ && format_ == Format::Html;
        req.zip_output = zip_output_;
        req.resume_from_event_id = checkpoint_.oldest_event_id;
        if (on_export_requested) on_export_requested(req);
    });

    start_new_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Start new export"), std::function<void()>{}, tk::Button::Variant::Subtle));
    start_new_btn_->set_on_click([this]() {
        resume_available_ = false;
        body_layout_.reset();
        update_child_visibility_();
    });

    cancel_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"), std::function<void()>{}, tk::Button::Variant::Subtle));
    cancel_btn_->set_on_click([this]() {
        if (on_cancel_requested) on_cancel_requested();
    });

    go_to_other_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Go to that export"), std::function<void()>{}, tk::Button::Variant::Primary));
    go_to_other_btn_->set_on_click([this]() {
        if (on_go_to_other_export) on_go_to_other_export(busy_room_id_);
    });

    close_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Close"), std::function<void()>{}, tk::Button::Variant::Primary));
    close_btn_->set_on_click([this]() { close(); });

    progress_bar_ = add_child(tk::create_widget<tk::ProgressBar>(this));

    // Added last (like RoomInfoPanel's notification combo) even though it
    // sits near the top of the Options form — later-added children are
    // dispatched first, so the expanded dropdown correctly captures
    // pointer events over the rows below it instead of them stealing the
    // click.
    auto format_combo = tk::create_widget<tk::ComboBox>(this);
    format_combo->set_options({
        {.label = tk::tr("HTML"),       .value = "html"},
        {.label = tk::tr("Plain text"), .value = "txt"},
    });
    format_combo->set_selected_value("html");
    format_combo->on_changed = [this](std::string value) {
        set_format_(value == "txt" ? Format::Text : Format::Html);
    };
    format_combo_ = add_child(std::move(format_combo));

    set_visible(false);
}

void ExportHistoryDialog::set_format_(Format f)
{
    format_ = f;
    if (format_combo_)
        format_combo_->set_selected_value(f == Format::Text ? "txt" : "html");
    if (f == Format::Text)
    {
        include_images_ = false;
        include_images_btn_->set_checked(false);
    }
    update_child_visibility_();
}

void ExportHistoryDialog::reset_for_room_(std::string room_id, std::string room_display_name)
{
    room_id_ = std::move(room_id);
    room_display_name_ = std::move(room_display_name);
    checkpoint_ = tesseract::RoomExportCheckpoint{};
    resume_available_ = false;
    last_progress_ = tesseract::RoomExportProgress{};
    title_layout_.reset();
    body_layout_.reset();
    set_format_(Format::Html);
    zip_output_ = false;
    zip_output_btn_->set_checked(false);
    // Invalidates any deferred show_finished() call still pending from a
    // previous export (see kMinInProgressDurationMs) — it must not apply
    // to this new one.
    ++finish_generation_;
}

void ExportHistoryDialog::open(std::string room_id, std::string room_display_name)
{
    const bool was_open = open_;
    const std::string opened_room_id = room_id;

    reset_for_room_(std::move(room_id), std::move(room_display_name));
    state_ = State::Options;
    open_  = true;
    set_visible(true);
    press_backdrop_ = false;
    update_child_visibility_();

    if (on_query_resume) on_query_resume(opened_room_id);
    if (!was_open && on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::open_busy_elsewhere(std::string busy_room_id,
                                              std::string busy_room_display_name,
                                              const tesseract::RoomExportProgress& last_progress)
{
    const bool was_open = open_;
    busy_room_id_ = std::move(busy_room_id);
    busy_room_display_name_ = std::move(busy_room_display_name);
    last_progress_ = last_progress;
    state_ = State::BusyElsewhere;
    open_  = true;
    set_visible(true);
    press_backdrop_ = false;
    title_layout_.reset();
    body_layout_.reset();
    update_child_visibility_();
    if (!was_open && on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::open_in_progress(std::string room_id, std::string room_display_name,
                                           const tesseract::RoomExportProgress& last_progress)
{
    const bool was_open = open_;
    room_id_ = std::move(room_id);
    room_display_name_ = std::move(room_display_name);
    last_progress_ = last_progress;
    state_ = State::InProgress;
    open_  = true;
    set_visible(true);
    press_backdrop_ = false;
    title_layout_.reset();
    body_layout_.reset();
    in_progress_started_at_ = std::chrono::steady_clock::now();
    finalizing_started_ = false;
    ++finish_generation_;
    update_child_visibility_();
    if (!was_open && on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    press_backdrop_ = false;
    update_child_visibility_();
    if (was_open && on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::set_resume_checkpoint(tesseract::RoomExportCheckpoint checkpoint)
{
    if (state_ != State::Options || checkpoint.room_id != room_id_)
        return;
    checkpoint_ = std::move(checkpoint);
    resume_available_ = checkpoint_.exists;
    body_layout_.reset();
    update_child_visibility_();
}

void ExportHistoryDialog::enter_in_progress_()
{
    state_ = State::InProgress;
    title_layout_.reset();
    body_layout_.reset();
    in_progress_started_at_ = std::chrono::steady_clock::now();
    finalizing_started_ = false;
    ++finish_generation_;
    update_child_visibility_();
    if (on_layout_changed) on_layout_changed();
}

std::optional<float> ExportHistoryDialog::compute_progress_fraction(const tesseract::RoomExportProgress& p)
{
    // Gathering (pagination) has no reliable fraction at all — a time-ratio
    // estimate (newest/oldest vs. room creation) assumes roughly uniform
    // message density over the room's whole lifetime, and real rooms can
    // violate that badly: measured directly on a real room, some 100-event
    // batches spanned under an hour while others spanned over a week, and
    // the final stretch alone spanned 271 days. That isn't a rare edge
    // case to special-case around — there's no way to know in advance
    // whether a given room's density is even, so no number is claimed
    // during gathering at all (nullopt → the bar's self-animating
    // indeterminate mode); the caption still shows the live, accurate
    // date, which doesn't have this problem. `reached_start` is
    // authoritative once available, and exporting (concatenate, then zip)
    // has a real, known total (segments + files to zip), so both of those
    // get real, determinate fractions — filling 85%..100%, leaving
    // gathering's indeterminate stretch as 0..85% conceptually, without
    // ever claiming a specific number for it.
    constexpr float kGatheringBandMax = 0.85f;

    if (p.finalizing)
    {
        if (p.assembly_total == 0)
            return std::nullopt; // defensive: no known total (shouldn't happen for a non-empty export)
        const float done = static_cast<float>(p.assembly_done);
        const float total = static_cast<float>(p.assembly_total);
        const float sub = std::clamp(done / total, 0.0f, 1.0f);
        return kGatheringBandMax + sub * (1.0f - kGatheringBandMax);
    }
    if (p.reached_start)
    {
        return kGatheringBandMax;
    }
    return std::nullopt;
}

std::string ExportHistoryDialog::format_short_date(std::uint64_t ts_ms)
{
    std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm_val{};
#if defined(_WIN32)
    localtime_s(&tm_val, &t);
#else
    localtime_r(&t, &tm_val);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
    return std::string(buf);
}

std::uint64_t ExportHistoryDialog::select_progress_display_ts(const tesseract::RoomExportProgress& p)
{
    return (p.reached_start && p.room_created_ts_ms != 0) ? p.room_created_ts_ms : p.oldest_ts_ms;
}

void ExportHistoryDialog::show_progress(const tesseract::RoomExportProgress& progress)
{
    const bool room_created_just_learned =
        last_progress_.room_created_ts_ms == 0 && progress.room_created_ts_ms != 0;
    const bool finalizing_just_started = !last_progress_.finalizing && progress.finalizing;
    const bool was_options = state_ == State::Options;
    last_progress_ = progress;
    if (was_options)
        enter_in_progress_(); // its own on_layout_changed() already reflects last_progress_ above
    if (room_created_just_learned)
    {
        // The InProgress body line only exists once room_created_ts_ms is
        // known (see arrange()'s InProgress case), so the progress bar's
        // already-arranged position — fixed by whatever arrange() pass ran
        // before this became true — doesn't yet leave room for it.
        // Resetting the cached text layout alone only affects *painting*;
        // without an actual relayout, the bar keeps its old position and
        // the new body line draws right on top of it.
        body_layout_.reset();
        if (!was_options && on_layout_changed)
            on_layout_changed();
    }
    if (finalizing_just_started)
    {
        finalizing_started_at_ = std::chrono::steady_clock::now();
        finalizing_started_ = true;
    }
    update_child_visibility_(); // e.g. hides Cancel once finalizing starts

    if (!progress_bar_)
        return;

    const auto fraction = compute_progress_fraction(last_progress_);
    if (fraction)
        progress_bar_->set_progress(*fraction);
    else
        progress_bar_->set_indeterminate();

    if (last_progress_.finalizing)
    {
        // reached_start is already true by the time finalizing starts (see
        // mod.rs's assembly-section emit_progress calls), so the corrected
        // date belongs here too — not just in the gathering caption below.
        // That matters because the single tick where reached_start just
        // became true but finalizing hasn't started yet can be superseded
        // by the very next (finalizing) tick before the UI ever paints it
        // (both fire with no yield in between) — finalizing lasts long
        // enough to actually be seen, so anchoring the date here as well
        // makes it reliably visible regardless of that race.
        std::string label = last_progress_.assembly_total != 0
            ? tk::trf(tk::tr("Finalizing export… ({0} of {1})"),
                     {std::to_string(last_progress_.assembly_done), std::to_string(last_progress_.assembly_total)})
            : tk::tr("Finalizing export…");
        if (last_progress_.room_created_ts_ms != 0)
        {
            label = tk::trf(tk::tr("{0} — back to {1}"),
                            {label, format_short_date(select_progress_display_ts(last_progress_))});
        }
        progress_bar_->set_label(label);
    }
    else if (last_progress_.oldest_ts_ms != 0)
    {
        // No message count claimed here while `events_written == 0` — the
        // window's authoritative count doesn't exist yet, and there is no
        // cheap, accurate way to approximate it mid-pagination (see
        // mod.rs's run_window: matrix-sdk-ui's live item count reflects
        // transient, not-yet-reconciled state while paginating, confirmed
        // to inflate 2x+ on a real room). The date alone is still honest
        // and live-updating, since it's the oldest *real* event seen so
        // far, unaffected by that transient inflation.
        progress_bar_->set_label(
            last_progress_.events_written != 0
                ? tk::trf(tk::tr("{0} messages — now at {1}"),
                         {std::to_string(last_progress_.events_written),
                          format_short_date(select_progress_display_ts(last_progress_))})
                : tk::trf(tk::tr("Reading messages — now at {0}"),
                         {format_short_date(select_progress_display_ts(last_progress_))}));
    }
    else
    {
        progress_bar_->set_label(tk::trf(
            tk::tr("{0} messages exported"), {std::to_string(last_progress_.events_written)}));
    }

    // set_progress()/set_label() above only mutate this widget's own
    // fields — neither schedules a repaint, so without this, a tick that
    // changes the displayed date/label but not the dialog's overall state
    // (unlike enter_in_progress_(), which goes through on_layout_changed())
    // updates data the screen never actually redraws, looking frozen
    // between the state transitions that do happen to trigger a repaint.
    if (auto* h = host())
        h->request_repaint();

    // This tick is real activity — arm the silence watchdog fresh. If
    // nothing else arrives within kSilenceWatchMs, the bar switches to its
    // self-animating indeterminate mode (see the member doc comment for
    // why: some steps behind a tick are single opaque async calls that can
    // run for several seconds with no way to report partial progress).
    const int gen = ++silence_watch_generation_;
    if (auto* h = host())
    {
        h->post_delayed(kSilenceWatchMs, [this, gen]()
        {
            if (gen != silence_watch_generation_)
                return; // a newer tick already arrived — not silent after all
            if (state_ != State::InProgress || !progress_bar_)
                return;
            progress_bar_->set_indeterminate();
            if (auto* h2 = host())
                h2->request_repaint();
        });
    }
}

void ExportHistoryDialog::show_finished(bool ok, bool cancelled, std::string out_path,
                                        std::uint64_t events_written, std::string error)
{
    // The finalizing->done tail can complete in well under 100ms
    // regardless of how long gathering took (real network round-trips can
    // make gathering itself run for many seconds) — measuring from
    // in_progress_started_at_ alone would mean that budget is long since
    // spent by the time the fast tail happens, so it's measured from
    // whichever is later: entering InProgress, or finalizing starting.
    const auto& baseline = finalizing_started_ ? finalizing_started_at_ : in_progress_started_at_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - baseline).count();
    if (state_ == State::InProgress && elapsed < kMinInProgressDurationMs)
    {
        const int remaining_ms = static_cast<int>(kMinInProgressDurationMs - elapsed);
        const int gen = finish_generation_;
        if (auto* h = host())
        {
            h->post_delayed(remaining_ms,
                [this, gen, ok, cancelled, out_path, events_written, error]() mutable
                {
                    if (gen != finish_generation_)
                        return; // a new export started in the meantime — this completion is stale
                    apply_finished_(ok, cancelled, std::move(out_path), events_written, std::move(error));
                });
            return;
        }
    }
    apply_finished_(ok, cancelled, std::move(out_path), events_written, std::move(error));
}

void ExportHistoryDialog::apply_finished_(bool ok, bool cancelled, std::string out_path,
                                          std::uint64_t events_written, std::string error)
{
    finished_ok_ = ok;
    finished_cancelled_ = cancelled;
    finished_out_path_ = std::move(out_path);
    finished_events_ = events_written;
    finished_error_ = std::move(error);
    state_ = State::Done;
    title_layout_.reset();
    body_layout_.reset();
    update_child_visibility_();
    if (open_ && on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::update_child_visibility_()
{
    const bool options = open_ && state_ == State::Options;
    const bool show_resume_choice = options && resume_available_;
    const bool show_format_form = options && !show_resume_choice;

    format_combo_->set_visible(show_format_form);
    include_images_btn_->set_visible(show_format_form);
    include_images_btn_->set_enabled(format_ == Format::Html);
    zip_output_btn_->set_visible(show_format_form);
    export_btn_->set_visible(show_format_form);

    resume_btn_->set_visible(show_resume_choice);
    start_new_btn_->set_visible(show_resume_choice);

    const bool in_progress = open_ && state_ == State::InProgress;
    progress_bar_->set_visible(in_progress);
    // Hidden once finalizing starts: cancelling no longer does anything
    // meaningful by then (the pagination walk that checks the cancel flag
    // has already finished; only local assembly remains).
    cancel_btn_->set_visible(in_progress && !last_progress_.finalizing);

    go_to_other_btn_->set_visible(open_ && state_ == State::BusyElsewhere);

    close_btn_->set_visible(open_ && state_ == State::Done);
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ExportHistoryDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void ExportHistoryDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);
    backdrop_rect_ = bounds;

    float card_h = kCardPad * 2 + kTitleH + kTitleGap;
    switch (state_)
    {
    case State::Options:
        if (resume_available_)
            card_h += kRowH * 2 + kRowGap + kBtnH;
        else
            card_h += kRowH * 4 + kRowGap * 3 + kBtnH;
        break;
    case State::InProgress:
        if (last_progress_.room_created_ts_ms != 0)
            card_h += kTitleGap + 20.0f /* "Exporting back to {date}" line */;
        card_h += 40.0f /* progress bar + caption */ + kRowGap + kBtnH;
        break;
    case State::BusyElsewhere:
        card_h += 48.0f /* body */ + kRowGap + kBtnH;
        break;
    case State::Done:
        card_h += 48.0f /* body */ + kRowGap + kBtnH;
        break;
    }

    const float card_w = std::min(kCardW, bounds.w);
    const float clamped_h = std::min(card_h, bounds.h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f,
                  bounds.y + (bounds.h - clamped_h) * 0.5f,
                  card_w, clamped_h};

    const float content_x = card_rect_.x + kCardPad;
    const float content_w = card_rect_.w - kCardPad * 2.0f;
    float y = card_rect_.y + kCardPad + kTitleH + kTitleGap;

    if (state_ == State::Options)
    {
        if (resume_available_)
        {
            y += kRowH * 2 + kRowGap; // body text rows drawn in paint()
            const float btn_w = (content_w - kRowGap) * 0.5f;
            resume_btn_->arrange(lc, {content_x, y, btn_w, kBtnH});
            start_new_btn_->arrange(lc, {content_x + btn_w + kRowGap, y, btn_w, kBtnH});
        }
        else
        {
            format_combo_->arrange(lc, {content_x, y, content_w, kRowH});
            y += kRowH + kRowGap;
            include_images_btn_->arrange(lc, {content_x, y, content_w, kRowH});
            y += kRowH + kRowGap;
            zip_output_btn_->arrange(lc, {content_x, y, content_w, kRowH});
            y += kRowH + kRowGap;
            const float btn_w = 120.0f;
            export_btn_->arrange(lc, {content_x + content_w - btn_w, y, btn_w, kBtnH});
        }
    }
    else if (state_ == State::InProgress)
    {
        if (last_progress_.room_created_ts_ms != 0)
            y += kTitleGap + 20.0f; // body text row drawn in paint()
        progress_bar_->arrange(lc, {content_x, y, content_w, 40.0f});
        y += 40.0f + kRowGap;
        const float btn_w = 120.0f;
        cancel_btn_->arrange(lc, {content_x + content_w - btn_w, y, btn_w, kBtnH});
    }
    else if (state_ == State::BusyElsewhere)
    {
        y += 48.0f + kRowGap;
        const float btn_w = 160.0f;
        go_to_other_btn_->arrange(lc, {content_x + content_w - btn_w, y, btn_w, kBtnH});
    }
    else if (state_ == State::Done)
    {
        y += 48.0f + kRowGap;
        const float btn_w = 100.0f;
        close_btn_->arrange(lc, {content_x + content_w - btn_w, y, btn_w, kBtnH});
    }
}

// ── paint ─────────────────────────────────────────────────────────────────

void ExportHistoryDialog::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);

    const float text_x = card_rect_.x + kCardPad;
    const float text_w = card_rect_.w - kCardPad * 2.0f;
    float y = card_rect_.y + kCardPad;

    std::string title;
    std::string body;
    switch (state_)
    {
    case State::Options:
        title = tk::trf(tk::tr("Export History of {0}"), {room_display_name_});
        if (resume_available_)
            body = tk::trf(tk::tr("A previous export of this room was interrupted after {0} messages. Resume it, or start over?"),
                          {std::to_string(checkpoint_.events_written)});
        break;
    case State::InProgress:
        title = tk::trf(tk::tr("Exporting {0}"), {room_display_name_});
        if (last_progress_.room_created_ts_ms != 0)
            body = tk::trf(tk::tr("Exporting back to {0}"), {format_short_date(last_progress_.room_created_ts_ms)});
        break;
    case State::BusyElsewhere:
        title = tk::tr("Export in progress");
        body = tk::trf(tk::tr("Currently exporting {0}."), {busy_room_display_name_});
        break;
    case State::Done:
        title = finished_cancelled_ ? tk::tr("Export cancelled")
                                    : (finished_ok_ ? tk::tr("Export complete") : tk::tr("Export failed"));
        body = finished_ok_ ? finished_out_path_
                            : (finished_error_.empty() ? std::string() : finished_error_);
        break;
    }

    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        title_layout_ = ctx.factory.build_text(title, st);
    }
    if (title_layout_)
    {
        cv.draw_text(*title_layout_, {text_x, y}, pal.text_primary);
        y += std::max(title_layout_->measure().h, kTitleH);
    }

    if (!body.empty())
    {
        y += kTitleGap;
        if (!body_layout_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            st.wrap = true;
            st.max_width = text_w;
            body_layout_ = ctx.factory.build_text(body, st);
        }
        if (body_layout_)
            cv.draw_text(*body_layout_, {text_x, y}, pal.text_secondary);
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool ExportHistoryDialog::on_pointer_down(tk::Point local)
{
    if (!open_) return false;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (rect_contains(card_rect_, w)) return false;
    press_backdrop_ = true;
    return true;
}

void ExportHistoryDialog::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_) return;
    press_backdrop_ = false;
    if (!inside_self) return;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(card_rect_, w))
    {
        // Backdrop dismiss never cancels an in-flight export — it just
        // hides the dialog, same as close().
        close();
    }
}

} // namespace tesseract::views
