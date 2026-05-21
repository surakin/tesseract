#include "RoomHeader.h"

#include "html_spans.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tesseract::views
{

namespace
{

constexpr float kPadX = 16.0f;
constexpr float kAvatarSize = 40.0f;
constexpr float kAvatarGap = 12.0f;

constexpr float kCalBtnSize = 28.0f;
constexpr float kCalBtnMargin = 8.0f;
constexpr float kCalBtnRadius = 6.0f;

// Vertical offsets for the name/topic block (from the top of the strip).
// Two-line layout (with topic): name at 12 px, topic at 34 px.
// One-line layout (name only):  name centred → 21 px.
constexpr float kNameY_Single = 21.0f;
constexpr float kNameH = 18.0f;
constexpr float kNameY_Pair = 12.0f;
constexpr float kTopicY = 34.0f;
constexpr float kTopicH = 14.0f;

} // namespace

RoomHeader::RoomHeader()
{
    auto name = std::make_unique<tk::Label>("", tk::FontRole::Title);
    name->set_trim(tk::TextTrim::Ellipsis);
    name_label_ = add_child(std::move(name));

    auto topic = std::make_unique<tk::Label>("", tk::FontRole::SidebarPreview);
    topic->set_trim(tk::TextTrim::Ellipsis);
    topic_label_ = add_child(std::move(topic));
    topic_label_->set_visible(false);
}

void RoomHeader::set_room(const tesseract::RoomInfo& info)
{
    display_name_ = info.name;
    topic_ = info.topic;
    topic_html_ = info.topic_html;
    avatar_url_ = info.effective_avatar_url();
    // Drop the previous room's rich-topic layout; mark dirty so arrange()
    // rebuilds it and recomputes truncation before the next paint.
    topic_layout_.reset();
    topic_dirty_ = true;
    topic_truncated_ = false;
    topic_multiline_ = false;
    if (name_label_)
    {
        name_label_->set_text(display_name_);
    }
    if (topic_label_)
    {
        if (!topic_html_.empty())
        {
            topic_spans_ = html_to_spans(topic_html_);
            // Build first-line display spans: scan for \n, stop there.
            topic_display_spans_ = {};
            for (const auto& span : topic_spans_)
            {
                const auto nl = span.text.find('\n');
                if (nl != std::string::npos)
                {
                    topic_multiline_ = true;
                    tk::TextSpan trunc = span;
                    trunc.text = span.text.substr(0, nl) + "…";
                    topic_display_spans_.push_back(std::move(trunc));
                    break;
                }
                topic_display_spans_.push_back(span);
            }
            if (!topic_multiline_)
            {
                topic_display_spans_ = topic_spans_;
            }
            topic_label_->set_visible(false);
        }
        else
        {
            topic_spans_.clear();
            topic_display_spans_.clear();
            const auto nl = topic_.find('\n');
            topic_multiline_ = (nl != std::string::npos);
            const std::string display =
                topic_multiline_ ? topic_.substr(0, nl) + "…" : topic_;
            topic_label_->set_text(display);
            topic_label_->set_visible(!topic_.empty());
        }
    }
}

void RoomHeader::set_avatar_provider(
    std::function<const tk::Image*(const std::string& mxc_url)> provider)
{
    avatar_provider_ = std::move(provider);
}

void RoomHeader::set_condensed(bool condensed)
{
    condensed_ = condensed;
}

tk::Size RoomHeader::measure(tk::LayoutCtx&, tk::Size constraints)
{
    if (!condensed_)
    {
        return {constraints.w, kHeight};
    }
    const bool has_topic = !topic_.empty() || !topic_html_.empty();
    if (!has_topic)
    {
        return {constraints.w, 0.0f};
    }
    // Fit the text plus minimal vertical padding.
    return {constraints.w, kTopicH + 8.0f};
}

void RoomHeader::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    if (condensed_ && bounds.h <= 0.0f)
    {
        if (name_label_)
        {
            name_label_->set_visible(false);
        }
        if (topic_label_)
        {
            topic_label_->set_visible(false);
        }
        return;
    }

    if (condensed_)
    {
        // Condensed: topic centred vertically across the full width,
        // no avatar, no name label, no calendar button.
        if (name_label_)
        {
            name_label_->set_visible(false);
        }

        const float topic_y = bounds.y + (bounds.h - kTopicH) * 0.5f;
        topic_rect_ = {bounds.x + kPadX, topic_y,
                       std::max(0.0f, bounds.w - 2.0f * kPadX), kTopicH};

        const bool has_topic = !topic_.empty() || !topic_html_.empty();
        const bool show_label = has_topic && topic_html_.empty();
        if (topic_label_)
        {
            topic_label_->set_visible(show_label);
            if (show_label)
            {
                topic_label_->arrange(ctx, topic_rect_);
            }
        }

        const bool needs_rebuild =
            topic_dirty_ || (topic_rect_.w != last_topic_w_);
        if (needs_rebuild)
        {
            topic_dirty_ = false;
            last_topic_w_ = topic_rect_.w;
            topic_truncated_ = false;
            topic_layout_.reset();

            if (!topic_display_spans_.empty())
            {
                tk::TextStyle ts{};
                ts.role = tk::FontRole::SidebarPreview;
                ts.trim = tk::TextTrim::Ellipsis;
                ts.max_width = topic_rect_.w;
                topic_layout_ =
                    ctx.factory.build_rich_text(topic_display_spans_, ts);
                if (topic_multiline_)
                {
                    topic_truncated_ = true;
                }
                else
                {
                    tk::TextStyle ts_nat{};
                    ts_nat.role = tk::FontRole::SidebarPreview;
                    auto nat = ctx.factory.build_rich_text(topic_display_spans_,
                                                           ts_nat);
                    topic_truncated_ = nat && nat->measure().w > topic_rect_.w;
                }
            }
            else if (!topic_.empty())
            {
                if (topic_multiline_)
                {
                    topic_truncated_ = true;
                }
                else
                {
                    tk::TextStyle ts_nat{};
                    ts_nat.role = tk::FontRole::SidebarPreview;
                    auto nat = ctx.factory.build_text(topic_, ts_nat);
                    topic_truncated_ = nat && nat->measure().w > topic_rect_.w;
                }
            }
        }
        return;
    }

    const float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
    const float right_reserve = show_calendar_btn_
                                    ? kCalBtnMargin + kCalBtnSize + kCalBtnMargin
                                    : 0.0f;
    const float text_w = std::max(0.0f, bounds.w - kPadX - kAvatarSize -
                                            kAvatarGap - right_reserve);

    const bool has_topic = !topic_.empty() || !topic_html_.empty();
    const bool show_label = has_topic && topic_html_.empty();

    const float name_y =
        has_topic ? bounds.y + kNameY_Pair : bounds.y + kNameY_Single;

    if (name_label_)
    {
        name_label_->arrange(ctx, {text_x, name_y, text_w, kNameH});
    }

    topic_rect_ = {text_x, bounds.y + kTopicY, text_w, kTopicH};

    if (topic_label_)
    {
        topic_label_->set_visible(show_label);
        if (show_label)
        {
            topic_label_->arrange(ctx, topic_rect_);
        }
    }

    // Rebuild topic layout and recompute truncation when the topic content
    // changed (set_room() sets topic_dirty_) or the available width changed.
    const bool needs_rebuild = topic_dirty_ || (text_w != last_topic_w_);
    if (needs_rebuild)
    {
        topic_dirty_ = false;
        last_topic_w_ = text_w;
        topic_truncated_ = false;
        topic_layout_.reset();

        if (!topic_display_spans_.empty())
        {
            // Rich text: paint from display spans (first-line only when
            // multiline; equals topic_spans_ for single-line topics).
            tk::TextStyle ts{};
            ts.role = tk::FontRole::SidebarPreview;
            ts.trim = tk::TextTrim::Ellipsis;
            ts.max_width = text_w;
            topic_layout_ =
                ctx.factory.build_rich_text(topic_display_spans_, ts);

            if (topic_multiline_)
            {
                topic_truncated_ = true;
            }
            else
            {
                tk::TextStyle ts_nat{};
                ts_nat.role = tk::FontRole::SidebarPreview;
                auto nat =
                    ctx.factory.build_rich_text(topic_display_spans_, ts_nat);
                topic_truncated_ = nat && nat->measure().w > text_w;
            }
        }
        else if (!topic_.empty())
        {
            // Plain text: label already shows first-line + "…" when multiline.
            if (topic_multiline_)
            {
                topic_truncated_ = true;
            }
            else
            {
                tk::TextStyle ts_nat{};
                ts_nat.role = tk::FontRole::SidebarPreview;
                auto nat = ctx.factory.build_text(topic_, ts_nat);
                topic_truncated_ = nat && nat->measure().w > text_w;
            }
        }
    }
}

