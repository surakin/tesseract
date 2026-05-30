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

constexpr float kLockW   = 10.0f;
constexpr float kLockH   = 12.0f;
constexpr float kLockGap =  4.0f;

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

    // Calendar / threads action buttons. Icon-variant buttons paint only their
    // hover/press background; this view draws the glyph itself in paint(). Both
    // start hidden until the shell enables them (MSC3030 support / thread roots).
    auto cal = std::make_unique<tk::Button>("", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    calendar_btn_ = add_child(std::move(cal));
    calendar_btn_->set_visible(false);
    calendar_btn_->set_on_click(
        [this] { if (on_jump_to_date_requested) on_jump_to_date_requested(); });

    auto thr = std::make_unique<tk::Button>("", std::function<void()>{},
                                            tk::Button::Variant::Icon);
    threads_btn_ = add_child(std::move(thr));
    threads_btn_->set_visible(false);
    threads_btn_->set_on_click(
        [this] { if (on_threads_requested) on_threads_requested(); });
}

void RoomHeader::set_room(const tesseract::RoomInfo& info)
{
    display_name_ = info.name;
    avatar_url_ = info.effective_avatar_url();
    encrypted_ = info.is_encrypted;
    if (name_label_)
    {
        name_label_->set_text(display_name_);
    }

    // Skip the expensive topic rebuild when topic content hasn't changed.
    // Read receipts, typing indicators, and membership events fire this path
    // frequently; guarding here eliminates redundant CTFramesetter work.
    const bool topic_changed =
        (info.topic != topic_) || (info.topic_html != topic_html_);
    topic_ = info.topic;
    topic_html_ = info.topic_html;
    if (!topic_changed)
        return;

    topic_layout_.reset();
    topic_dirty_ = true;
    topic_truncated_ = false;
    topic_multiline_ = false;
    topic_natural_w_ = -1.0f;
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

    // Action buttons are never shown in condensed mode; hide + zero them so the
    // test rect accessor and hit-testing report no clickable area.
    auto hide_action_buttons = [&]
    {
        if (calendar_btn_)
        {
            calendar_btn_->set_visible(false);
            calendar_btn_->arrange(ctx, {});
        }
        if (threads_btn_)
        {
            threads_btn_->set_visible(false);
            threads_btn_->arrange(ctx, {});
        }
    };

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
        hide_action_buttons();
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
        hide_action_buttons();

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
                    if (topic_natural_w_ < 0.0f)
                    {
                        tk::TextStyle ts_nat{};
                        ts_nat.role = tk::FontRole::SidebarPreview;
                        auto nat = ctx.factory.build_rich_text(
                            topic_display_spans_, ts_nat);
                        topic_natural_w_ = nat ? nat->measure().w : 0.0f;
                    }
                    topic_truncated_ = topic_natural_w_ > topic_rect_.w;
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
                    if (topic_natural_w_ < 0.0f)
                    {
                        tk::TextStyle ts_nat{};
                        ts_nat.role = tk::FontRole::SidebarPreview;
                        auto nat = ctx.factory.build_text(topic_, ts_nat);
                        topic_natural_w_ = nat ? nat->measure().w : 0.0f;
                    }
                    topic_truncated_ = topic_natural_w_ > topic_rect_.w;
                }
            }
        }
        return;
    }

    const float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
    // Right-side reserve: each visible action button takes 28 px; when both
    // are shown they sit 8 px apart. When neither is shown the reserve is just
    // the outer margin so the topic can use the freed width.
    const int visible_btns =
        (show_threads_btn_ ? 1 : 0) + (show_calendar_btn_ ? 1 : 0);
    const float right_reserve =
        visible_btns == 0
            ? kCalBtnMargin
            : (visible_btns == 1
                   ? kCalBtnMargin + kCalBtnSize + kCalBtnMargin
                   : kCalBtnMargin + kCalBtnSize + 8.0f + kCalBtnSize +
                         kCalBtnMargin);
    const float text_w = std::max(0.0f, bounds.w - kPadX - kAvatarSize -
                                            kAvatarGap - right_reserve);

    const bool has_topic = !topic_.empty() || !topic_html_.empty();
    const bool show_label = has_topic && topic_html_.empty();

    const float name_y =
        has_topic ? bounds.y + kNameY_Pair : bounds.y + kNameY_Single;

    const float lock_reserve = encrypted_ ? (kLockW + kLockGap) : 0.0f;
    lock_icon_rect_ = encrypted_
                          ? tk::Rect{text_x,
                                     name_y + (kNameH - kLockH) * 0.5f,
                                     kLockW, kLockH}
                          : tk::Rect{};

    if (name_label_)
    {
        name_label_->arrange(
            ctx,
            {text_x + lock_reserve, name_y,
             std::max(0.0f, text_w - lock_reserve), kNameH});
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
                if (topic_natural_w_ < 0.0f)
                {
                    tk::TextStyle ts_nat{};
                    ts_nat.role = tk::FontRole::SidebarPreview;
                    auto nat =
                        ctx.factory.build_rich_text(topic_display_spans_,
                                                    ts_nat);
                    topic_natural_w_ = nat ? nat->measure().w : 0.0f;
                }
                topic_truncated_ = topic_natural_w_ > text_w;
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
                if (topic_natural_w_ < 0.0f)
                {
                    tk::TextStyle ts_nat{};
                    ts_nat.role = tk::FontRole::SidebarPreview;
                    auto nat = ctx.factory.build_text(topic_, ts_nat);
                    topic_natural_w_ = nat ? nat->measure().w : 0.0f;
                }
                topic_truncated_ = topic_natural_w_ > text_w;
            }
        }
    }

    // Position the calendar / threads action buttons. Calendar takes the
    // right-most slot; threads sits 8 px to its left (or takes the right-most
    // slot itself when calendar is hidden). Mirrors the old paint-time layout.
    const float btn_y = bounds.y + (kHeight - kCalBtnSize) * 0.5f;
    tk::Rect cal_r{};
    if (show_calendar_btn_)
    {
        cal_r = {bounds.x + bounds.w - kCalBtnMargin - kCalBtnSize, btn_y,
                 kCalBtnSize, kCalBtnSize};
    }
    if (calendar_btn_)
    {
        calendar_btn_->set_visible(show_calendar_btn_);
        calendar_btn_->arrange(ctx, show_calendar_btn_ ? cal_r : tk::Rect{});
    }

    tk::Rect thr_r{};
    if (show_threads_btn_)
    {
        const float threads_right =
            show_calendar_btn_ ? (cal_r.x - 8.0f)
                               : (bounds.x + bounds.w - kCalBtnMargin);
        thr_r = {threads_right - kCalBtnSize, btn_y, kCalBtnSize, kCalBtnSize};
    }
    if (threads_btn_)
    {
        threads_btn_->set_visible(show_threads_btn_);
        threads_btn_->arrange(ctx, show_threads_btn_ ? thr_r : tk::Rect{});
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

    if (encrypted_)
    {
        draw_lock_icon(ctx.canvas, lock_icon_rect_,
                       ctx.theme.palette.text_secondary);
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
    // The Icon-variant button paints its own hover/press background (positioned
    // in arrange()); we draw the vector glyph centred inside its bounds.
    if (calendar_btn_ && calendar_btn_->visible())
    {
        calendar_btn_->paint(ctx);
        draw_calendar_icon(ctx.canvas, calendar_btn_->bounds(),
                           ctx.theme.palette.text_primary);
    }

    // Threads button — shown only when the SDK reports the room has at least
    // one thread root. Sits immediately left of the calendar button when both
    // are visible, otherwise takes the right-most slot (see arrange()).
    if (threads_btn_ && threads_btn_->visible())
    {
        threads_btn_->paint(ctx);
        draw_threads_icon(ctx.canvas, threads_btn_->bounds(),
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

void RoomHeader::draw_threads_icon(tk::Canvas& canvas, tk::Rect button,
                                   tk::Color tint)
{
    // 16x16 icon box, pixel-snapped and centred. The glyph is three stacked
    // horizontal bars of decreasing length — reads as a thread / reply stack.
    const float ox = std::floor(button.x + (button.w - 16.0f) * 0.5f);
    const float oy = std::floor(button.y + (button.h - 16.0f) * 0.5f);

    // Three bars of widths 12, 10, 8 px, each 2 px tall with 3 px gaps.
    canvas.fill_rounded_rect({ox + 2.0f, oy + 2.0f, 12.0f, 2.0f}, 1.0f, tint);
    canvas.fill_rounded_rect({ox + 2.0f, oy + 7.0f, 10.0f, 2.0f}, 1.0f, tint);
    canvas.fill_rounded_rect({ox + 2.0f, oy + 12.0f, 8.0f, 2.0f}, 1.0f, tint);
}

void RoomHeader::draw_lock_icon(tk::Canvas& canvas, tk::Rect rect,
                                tk::Color tint)
{
    // 10×12 icon box, pixel-snapped and centred in `rect`.
    const float ox = std::floor(rect.x + (rect.w - 10.0f) * 0.5f);
    const float oy = std::floor(rect.y + (rect.h - 12.0f) * 0.5f);

    // Shackle drawn first (stroke) so the filled body can overlap its lower
    // half, giving a clean join without any background-colour dependency.
    const tk::Rect shackle{ox + 2.0f, oy, 6.0f, 8.0f};
    canvas.stroke_rounded_rect(shackle, 3.0f, tint, 2.0f);

    // Padlock body — fills over the bottom portion of the shackle.
    const tk::Rect body{ox, oy + 5.0f, 10.0f, 7.0f};
    canvas.fill_rounded_rect(body, 1.5f, tint);
}

bool RoomHeader::on_pointer_down(tk::Point /*local*/)
{
    // Calendar / threads clicks are claimed by their child tk::Button widgets
    // before this fires (the host dispatches to the topmost child). This only
    // runs for the name/avatar/topic area: capture the press in full
    // (non-condensed) mode so on_pointer_up is delivered and can fire
    // on_info_requested.
    if (!condensed_)
    {
        press_info_ = true;
        return true;
    }
    return false;
}

void RoomHeader::on_pointer_up(tk::Point local, bool inside_self)
{
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

    // Calendar / threads hover backgrounds are driven by the child tk::Button
    // widgets via the host's topmost-hovered-button tracking; this handler only
    // owns the topic tooltip. topic_rect_ already excludes the button reserve,
    // so it never overlaps the action buttons.
    const bool new_hover_topic = world.x >= topic_rect_.x &&
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
    // Action-button hover/press is owned by the child tk::Button widgets.
    press_info_ = false;
    if (hover_topic_ && on_hide_tooltip)
    {
        on_hide_tooltip();
    }
    hover_topic_ = false;
    // Host calls request_repaint() after dispatching pointer-leave.
}

} // namespace tesseract::views
