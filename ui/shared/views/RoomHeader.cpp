#include "RoomHeader.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views {

namespace {

constexpr float kPadX       = 16.0f;
constexpr float kAvatarSize = 40.0f;
constexpr float kAvatarGap  = 12.0f;

constexpr float kCalBtnSize   = 28.0f;
constexpr float kCalBtnMargin =  8.0f;
constexpr float kCalBtnRadius =  6.0f;

// Vertical offsets for the name/topic block (from the top of the strip).
// Two-line layout (with topic): name at 12 px, topic at 34 px.
// One-line layout (name only):  name centred → 21 px.
constexpr float kNameY_Single =  21.0f;
constexpr float kNameH        =  18.0f;
constexpr float kNameY_Pair   =  12.0f;
constexpr float kTopicY       =  34.0f;
constexpr float kTopicH       =  14.0f;

} // namespace

RoomHeader::RoomHeader() {
    auto name = std::make_unique<tk::Label>("", tk::FontRole::Title);
    name->set_trim(tk::TextTrim::Ellipsis);
    name_label_ = add_child(std::move(name));

    auto topic = std::make_unique<tk::Label>("", tk::FontRole::SidebarPreview);
    topic->set_trim(tk::TextTrim::Ellipsis);
    topic_label_ = add_child(std::move(topic));
    topic_label_->set_visible(false);
}

void RoomHeader::set_room(const tesseract::RoomInfo& info) {
    display_name_ = info.name;
    topic_        = info.topic;
    topic_html_   = info.topic_html;
    avatar_url_   = info.avatar_url;
    if (name_label_)  name_label_->set_text(display_name_);
    if (topic_label_) {
        if (!topic_html_.empty()) {
            topic_spans_ = html_to_spans(topic_html_);
            topic_label_->set_visible(false);
        } else {
            topic_spans_.clear();
            topic_label_->set_text(topic_);
            topic_label_->set_visible(!topic_.empty());
        }
    }
}

void RoomHeader::set_avatar_provider(
    std::function<const tk::Image*(const std::string& mxc_url)> provider)
{
    avatar_provider_ = std::move(provider);
}

tk::Size RoomHeader::measure(tk::LayoutCtx&, tk::Size constraints) {
    return { constraints.w, kHeight };
}

void RoomHeader::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    const float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
    // Reserve space on the right for the calendar button.
    const float right_reserve = kCalBtnMargin + kCalBtnSize + kCalBtnMargin;
    const float text_w = std::max(0.0f,
        bounds.w - kPadX - kAvatarSize - kAvatarGap - right_reserve);

    const bool has_topic = !topic_.empty() || !topic_html_.empty();

    const float name_y = has_topic
        ? bounds.y + kNameY_Pair
        : bounds.y + kNameY_Single;

    if (name_label_) {
        name_label_->arrange(ctx, { text_x, name_y, text_w, kNameH });
    }

    topic_rect_ = { text_x, bounds.y + kTopicY, text_w, kTopicH };

    if (topic_label_) {
        const bool show_label = has_topic && topic_html_.empty();
        topic_label_->set_visible(show_label);
        if (show_label) {
            topic_label_->arrange(ctx, topic_rect_);
        }
    }
}