void RoomHeader::paint(tk::PaintCtx& ctx)
{
    if (bounds_.h <= 0.0f)
    {
        return;
    }

    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.chrome_bg);

    // 1 px bottom hairline separator.
    tk::Rect sep{bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f};
    ctx.canvas.fill_rect(sep, ctx.theme.palette.separator);

    if (condensed_)
    {
        // Condensed: only topic, no avatar / name / calendar.
        if (!topic_spans_.empty())
        {
            if (topic_layout_)
            {
                ctx.canvas.draw_text(*topic_layout_,
                                     {topic_rect_.x, topic_rect_.y},
                                     ctx.theme.palette.text_primary);
            }
        }
        else if (topic_label_ && topic_label_->visible())
        {
            topic_label_->paint(ctx);
        }
        return;
    }

    // Avatar — circle-cropped image or initials disc.
    const tk::Point avatar_centre{bounds_.x + kPadX + kAvatarSize * 0.5f,
                                  bounds_.y + kHeight * 0.5f};
    if (avatar_provider_ && !avatar_url_.empty())
    {
        if (const tk::Image* img = avatar_provider_(avatar_url_))
        {
            ctx.canvas.draw_circle_image(*img, avatar_centre, kAvatarSize);
        }
        else
        {
            ctx.canvas.draw_initials_circle(
                display_name_, avatar_centre, kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }
    }
    else
    {
        ctx.canvas.draw_initials_circle(display_name_, avatar_centre,
                                        kAvatarSize,
                                        ctx.theme.palette.avatar_initials_bg,
                                        ctx.theme.palette.avatar_initials_text);
    }

    if (name_label_)
    {
        name_label_->paint(ctx);
    }

    // Topic: rich text (HTML) if present, plain label otherwise.
    // topic_layout_ is built and cached in arrange(); no rebuild needed here.
    if (!topic_spans_.empty())
    {
        if (topic_layout_)
        {
            ctx.canvas.draw_text(*topic_layout_, {topic_rect_.x, topic_rect_.y},
                                 ctx.theme.palette.text_primary);
        }
    }
    else if (topic_label_ && topic_label_->visible())
    {
        topic_label_->paint(ctx);
    }

    // Calendar / jump-to-date button — only shown when MSC3030 is supported.
    if (show_calendar_btn_)
    {
        calendar_btn_rect_ = {bounds_.x + bounds_.w - kCalBtnMargin - kCalBtnSize,
                              bounds_.y + (kHeight - kCalBtnSize) * 0.5f,
                              kCalBtnSize, kCalBtnSize};

        const tk::Color btn_bg =
            press_calendar_ ? ctx.theme.palette.subtle_pressed
            : hover_calendar_
                ? ctx.theme.palette.subtle_hover
                : ctx.theme.palette.chrome_bg;

        ctx.canvas.fill_rounded_rect(calendar_btn_rect_, kCalBtnRadius, btn_bg);
        draw_calendar_icon(ctx.canvas, calendar_btn_rect_,
                           ctx.theme.palette.text_primary);
    }
}

