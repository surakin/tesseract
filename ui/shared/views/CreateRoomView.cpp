#include "CreateRoomView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tesseract::views
{

namespace
{

constexpr float kCRPadX = 20.0f;
constexpr float kCRPadY = 16.0f;
constexpr float kCRGap = 10.0f;
constexpr float kCRSmallGap = 6.0f;
constexpr float kCRTitleH = 28.0f;
constexpr float kCRFieldH = 32.0f;
constexpr float kCRTopicH = 60.0f;
constexpr float kCRInviteH = 50.0f;
constexpr float kCRHintH = 16.0f;
constexpr float kCRBtnH = 32.0f;
constexpr float kCRBtnW = 96.0f;
constexpr float kCRStatusH = 20.0f;
constexpr float kCRRadius = tesseract::visual::kRadiusSM;
constexpr float kCRBorderW = 1.0f;

// Splits `text` on commas/newlines, trims whitespace, and drops empties —
// used to turn the free-text invite field into a Matrix ID list. Malformed
// IDs aren't validated here; they surface via the existing
// Result::message / on_create_room_outcome_ui_ error path.
std::vector<std::string> split_invitees(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]
    {
        size_t begin = cur.find_first_not_of(" \t\r\n");
        size_t end = cur.find_last_not_of(" \t\r\n");
        if (begin != std::string::npos)
            out.push_back(cur.substr(begin, end - begin + 1));
        cur.clear();
    };
    for (char c : text)
    {
        if (c == ',' || c == '\n')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

} // namespace

CreateRoomView::CreateRoomView()
{
    // host() is nullable: when null (e.g. unit tests constructing this
    // detached, or under a null-host MainAppWidget in tests), the native
    // fields are skipped — they stay null, mirroring
    // ForwardRoomPicker::search_field_'s identical rationale.
    if (host())
    {
        auto name = tk::create_widget<tk::TextField>(this, kCRFieldH);
        name->set_placeholder(tk::tr("Room name"));
        name_field_ = add_child(std::move(name));

        auto topic = tk::create_widget<tk::TextArea>(this, kCRTopicH);
        topic->set_placeholder(tk::tr("Topic (optional)"));
        topic_field_ = add_child(std::move(topic));

        auto alias = tk::create_widget<tk::TextField>(this, kCRFieldH);
        alias->set_placeholder(tk::tr("Room alias (optional)"));
        alias_field_ = add_child(std::move(alias));

        auto invite = tk::create_widget<tk::TextArea>(this, kCRInviteH);
        invite->set_placeholder(tk::tr("Invite people (optional) — Matrix IDs, one per line or comma-separated"));
        invite_field_ = add_child(std::move(invite));

        auto reason = tk::create_widget<tk::TextField>(this, kCRFieldH);
        reason->set_placeholder(tk::tr("Reason (optional)"));
        reason_field_ = add_child(std::move(reason));
    }

    auto reason_hint = tk::create_widget<tk::Label>(
        this, tk::tr("Sent as plain text, even in encrypted rooms"), tk::FontRole::Caption);
    reason_hint_lbl_ = add_child(std::move(reason_hint));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Private"), "private"},
        {tk::tr("Public"), "public"},
    });
    combo->set_selected_value("private");
    visibility_combo_ = add_child(std::move(combo));

    auto enc = tk::create_widget<tk::CheckButton>(this, tk::tr("Encrypt this room"));
    encryption_check_ = add_child(std::move(enc));

    auto create = tk::create_widget<tk::Button>(this, tk::tr("Create"),
        std::function<void()>{}, tk::Button::Variant::Primary);
    create->set_on_click(
        [this]
        {
            if (on_create_requested)
            {
                on_create_requested(build_options_());
            }
        });
    create_btn_ = add_child(std::move(create));

    auto cancel = tk::create_widget<tk::Button>(this, tk::tr("Cancel"),
        std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_on_click(
        [this]
        {
            if (on_cancel)
            {
                on_cancel();
            }
        });
    cancel_btn_ = add_child(std::move(cancel));

    auto status = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    status->set_halign(tk::TextHAlign::Center);
    status->set_trim(tk::TextTrim::Ellipsis);
    status_lbl_ = add_child(std::move(status));

    apply_state();
}

tesseract::RoomCreateOptions CreateRoomView::build_options_() const
{
    tesseract::RoomCreateOptions o;
    o.name = name_field_ ? name_field_->text() : std::string();
    o.topic = topic_field_ ? topic_field_->text() : std::string();
    o.room_alias_local_part = alias_field_ ? alias_field_->text() : std::string();
    o.visibility = visibility_combo_ && !visibility_combo_->selected_value().empty()
                       ? visibility_combo_->selected_value()
                       : std::string("private");
    o.encrypted = encryption_check_ && encryption_check_->checked();
    o.invite = invite_field_ ? split_invitees(invite_field_->text()) : std::vector<std::string>();
    o.invite_reason = reason_field_ ? reason_field_->text() : std::string();
    return o;
}

void CreateRoomView::set_state(State s)
{
    state_ = s;
    if (s != State::Error)
    {
        error_msg_.clear();
    }
    apply_state();
}

void CreateRoomView::set_error(std::string msg)
{
    error_msg_ = std::move(msg);
    state_ = State::Error;
    apply_state();
}

void CreateRoomView::reset()
{
    if (name_field_) name_field_->set_text("");
    if (topic_field_) topic_field_->set_text("");
    if (alias_field_) alias_field_->set_text("");
    if (invite_field_) invite_field_->set_text("");
    if (reason_field_) reason_field_->set_text("");
    if (visibility_combo_) visibility_combo_->set_selected_value("private");
    if (encryption_check_) encryption_check_->set_checked(false);
    set_state(State::Idle);
}

void CreateRoomView::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (name_field_) name_field_->set_visible(v);
    if (topic_field_) topic_field_->set_visible(v);
    if (alias_field_) alias_field_->set_visible(v);
    if (invite_field_) invite_field_->set_visible(v);
    if (reason_field_) reason_field_->set_visible(v);
}