void RoomHeader::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.chrome_bg);

    // 1 px bottom hairline separator.
    tk::Rect sep{
        bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f
    };
    ctx.canvas.fill_rect(sep, ctx.theme.palette.separator);

    // Avatar — circle-cropped image or initials disc.
    const tk::Point avatar_centre{
        bounds_.x + kPadX + kAvatarSize * 0.5f,
        bounds_.y + kHeight * 0.5f
    };
    if (avatar_provider_ && !avatar_url_.empty()) {
        if (const tk::Image* img = avatar_provider_(avatar_url_)) {
            ctx.canvas.draw_circle_image(*img, avatar_centre, kAvatarSize);
        } else {
            ctx.canvas.draw_initials_circle(
                display_name_, avatar_centre, kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }
    } else {
        ctx.canvas.draw_initials_circle(
            display_name_, avatar_centre, kAvatarSize,
            ctx.theme.palette.avatar_initials_bg,
            ctx.theme.palette.avatar_initials_text);
    }

    if (name_label_)  name_label_->paint(ctx);

    // Topic: rich text (HTML) if present, plain label otherwise.
    if (!topic_spans_.empty()) {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::SidebarPreview;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = topic_rect_.w;
        topic_layout_ = ctx.factory.build_rich_text(topic_spans_, ts);
        if (topic_layout_) {
            ctx.canvas.draw_text(*topic_layout_,
                { topic_rect_.x, topic_rect_.y },
                ctx.theme.palette.text_primary);
        }
    } else if (topic_label_ && topic_label_->visible()) {
        topic_label_->paint(ctx);
    }

    // Calendar / jump-to-date button — 28×28 rounded-rect at the right edge.
    calendar_btn_rect_ = {
        bounds_.x + bounds_.w - kCalBtnMargin - kCalBtnSize,
        bounds_.y + (kHeight - kCalBtnSize) * 0.5f,
        kCalBtnSize,
        kCalBtnSize
    };

    const tk::Color btn_bg = press_calendar_
        ? ctx.theme.palette.subtle_pressed
        : hover_calendar_
            ? ctx.theme.palette.subtle_hover
            : ctx.theme.palette.chrome_bg;  // blends with header when idle

    ctx.canvas.fill_rounded_rect(calendar_btn_rect_, kCalBtnRadius, btn_bg);

    // Draw a calendar glyph centred inside the button.
    tk::TextStyle cal_ts;
    cal_ts.role   = tk::FontRole::Body;
    cal_ts.halign = tk::TextHAlign::Center;
    cal_ts.valign = tk::TextVAlign::Center;
    auto glyph = ctx.factory.build_text("\U0001F4C5", cal_ts);
    if (glyph) {
        const tk::Size gs = glyph->measure();
        const tk::Point glyph_origin{
            calendar_btn_rect_.x + (kCalBtnSize - gs.w) * 0.5f,
            calendar_btn_rect_.y + (kCalBtnSize - gs.h) * 0.5f
        };
        ctx.canvas.draw_text(*glyph, glyph_origin, ctx.theme.palette.text_primary);
    }
}

bool RoomHeader::on_pointer_down(tk::Point local) {
    // calendar_btn_rect_ is in world coords; reconstruct the world point
    // from the widget-local `local` by adding the widget's own origin.
    const tk::Point world{ bounds_.x + local.x, bounds_.y + local.y };
    if (world.x >= calendar_btn_rect_.x &&
        world.x <  calendar_btn_rect_.x + calendar_btn_rect_.w &&
        world.y >= calendar_btn_rect_.y &&
        world.y <  calendar_btn_rect_.y + calendar_btn_rect_.h)
    {
        press_calendar_ = true;
        return true;
    }
    return false;
}

void RoomHeader::on_pointer_up(tk::Point local, bool inside_self) {
    // Calendar button takes priority: if it captured the press, handle it first.
    if (press_calendar_) {
        press_calendar_ = false;
        if (inside_self) {
            const tk::Point world{ bounds_.x + local.x, bounds_.y + local.y };
            if (world.x >= calendar_btn_rect_.x &&
                world.x <  calendar_btn_rect_.x + calendar_btn_rect_.w &&
                world.y >= calendar_btn_rect_.y &&
                world.y <  calendar_btn_rect_.y + calendar_btn_rect_.h)
            {
                if (on_jump_to_date_requested) on_jump_to_date_requested();
            }
        }
        return;
    }

    // Topic hyperlink click.
    if (!inside_self || !topic_layout_) return;
    tk::Point ll{ local.x - topic_rect_.x, local.y - topic_rect_.y };
    std::string url = topic_layout_->link_at(ll);
    if (!url.empty() && on_link_clicked) on_link_clicked(url);
}

void RoomHeader::on_pointer_move(tk::Point local) {
    const tk::Point world{ bounds_.x + local.x, bounds_.y + local.y };
    hover_calendar_ =
        world.x >= calendar_btn_rect_.x &&
        world.x <  calendar_btn_rect_.x + calendar_btn_rect_.w &&
        world.y >= calendar_btn_rect_.y &&
        world.y <  calendar_btn_rect_.y + calendar_btn_rect_.h;
    // Host calls request_repaint() after dispatching pointer-move.
}

void RoomHeader::on_pointer_leave() {
    hover_calendar_ = false;
    press_calendar_ = false;
    // Host calls request_repaint() after dispatching pointer-leave.
}

} // namespace tesseract::views
