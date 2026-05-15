#include "RoomHeader.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views {

namespace {

constexpr float kPadX       = 16.0f;
constexpr float kAvatarSize = 40.0f;
constexpr float kAvatarGap  = 12.0f;

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
    const float text_w = std::max(0.0f,
        bounds.w - kPadX - kAvatarSize - kAvatarGap - kPadX);

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
    if (!topic_spans_.empty()) {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::SidebarPreview;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = topic_rect_.w;
        auto lo = ctx.factory.build_rich_text(topic_spans_, ts);
        if (lo) {
            ctx.canvas.draw_text(*lo,
                { topic_rect_.x, topic_rect_.y },
                ctx.theme.palette.text_primary);
        }
    } else if (topic_label_ && topic_label_->visible()) {
        topic_label_->paint(ctx);
    }
}

} // namespace tesseract::views
