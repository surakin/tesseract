#include "SpaceRootView.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

SpaceRootView::SpaceRootView()
{
    settings_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "\xF0\x9F\x94\xA7", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    settings_btn_->set_on_click([this]() {
        if (!space_ || !settings_view_) return;
        settings_view_->open(*space_);
        if (on_settings_opened) on_settings_opened(space_->id);
        if (on_layout_changed) on_layout_changed();
    });

    auto settings = tk::create_widget<RoomSettingsView>(this);
    settings_view_ = add_child(std::move(settings));
    settings_view_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
    settings_view_->on_cancel = [this]()
    {
        settings_view_->close();
    };
    settings_view_->on_avatar_upload_clicked = [this]()
    {
        if (on_settings_avatar_upload_requested && space_)
            on_settings_avatar_upload_requested(space_->id);
    };
    settings_view_->on_avatar_remove_clicked = [this]()
    {
        settings_view_->set_staged_avatar("");
    };
    settings_view_->on_copy_to_clipboard = [this](std::string text)
    {
        if (on_copy_to_clipboard) on_copy_to_clipboard(std::move(text));
    };

    set_visible(false);
}

void SpaceRootView::set_space(const tesseract::RoomInfo& space,
                              std::size_t joined_children,
                              std::size_t unjoined_children)
{
    const bool space_changed = !space_ || space_->id != space.id;
    if (space_changed && settings_view_ && settings_view_->is_open())
        settings_view_->close();

    space_ = space;
    joined_children_ = joined_children;
    unjoined_children_ = unjoined_children;
    reset_layouts_();
    set_visible(true);
}

void SpaceRootView::clear()
{
    if (settings_view_ && settings_view_->is_open())
        settings_view_->close();
    space_.reset();
    joined_children_ = 0;
    unjoined_children_ = 0;
    reset_layouts_();
    set_visible(false);
}

void SpaceRootView::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = p;
    if (settings_view_) settings_view_->set_avatar_provider(p);
}

void SpaceRootView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    if (settings_view_) settings_view_->set_post_delayed(std::move(f));
}

tk::Size SpaceRootView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void SpaceRootView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const bool settings_open = settings_view_ && settings_view_->is_open();
    if (settings_btn_)
        settings_btn_->set_visible(!settings_open);

    if (settings_open)
    {
        settings_view_->arrange(ctx, bounds);
        return;
    }

    constexpr float kBtnSz = 32.0f;
    if (settings_btn_)
        settings_btn_->arrange(ctx, {bounds.x + 8.0f, bounds.y + 8.0f, kBtnSz, kBtnSz});
}

void SpaceRootView::reset_layouts_()
{
    name_layout_.reset();
    alias_layout_.reset();
    topic_layout_.reset();
    meta_layout_.reset();
    hint_layout_.reset();
    factory_seen_ = nullptr;
    last_bounds_h_ = -1.0f;
    last_content_w_ = -1.0f;
}

std::string SpaceRootView::child_count_label_() const
{
    const auto joined = static_cast<long>(joined_children_);
    std::string label = tk::trf(
        tk::trn("{0} room", "{0} rooms", joined),
        {std::to_string(joined_children_)});
    if (unjoined_children_ > 0)
    {
        const auto available = static_cast<long>(unjoined_children_);
        label += " \xc2\xb7 ";
        label += tk::trf(
            tk::trn("{0} available to join", "{0} available to join", available),
            {std::to_string(unjoined_children_)});
    }
    return label;
}