void RoomHeader::draw_calendar_icon(tk::Canvas& canvas, tk::Rect button,
                                    tk::Color tint)
{
    // 16x16 icon box, pixel-snapped and centred in the button rect so the
    // 1.5 px strokes stay crisp on every backend.
    const float ox = std::floor(button.x + (button.w - 16.0f) * 0.5f);
    const float oy = std::floor(button.y + (button.h - 16.0f) * 0.5f);

    // Calendar body: outline rounded rect, leaving 2 px at the top so the
    // binding tabs straddle its edge.
    const tk::Rect body{ox, oy + 2.0f, 16.0f, 14.0f};
    canvas.stroke_rounded_rect(body, 2.5f, tint, 1.5f);

    // Two binding tabs straddling the top edge of the body.
    canvas.fill_rounded_rect({ox + 3.0f, oy, 2.5f, 4.0f}, 1.0f, tint);
    canvas.fill_rounded_rect({ox + 10.5f, oy, 2.5f, 4.0f}, 1.0f, tint);

    // Header / day-grid divider rule.
    canvas.fill_rect({ox + 1.5f, oy + 6.0f, 13.0f, 1.5f}, tint);

    // 2x3 day-grid dots, faint so they read as texture, not noise.
    const tk::Color dot =
        tint.with_alpha(static_cast<std::uint8_t>(tint.a * 0.55f));
    for (int row = 0; row < 2; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            const float cx = ox + 4.0f + static_cast<float>(col) * 4.0f;
            const float cy = oy + 10.0f + static_cast<float>(row) * 3.0f;
            canvas.fill_rect({cx - 0.8f, cy - 0.8f, 1.6f, 1.6f}, dot);
        }
    }
}

