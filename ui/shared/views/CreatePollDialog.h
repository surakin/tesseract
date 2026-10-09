#pragma once

// Cross-platform modal "Create poll" overlay (MSC3381). Mounted at
// MainAppWidget level like ConfirmDialog / ExportHistoryDialog: closed by
// default, visibility tied to open state. Opened by the /poll slash command;
// ShellBase binds `on_create` at open time (see open_create_poll_dialog_).

#include "poll_logic.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/scroll_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class CreatePollDialog : public tk::Widget
{
public:
    CreatePollDialog();
    ~CreatePollDialog() override = default;

    // Opens empty. `on_create` receives a NORMALIZED, VALID draft.
    // `room_name` (the target room's display name) is shown in the title; empty
    // keeps the plain "Create poll" title.
    void open(std::function<void(PollDraft)> on_create, std::string room_name = {});
    void close();
    bool is_open() const { return open_; }

    // Same contract as ConfirmDialog::on_layout_changed.
    std::function<void()> on_layout_changed;

    // The draft as typed (un-normalized).
    PollDraft current_draft() const;
    // Whether Create would currently succeed.
    bool create_enabled() const;

    // Draft mutation. add_option is capped at kPollMaxOptions; remove_option
    // never drops below kPollMinOptions.
    void add_option();
    void remove_option(std::size_t index);
    // What pressing Enter in option `index` does (adds/focuses the next row).
    void enter_in_option(std::size_t index)
    {
        if (index < rows_.size() && rows_[index].field)
            on_option_submit_(rows_[index].field);
    }
    std::size_t option_count() const { return rows_.size(); }

    // Child widgets (null without a Host for the text fields).
    tk::ScrollView* options_scroll() const { return scroll_; }
    tk::TextField* question_field() const { return question_; }
    tk::TextField* option_field(std::size_t i) const
    {
        return i < rows_.size() ? rows_[i].field : nullptr;
    }
    tk::CheckButton* multiple_check() const { return multiple_; }
    tk::CheckButton* hide_results_check() const { return hide_results_; }
    tk::Button* create_button() const { return create_btn_; }
    tk::Button* cancel_button() const { return cancel_btn_; }

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_wheel(tk::Point, float, float, bool) override { return open_; }
    void     on_theme_changed(const tk::Theme& t) override;

    tk::Role access_role() const override { return tk::Role::Dialog; }
    std::string access_name() const override { return title_text_(); }
    bool access_modal() const override { return open_ && visible(); }

private:
    struct Row
    {
        tk::TextField* field  = nullptr;
        tk::Button*    remove = nullptr;
    };
    class OptionsColumn; // defined in the .cpp

    void add_row_();
    void relabel_rows_();
    void refresh_state_();
    void submit_();
    void on_option_submit_(tk::TextField* from);

    bool open_ = false;
    // Row to scroll into view at the next arrange() (npos = none).
    std::size_t reveal_row_ = static_cast<std::size_t>(-1);
    std::function<void(PollDraft)> on_create_;

    tk::TextField*   question_     = nullptr;
    tk::ScrollView*  scroll_       = nullptr;
    OptionsColumn*   column_       = nullptr;
    tk::Button*      add_btn_      = nullptr;
    tk::CheckButton* multiple_     = nullptr;
    tk::CheckButton* hide_results_ = nullptr;
    tk::Label*       hint_         = nullptr;
    tk::Button*      create_btn_   = nullptr;
    tk::Button*      cancel_btn_   = nullptr;
    std::vector<Row> rows_;

    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};
    std::string room_name_;
    std::string title_text_() const
    {
        return room_name_.empty()
                   ? tk::tr("Create poll")
                   : tk::trf(tk::tr("Create poll in {0}"), {room_name_});
    }
    std::unique_ptr<tk::TextLayout> title_layout_;
    float title_w_ = -1.0f;
    bool press_backdrop_ = false;

    static constexpr float kCardW   = 420.0f;
    static constexpr float kMargin  = 24.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kBtnH    = 36.0f;
    static constexpr float kBtnGap  = 8.0f;
    static constexpr float kTitleH  = 22.0f;
    static constexpr float kGap     = 12.0f;
    static constexpr float kFieldH  = 32.0f;
    static constexpr float kCheckH  = 28.0f;
    static constexpr float kHintH   = 20.0f;
};

} // namespace tesseract::views