void SpaceRootView::paint(tk::PaintCtx& ctx)
{
    if (settings_view_ && settings_view_->is_open())
    {
        settings_view_->paint(ctx);
        return;
    }

    if (!space_) return;

    const auto& pal = ctx.theme.palette;
    const auto& s = *space_;
    auto& cv = ctx.canvas;

    cv.fill_rect(bounds_, pal.bg);

    if (&ctx.factory != factory_seen_)
    {
        reset_layouts_();
        factory_seen_ = &ctx.factory;
    }

    const float content_w = std::min(kContentW, std::max(0.0f, bounds_.w - 48.0f));

    if (!name_layout_)
    {
        tk::TextStyle name_style{};
        name_style.role = tk::FontRole::Title;
        name_style.trim = tk::TextTrim::Ellipsis;
        name_style.max_width = content_w;
        const std::string& nm = s.name.empty() ? s.id : s.name;
        name_layout_ = ctx.factory.build_text(nm, name_style);

        if (!s.canonical_alias.empty())
        {
            tk::TextStyle alias_style{};
            alias_style.role = tk::FontRole::Body;
            alias_style.trim = tk::TextTrim::Ellipsis;
            alias_style.max_width = content_w;
            alias_layout_ = ctx.factory.build_text(s.canonical_alias, alias_style);
        }

        tk::TextStyle meta_style{};
        meta_style.role = tk::FontRole::SidebarPreview;
        meta_style.trim = tk::TextTrim::Ellipsis;
        meta_style.max_width = content_w;
        meta_layout_ = ctx.factory.build_text(child_count_label_(), meta_style);

        tk::TextStyle hint_style{};
        hint_style.role = tk::FontRole::Body;
        hint_style.wrap = true;
        hint_style.halign = tk::TextHAlign::Center;
        hint_style.max_width = content_w;
        hint_layout_ = ctx.factory.build_text(
            tk::tr("Explore the rooms in this space from the room list."),
            hint_style);
    }

    const float meta_h = meta_layout_ ? meta_layout_->measure().h : 16.0f;
    const float hint_h = hint_layout_ ? hint_layout_->measure().h : 40.0f;
    const float bottom_block_y =
        bounds_.y + bounds_.h - kPadY - hint_h - kGap - meta_h;

    if (!s.topic.empty() &&
        (bounds_.h != last_bounds_h_ || content_w != last_content_w_ ||
         !topic_layout_))
    {
        last_bounds_h_ = bounds_.h;
        last_content_w_ = content_w;
        topic_layout_.reset();

        float cy_topic = bounds_.y + kPadY + kAvatarD + kGap;
        if (name_layout_) cy_topic += name_layout_->measure().h + kGap * 0.5f;
        if (alias_layout_) cy_topic += alias_layout_->measure().h + kGap * 0.5f;

        const float max_h = bottom_block_y - kGap - cy_topic;

        tk::TextStyle ts{};
        ts.role = tk::FontRole::Body;
        ts.trim = tk::TextTrim::Ellipsis;
        ts.wrap = true;
        ts.max_width = content_w;
        ts.max_height = std::max(0.0f, max_h);
        topic_layout_ = ctx.factory.build_text(s.topic, ts);
    }

    const float cx = bounds_.x + (bounds_.w - content_w) * 0.5f;
    float cy = bounds_.y + kPadY;

    const tk::Point av_centre{cx + content_w * 0.5f,
                              cy + kAvatarD * 0.5f};
    const tk::Image* av_img = (!s.avatar_url.empty() && avatar_provider_)
                                  ? avatar_provider_(s.avatar_url)
                                  : nullptr;
    if (!av_img && !s.avatar_url.empty() && on_avatar_needed)
        on_avatar_needed(s.avatar_url);

    const std::string& initials_src = s.name.empty() ? s.id : s.name;
    draw_avatar(cv, av_img, av_centre, kAvatarD, initials_src,
                pal.avatar_initials_bg, pal.avatar_initials_text);
    cy += kAvatarD + kGap;

    if (name_layout_)
    {
        const auto nm = name_layout_->measure();
        cv.draw_text(*name_layout_, {cx + (content_w - nm.w) * 0.5f, cy},
                     pal.text_primary);
        cy += nm.h + kGap * 0.5f;
    }

    if (alias_layout_)
    {
        const auto al = alias_layout_->measure();
        cv.draw_text(*alias_layout_, {cx + (content_w - al.w) * 0.5f, cy},
                     pal.text_secondary);
        cy += al.h + kGap * 0.5f;
    }

    if (topic_layout_)
    {
        const auto tp = topic_layout_->measure();
        cv.draw_text(*topic_layout_, {cx, cy}, pal.text_secondary);
    }

    if (meta_layout_)
    {
        const auto mt = meta_layout_->measure();
        cv.draw_text(*meta_layout_,
                     {cx + (content_w - mt.w) * 0.5f, bottom_block_y},
                     pal.text_secondary);
    }

    if (hint_layout_)
    {
        const auto ht = hint_layout_->measure();
        cv.draw_text(*hint_layout_,
                     {cx + (content_w - ht.w) * 0.5f,
                      bottom_block_y + meta_h + kGap},
                     pal.text_muted);
    }

    if (settings_btn_)
    {
        settings_btn_->paint(ctx);
        settings_icon_.draw(ctx.canvas, ctx.factory, kWrenchSvg,
                            settings_btn_->bounds(), 16.0f,
                            pal.text_secondary);
    }
}

} // namespace tesseract::views