void CreateRoomView::focus_name_field()
{
    if (name_field_)
        name_field_->set_focused(true);
}

void CreateRoomView::on_theme_changed(const tk::Theme& t)
{
    if (name_field_) name_field_->set_text_color(t.palette.text_primary);
    if (topic_field_) topic_field_->set_text_color(t.palette.text_primary);
    if (alias_field_) alias_field_->set_text_color(t.palette.text_primary);
    if (invite_field_) invite_field_->set_text_color(t.palette.text_primary);
    if (reason_field_) reason_field_->set_text_color(t.palette.text_primary);
    if (reason_hint_lbl_) reason_hint_lbl_->set_colour(t.palette.text_muted);
}

void CreateRoomView::apply_state()
{
    const bool creating = (state_ == State::Creating);
    const bool show_status = (state_ == State::Creating || state_ == State::Error);

    if (name_field_) name_field_->set_enabled(!creating);
    if (topic_field_) topic_field_->set_enabled(!creating);
    if (alias_field_) alias_field_->set_enabled(!creating);
    if (invite_field_) invite_field_->set_enabled(!creating);
    if (reason_field_) reason_field_->set_enabled(!creating);
    if (visibility_combo_) visibility_combo_->set_enabled(!creating);
    if (encryption_check_) encryption_check_->set_enabled(!creating);
    if (create_btn_) create_btn_->set_enabled(!creating);

    if (status_lbl_)
    {
        status_lbl_->set_visible(show_status);
        if (state_ == State::Creating)
        {
            status_lbl_->set_text(tk::tr("Creating room\xe2\x80\xa6"));
        }
        else if (state_ == State::Error)
        {
            status_lbl_->set_text(error_msg_.empty() ? tk::tr("Couldn't create room.")
                                                       : error_msg_);
        }
        else
        {
            status_lbl_->set_text("");
        }
    }
}

tk::Size CreateRoomView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, constraints.h};
}