bool RoomHeader::on_pointer_down(tk::Point local)
{
    // calendar_btn_rect_ is in world coords; reconstruct the world point
    // from the widget-local `local` by adding the widget's own origin.
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    if (show_calendar_btn_ &&
        world.x >= calendar_btn_rect_.x &&
        world.x < calendar_btn_rect_.x + calendar_btn_rect_.w &&
        world.y >= calendar_btn_rect_.y &&
        world.y < calendar_btn_rect_.y + calendar_btn_rect_.h)
    {
        press_calendar_ = true;
        return true;
    }
    // Capture press on the name/avatar area in full (non-condensed) mode so
    // on_pointer_up is delivered and can fire on_info_requested.
    if (!condensed_)
    {
        press_info_ = true;
        return true;
    }
    return false;
}

void RoomHeader::on_pointer_up(tk::Point local, bool inside_self)
{
    // Calendar button takes priority: if it captured the press, handle it first.
    if (press_calendar_)
    {
        press_calendar_ = false;
        if (inside_self)
        {
            const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
            if (world.x >= calendar_btn_rect_.x &&
                world.x < calendar_btn_rect_.x + calendar_btn_rect_.w &&
                world.y >= calendar_btn_rect_.y &&
                world.y < calendar_btn_rect_.y + calendar_btn_rect_.h)
            {
                if (on_jump_to_date_requested)
                {
                    on_jump_to_date_requested();
                }
            }
        }
        return;
    }

    if (!press_info_)
        return;
    press_info_ = false;

    // Topic hyperlink click and name/avatar click.
    // `local` is widget-local but topic_rect_ is in world coordinates (set in
    // arrange() from bounds_.{x,y}); convert to world first, exactly like the
    // calendar-button hit-test above, else links are unhittable on any layout
    // whose origin isn't (0,0).
    if (!inside_self)
        return;

    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};

    // Check for a topic hyperlink hit first.
    if (topic_layout_)
    {
        tk::Point ll{world.x - topic_rect_.x, world.y - topic_rect_.y};
        std::string url = topic_layout_->link_at(ll);
        if (!url.empty() && on_link_clicked)
        {
            on_link_clicked(url);
            return;
        }
    }

    // Click was not on a hyperlink — fire on_info_requested.
    if (on_info_requested)
        on_info_requested();
}

tk::Widget* RoomHeader::dispatch_pointer_move(tk::Point world, bool* dirty)
{
    if (!visible_ || !contains_world(world))
    {
        return nullptr;
    }

    // Children (name_label_, topic_label_) are purely visual — they don't
    // override on_pointer_move. But the default Widget::dispatch_pointer_move
    // returns the deepest child and skips calling on_pointer_move() on the
    // parent, so calendar-button hover and topic tooltip never fire when the
    // cursor is over the label text. Fix: delegate first, then always call our
    // own on_pointer_move(), and return `this` so the host tracks RoomHeader
    // as hovered_widget_ and on_pointer_leave() reaches us on exit.
    Widget* hit = Widget::dispatch_pointer_move(world, dirty);
    if (hit && hit != this)
    {
        tk::Point local{world.x - bounds_.x, world.y - bounds_.y};
        if (on_pointer_move(local) && dirty)
        {
            *dirty = true;
        }
    }
    return this;
}

bool RoomHeader::on_pointer_move(tk::Point local)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};

    hover_calendar_ = show_calendar_btn_ &&
                      world.x >= calendar_btn_rect_.x &&
                      world.x < calendar_btn_rect_.x + calendar_btn_rect_.w &&
                      world.y >= calendar_btn_rect_.y &&
                      world.y < calendar_btn_rect_.y + calendar_btn_rect_.h;

    const bool new_hover_topic = !hover_calendar_ && world.x >= topic_rect_.x &&
                                 world.x < topic_rect_.x + topic_rect_.w &&
                                 world.y >= topic_rect_.y &&
                                 world.y < topic_rect_.y + topic_rect_.h;

    if (new_hover_topic && !hover_topic_ && topic_truncated_)
    {
        if (on_show_tooltip)
        {
            on_show_tooltip(topic_, topic_rect_);
        }
    }
    else if (!new_hover_topic && hover_topic_)
    {
        if (on_hide_tooltip)
        {
            on_hide_tooltip();
        }
    }
    hover_topic_ = new_hover_topic;
    return true;
}

void RoomHeader::on_pointer_leave()
{
    hover_calendar_ = false;
    press_calendar_ = false;
    press_info_ = false;
    if (hover_topic_ && on_hide_tooltip)
    {
        on_hide_tooltip();
    }
    hover_topic_ = false;
    // Host calls request_repaint() after dispatching pointer-leave.
}

} // namespace tesseract::views
