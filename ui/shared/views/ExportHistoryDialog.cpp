#include "ExportHistoryDialog.h"

#include "media_utils.h" // rect_contains
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
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
    update_child_visibility_();
    if (on_layout_changed) on_layout_changed();
}

void ExportHistoryDialog::show_progress(const tesseract::RoomExportProgress& progress)
{
    last_progress_ = progress;
    if (state_ == State::Options)
        enter_in_progress_();
    update_child_visibility_(); // e.g. hides Cancel once finalizing starts
    if (progress_bar_)
    {
        if (last_progress_.finalizing)
        {
            // Local assembly (concatenating segments, then zipping if
            // requested) has no meaningful fraction of its own — just
            // show that it's happening instead of freezing on the last
            // pagination-round tick for however long it takes.
            progress_bar_->set_indeterminate();
            progress_bar_->set_label(tk::tr("Finalizing export…"));
        }
        else if (last_progress_.room_created_ts_ms != 0 && last_progress_.newest_ts_ms > last_progress_.room_created_ts_ms)
        {
            const auto span = static_cast<float>(last_progress_.newest_ts_ms - last_progress_.room_created_ts_ms);
            const auto done = static_cast<float>(last_progress_.newest_ts_ms - last_progress_.oldest_ts_ms);
            progress_bar_->set_progress(span > 0.0f ? done / span : 0.0f);
            progress_bar_->set_label(tk::trf(
                tk::tr("{0} messages exported"), {std::to_string(last_progress_.events_written)}));
        }
        else
        {
            progress_bar_->set_indeterminate();
            progress_bar_->set_label(tk::trf(
                tk::tr("{0} messages exported"), {std::to_string(last_progress_.events_written)}));
        }
    }
}

void ExportHistoryDialog::show_finished(bool ok, bool cancelled, std::string out_path,
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