void CreateRoomView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    float x = bounds.x + kCRPadX;
    float y = bounds.y + kCRPadY;
    float inner_w = bounds.w - kCRPadX * 2.0f;

    if (title_visible_)
        y += kCRTitleH + kCRGap;

    if (name_field_)
    {
        name_field_->arrange(ctx, {x, y, inner_w, kCRFieldH});
    }
    y += kCRFieldH + kCRGap;

    if (topic_field_)
    {
        topic_field_->arrange(ctx, {x, y, inner_w, kCRTopicH});
    }
    y += kCRTopicH + kCRGap;

    if (alias_field_)
    {
        alias_field_->arrange(ctx, {x, y, inner_w, kCRFieldH});
    }
    y += kCRFieldH + kCRGap;

    if (invite_field_)
    {
        invite_field_->arrange(ctx, {x, y, inner_w, kCRInviteH});
    }
    y += kCRInviteH + kCRGap;

    if (reason_field_)
    {
        reason_field_->arrange(ctx, {x, y, inner_w, kCRFieldH});
    }
    y += kCRFieldH + kCRSmallGap;

    if (reason_hint_lbl_)
    {
        reason_hint_lbl_->arrange(ctx, {x, y, inner_w, kCRHintH});
    }
    y += kCRHintH + kCRGap;

    // Visibility combo (left half) + encryption checkbox (right half).
    float half_w = (inner_w - kCRGap) * 0.5f;
    if (visibility_combo_)
    {
        visibility_combo_->arrange(ctx, {x, y, half_w, kCRFieldH});
    }
    if (encryption_check_)
    {
        encryption_check_->arrange(ctx, {x + half_w + kCRGap, y, half_w, kCRFieldH});
    }
    y += kCRFieldH + kCRGap;

    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->arrange(ctx, {x, y, inner_w, kCRStatusH});
        y += kCRStatusH + kCRSmallGap;
    }

    float btn_row_y = std::max(y, bounds.y + bounds.h - kCRPadY - kCRBtnH);
    float btn_x = x + inner_w;

    if (create_btn_)
    {
        btn_x -= kCRBtnW;
        create_btn_->arrange(ctx, {btn_x, btn_row_y, kCRBtnW, kCRBtnH});
        btn_x -= kCRSmallGap;
    }
    if (cancel_btn_)
    {
        btn_x -= kCRBtnW;
        cancel_btn_->arrange(ctx, {btn_x, btn_row_y, kCRBtnW, kCRBtnH});
    }
}

void CreateRoomView::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    float x = bounds_.x + kCRPadX;
    float y = bounds_.y + kCRPadY;

    if (title_visible_)
    {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Title;
        ts.halign = tk::TextHAlign::Leading;
        ts.trim = tk::TextTrim::Ellipsis;
        auto lo = ctx.factory.build_text(tk::tr("Create a Room"), ts);
        if (lo)
        {
            ctx.canvas.draw_text(*lo, {x, y}, pal.text_primary);
        }
        y += kCRTitleH + kCRGap;
    }

    auto draw_field_bg = [&](tk::Rect r)
    {
        if (r.empty())
            return;
        ctx.canvas.fill_rounded_rect(r, kCRRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(r, kCRRadius, pal.border, kCRBorderW);
    };

    if (name_field_ && name_field_->visible())
    {
        draw_field_bg(name_field_->bounds());
        name_field_->paint(ctx);
    }
    y += kCRFieldH + kCRGap;

    if (topic_field_ && topic_field_->visible())
    {
        draw_field_bg(topic_field_->bounds());
        topic_field_->paint(ctx);
    }
    y += kCRTopicH + kCRGap;

    if (alias_field_ && alias_field_->visible())
    {
        draw_field_bg(alias_field_->bounds());
        alias_field_->paint(ctx);
    }
    y += kCRFieldH + kCRGap;

    if (invite_field_ && invite_field_->visible())
    {
        draw_field_bg(invite_field_->bounds());
        invite_field_->paint(ctx);
    }
    y += kCRInviteH + kCRGap;

    if (reason_field_ && reason_field_->visible())
    {
        draw_field_bg(reason_field_->bounds());
        reason_field_->paint(ctx);
    }
    y += kCRFieldH + kCRSmallGap;

    if (reason_hint_lbl_ && reason_hint_lbl_->visible())
    {
        reason_hint_lbl_->paint(ctx);
    }
    y += kCRHintH + kCRGap;

    y += kCRFieldH + kCRGap; // visibility combo / encryption checkbox row

    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->set_colour(state_ == State::Error
                                     ? std::optional<tk::Color>(pal.destructive)
                                     : std::nullopt);
        status_lbl_->paint(ctx);
        y += kCRStatusH + kCRSmallGap;
    }

    if (visibility_combo_)
        visibility_combo_->paint(ctx);
    if (encryption_check_)
        encryption_check_->paint(ctx);
    if (create_btn_)
        create_btn_->paint(ctx);
    if (cancel_btn_)
        cancel_btn_->paint(ctx);
}

} // namespace tesseract::views
