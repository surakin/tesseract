#include "RoomPreviewView.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <string>

namespace tesseract::views
{

// ── constructor ─────────────────────────────────────────────────────────────

RoomPreviewView::RoomPreviewView()
{
    auto join = std::make_unique<tk::Button>(
        tk::tr("Join"), std::function<void()>{}, tk::Button::Variant::Primary);
    join->set_on_click([this]() { fire_join_(); });
    join_btn_ = add_child(std::move(join));

    auto dismiss = std::make_unique<tk::Button>(
        tk::tr("Dismiss"), std::function<void()>{}, tk::Button::Variant::Subtle);
    dismiss->set_on_click([this]() { if (on_dismiss) on_dismiss(); });
    dismiss_btn_ = add_child(std::move(dismiss));

    set_visible(false);
}

// ── public API ──────────────────────────────────────────────────────────────

void RoomPreviewView::set_summary(const tesseract::RoomSummary& s)
{
    summary_ = s;
    state_   = State::Idle;
    reset_layouts_();
    if (join_btn_)
    {
        join_btn_->set_enabled(s.join_rule != "ban");
        join_btn_->set_label(tk::tr("Join"));
    }
    set_visible(true);
}

void RoomPreviewView::clear()
{
    summary_.reset();
    reset_layouts_();
    set_visible(false);
}

void RoomPreviewView::set_state(State s)
{
    state_ = s;
    if (join_btn_)
        join_btn_->set_enabled(s == State::Idle &&
                               summary_ && summary_->join_rule != "ban");
}

void RoomPreviewView::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

bool RoomPreviewView::join_button_enabled() const
{
    return join_btn_ && join_btn_->enabled();
}

void RoomPreviewView::trigger_join_for_test()
{
    fire_join_();
}

// ── private helpers ─────────────────────────────────────────────────────────

std::string RoomPreviewView::join_rule_label_() const
{
    if (!summary_) return {};
    const auto& jr = summary_->join_rule;
    if (jr == "public")           return tk::tr("Public");
    if (jr == "knock")            return tk::tr("Knock to join");
    if (jr == "invite")           return tk::tr("Invite-only");
    if (jr == "restricted")       return tk::tr("Restricted");
    if (jr == "knock_restricted") return tk::tr("Knock (restricted)");
    return tk::tr("Private");
}

void RoomPreviewView::reset_layouts_()
{
    name_layout_.reset();
    alias_layout_.reset();
    topic_layout_.reset();
    meta_layout_.reset();
    factory_seen_  = nullptr;
    last_bounds_h_ = -1.0f;
}

void RoomPreviewView::fire_join_()
{
    if (!summary_) return;
    if (on_join) on_join(summary_->room_id);
}

// ── layout ───────────────────────────────────────────────────────────────────

tk::Size RoomPreviewView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void RoomPreviewView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!summary_) return;

    // Pin buttons to the bottom of the panel so the topic can grow freely.
    const float cx      = bounds.x + (bounds.w - kContentW) * 0.5f;
    const float btn_y   = bounds.y + bounds.h - kPadY - kBtnH;
    const float btn_w   = (kContentW - kBtnGap) * 0.5f;
    if (join_btn_)    join_btn_->arrange(lc, {cx, btn_y, btn_w, kBtnH});
    if (dismiss_btn_) dismiss_btn_->arrange(lc, {cx + btn_w + kBtnGap, btn_y, btn_w, kBtnH});
}

// ── paint ────────────────────────────────────────────────────────────────────

void RoomPreviewView::paint(tk::PaintCtx& ctx)
{
    if (!summary_) return;

    const auto& pal = ctx.theme.palette;
    const auto& s   = *summary_;
    auto&       cv  = ctx.canvas;

    cv.fill_rect(bounds_, pal.bg);

    if (&ctx.factory != factory_seen_)
    {
        reset_layouts_();
        factory_seen_ = &ctx.factory;
    }

    // Fixed single-line layouts (don't depend on available height).
    if (!name_layout_)
    {
        tk::TextStyle name_style{};
        name_style.role      = tk::FontRole::Title;
        name_style.trim      = tk::TextTrim::Ellipsis;
        name_style.max_width = kContentW;
        const std::string& nm = s.name.empty() ? s.room_id : s.name;
        name_layout_ = ctx.factory.build_text(nm, name_style);

        tk::TextStyle meta_style{};
        meta_style.role      = tk::FontRole::SidebarPreview;
        meta_style.trim      = tk::TextTrim::Ellipsis;
        meta_style.max_width = kContentW;
        const std::string meta = tk::trf(
            tk::tr("{0} members \xc2\xb7 {1}"),
            {std::to_string(s.num_joined_members), join_rule_label_()});
        meta_layout_ = ctx.factory.build_text(meta, meta_style);

        if (!s.canonical_alias.empty())
        {
            tk::TextStyle alias_style{};
            alias_style.role      = tk::FontRole::Body;
            alias_style.trim      = tk::TextTrim::Ellipsis;
            alias_style.max_width = kContentW;
            alias_layout_ = ctx.factory.build_text(s.canonical_alias, alias_style);
        }
    }

    // Topic layout depends on available vertical space; rebuild when height changes.
    if (!s.topic.empty() && (bounds_.h != last_bounds_h_ || !topic_layout_))
    {
        last_bounds_h_ = bounds_.h;
        topic_layout_.reset();

        // Compute how far down the content starts at the topic row.
        float cy_topic = bounds_.y + kPadY + kAvatarD + kGap;
        if (name_layout_)  cy_topic += name_layout_->measure().h  + kGap * 0.5f;
        if (alias_layout_) cy_topic += alias_layout_->measure().h + kGap * 0.5f;

        // Reserve space below: gap + meta + gap + buttons + bottom pad.
        const float meta_h   = meta_layout_ ? meta_layout_->measure().h : 16.0f;
        const float btn_y    = bounds_.y + bounds_.h - kPadY - kBtnH;
        const float max_h    = btn_y - kGap - meta_h - kGap - cy_topic;

        tk::TextStyle ts{};
        ts.role       = tk::FontRole::Body;
        ts.trim       = tk::TextTrim::Ellipsis;
        ts.wrap       = true;
        ts.max_width  = kContentW;
        ts.max_height = std::max(16.0f, max_h);
        topic_layout_ = ctx.factory.build_text(s.topic, ts);
    }

    const float cx = bounds_.x + (bounds_.w - kContentW) * 0.5f;
    float       cy = bounds_.y + kPadY;

    // ── Avatar ────────────────────────────────────────────────────────────

    const tk::Point av_centre{cx + kContentW * 0.5f,
                               cy + kAvatarD * 0.5f};
    const tk::Image* av_img = (!s.avatar_url.empty() && avatar_provider_)
                              ? avatar_provider_(s.avatar_url) : nullptr;
    if (!av_img && !s.avatar_url.empty() && on_avatar_needed)
        on_avatar_needed(s.avatar_url);

    const std::string& initials_src = s.name.empty() ? s.room_id : s.name;
    draw_avatar(cv, av_img, av_centre, kAvatarD, initials_src,
                pal.avatar_initials_bg, pal.avatar_initials_text);
    cy += kAvatarD + kGap;

    // ── Name ──────────────────────────────────────────────────────────────

    if (name_layout_)
    {
        const auto nm = name_layout_->measure();
        cv.draw_text(*name_layout_,
                     {cx + (kContentW - nm.w) * 0.5f, cy},
                     pal.text_primary);
        cy += nm.h + kGap * 0.5f;
    }

    // ── Alias ─────────────────────────────────────────────────────────────

    if (alias_layout_)
    {
        const auto al = alias_layout_->measure();
        cv.draw_text(*alias_layout_,
                     {cx + (kContentW - al.w) * 0.5f, cy},
                     pal.text_secondary);
        cy += al.h + kGap * 0.5f;
    }

    // ── Topic ─────────────────────────────────────────────────────────────

    if (topic_layout_)
    {
        const auto tp = topic_layout_->measure();
        cv.draw_text(*topic_layout_, {cx, cy}, pal.text_secondary);
        cy += tp.h + kGap;
    }

    // ── Meta: member count + join rule ────────────────────────────────────
    // Pinned just above the button row so it stays anchored to the bottom
    // regardless of how much vertical space the topic occupies.

    if (meta_layout_)
    {
        const auto  mt    = meta_layout_->measure();
        const float btn_y = bounds_.y + bounds_.h - kPadY - kBtnH;
        cv.draw_text(*meta_layout_,
                     {cx + (kContentW - mt.w) * 0.5f, btn_y - kGap - mt.h},
                     pal.text_secondary);
    }

    // Buttons are child widgets, painted by the widget tree.
    if (join_btn_ && join_btn_->visible())
        join_btn_->paint(ctx);
    if (dismiss_btn_ && dismiss_btn_->visible())
        dismiss_btn_->paint(ctx);
}

} // namespace tesseract::views
