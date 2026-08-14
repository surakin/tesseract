#include "KnockRequestsPanel.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{
constexpr float kKnockRowBtnH   = 28.0f;
constexpr float kKnockRowBtnGap = 6.0f;
constexpr float kKnockRowPadX   = 8.0f;
constexpr float kKnockAvatarD   = 32.0f;
} // namespace

// ── RowWidget ────────────────────────────────────────────────────────────
//
// One pending request, with its own real Accept/Deny(/Deny & Ban) buttons —
// mirrors DevicesSection::DeviceRow, the established pattern for a
// variable-length list whose rows each carry interactive action widgets.

class KnockRequestsPanel::RowWidget : public tk::Widget
{
public:
    RowWidget(tesseract::KnockRequestInfo info, bool can_ban,
              ImageProvider avatar_provider)
        : info_(std::move(info)), avatar_provider_(std::move(avatar_provider))
    {
        auto accept = tk::create_widget<tk::Button>(
            this, tk::tr("Accept"), std::function<void()>{}, tk::Button::Variant::Primary);
        accept->set_on_click([this]() { if (on_accept) on_accept(); });
        accept_btn_ = add_child(std::move(accept));

        auto decline = tk::create_widget<tk::Button>(
            this, tk::tr("Deny"), std::function<void()>{}, tk::Button::Variant::Subtle);
        decline->set_on_click([this]() { if (on_decline) on_decline(); });
        decline_btn_ = add_child(std::move(decline));

        if (can_ban)
        {
            auto ban = tk::create_widget<tk::Button>(
                this, tk::tr("Deny & Ban"), std::function<void()>{},
                tk::Button::Variant::Destructive);
            ban->set_on_click([this]() { if (on_ban) on_ban(); });
            ban_btn_ = add_child(std::move(ban));
        }
    }

    const tesseract::KnockRequestInfo& info() const { return info_; }

    std::function<void()> on_accept;
    std::function<void()> on_decline;
    std::function<void()> on_ban;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w, kRowH};
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;
        const int n = ban_btn_ ? 3 : 2;
        const float total_w = bounds.w - kKnockRowPadX * 2.0f;
        const float btn_w = (total_w - kKnockRowBtnGap * (n - 1)) / static_cast<float>(n);
        const float by = bounds.y + bounds.h - kKnockRowBtnH - 8.0f;
        float bx = bounds.x + kKnockRowPadX;

        accept_btn_->arrange(ctx, {bx, by, btn_w, kKnockRowBtnH});
        bx += btn_w + kKnockRowBtnGap;
        decline_btn_->arrange(ctx, {bx, by, btn_w, kKnockRowBtnH});
        bx += btn_w + kKnockRowBtnGap;
        if (ban_btn_)
        {
            ban_btn_->arrange(ctx, {bx, by, btn_w, kKnockRowBtnH});
        }
    }

    void paint_before_children(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        const float x = bounds_.x + kKnockRowPadX;
        float y = bounds_.y + 8.0f;

        const tk::Point av_centre{x + kKnockAvatarD * 0.5f, y + kKnockAvatarD * 0.5f};
        const tk::Image* av_img = nullptr;
        if (avatar_provider_ && !info_.avatar_url.empty())
        {
            av_img = avatar_provider_(info_.avatar_url);
        }
        const std::string& fallback_name =
            info_.display_name.empty() ? info_.user_id : info_.display_name;
        draw_avatar(ctx.canvas, av_img, av_centre, kKnockAvatarD, fallback_name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        const float text_x = x + kKnockAvatarD + 10.0f;
        const float text_w = bounds_.w - (text_x - bounds_.x) - kKnockRowPadX;

        const std::string& name_str =
            info_.display_name.empty() ? info_.user_id : info_.display_name;
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Body;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = text_w;
            if (auto lo = ctx.factory.build_text(name_str, ts))
            {
                ctx.canvas.draw_text(*lo, {text_x, y}, pal.text_primary);
            }
        }
        y += 18.0f;
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Small;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = text_w;
            const std::string secondary =
                info_.reason.empty() ? info_.user_id
                                     : tk::trf(tk::tr("Reason: {0}"), {info_.reason});
            if (auto lo = ctx.factory.build_text(secondary, ts))
            {
                ctx.canvas.draw_text(*lo, {text_x, y}, pal.text_muted);
            }
        }

        // Row separator.
        ctx.canvas.fill_rect({bounds_.x, bounds_.bottom() - 1.0f, bounds_.w, 1.0f},
                             pal.border);
    }

    static constexpr float kRowH = 96.0f;

private:
    tesseract::KnockRequestInfo info_;
    ImageProvider avatar_provider_;
    tk::Button* accept_btn_  = nullptr;
    tk::Button* decline_btn_ = nullptr;
    tk::Button* ban_btn_     = nullptr; // only when can_ban
};

// ── KnockRequestsPanel ───────────────────────────────────────────────────

KnockRequestsPanel::KnockRequestsPanel()
{
    auto close = tk::create_widget<tk::Button>(
        this, "\xC3\x97", std::function<void()>{}, tk::Button::Variant::Subtle);
    close->set_on_click([this]() { if (on_close) on_close(); });
    close_btn_ = add_child(std::move(close));

    set_visible(false);
}

KnockRequestsPanel::~KnockRequestsPanel() = default;

void KnockRequestsPanel::open(const std::string& room_id)
{
    room_id_       = room_id;
    open_          = true;
    scroll_offset_ = 0.0f;
    set_visible(true);
}

void KnockRequestsPanel::close()
{
    open_ = false;
    room_id_.clear();
    requests_.clear();
    rebuild_rows_();
    set_visible(false);
}

void KnockRequestsPanel::set_requests(std::vector<tesseract::KnockRequestInfo> requests)
{
    requests_ = std::move(requests);
    rebuild_rows_();
    if (on_layout_changed)
        on_layout_changed();
}

void KnockRequestsPanel::set_can_ban(bool can_ban)
{
    if (can_ban_ == can_ban)
        return;
    can_ban_ = can_ban;
    rebuild_rows_();
    if (on_layout_changed)
        on_layout_changed();
}

void KnockRequestsPanel::set_avatar_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void KnockRequestsPanel::rebuild_rows_()
{
    for (auto* row : rows_)
    {
        remove_child(row);
    }
    rows_.clear();

    for (const auto& req : requests_)
    {
        auto row = std::make_unique<RowWidget>(req, can_ban_, image_provider_);
        // Copy out the user_id rather than capturing `req`/`this` row-by-
        // reference — the row itself outlives this loop iteration, so the
        // copy just needs to be independent of the source vector's storage.
        const std::string user_id = req.user_id;
        row->on_accept = [this, user_id]() { if (on_accept) on_accept(user_id); };
        row->on_decline = [this, user_id]() { if (on_decline) on_decline(user_id); };
        row->on_ban = [this, user_id]()
        {
            if (on_decline_and_ban) on_decline_and_ban(user_id, "");
        };
        rows_.push_back(add_child(std::move(row)));
    }
}

void KnockRequestsPanel::on_theme_changed(const tk::Theme&)
{
    title_layout_.reset();
    empty_layout_.reset();
}

tk::Size KnockRequestsPanel::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void KnockRequestsPanel::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;

    backdrop_rect_ = bounds;
    panel_rect_    = {bounds.x + bounds.w - kPanelW, bounds.y, kPanelW, bounds.h};

    const float px = panel_rect_.x;

    if (close_btn_)
    {
        close_btn_->arrange(
            lc, {px + kPanelW - 8.0f - kCloseSz, panel_rect_.y + 8.0f, kCloseSz, kCloseSz});
    }

    const float scroll_top = panel_rect_.y + kHeaderH;
    const float viewport_h = std::max(0.0f, panel_rect_.h - kHeaderH);

    float y = scroll_top - scroll_offset_;
    for (auto* row : rows_)
    {
        row->arrange(lc, {px, y, kPanelW, RowWidget::kRowH});
        y += RowWidget::kRowH;
    }
    content_height_ = (y + scroll_offset_) - scroll_top;

    const float max_scroll = std::max(0.0f, content_height_ - viewport_h);
    scroll_offset_ = std::clamp(scroll_offset_, 0.0f, max_scroll);
}

void KnockRequestsPanel::paint(tk::PaintCtx& ctx)
{
    if (!open_)
        return;

    const auto& pal = ctx.theme.palette;
    auto&       cv  = ctx.canvas;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 100});
    cv.fill_rect(panel_rect_, pal.bg);
    cv.fill_rect({panel_rect_.x, panel_rect_.y, 1.0f, panel_rect_.h}, pal.border);

    const float px = panel_rect_.x;

    if (!title_layout_)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Title;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = kPanelW - kPadX * 2.0f - kCloseSz - 8.0f;
        title_layout_ = ctx.factory.build_text(
            tk::trf(tk::tr("Requests to Join ({0})"), {std::to_string(requests_.size())}),
            ts);
    }
    if (title_layout_)
    {
        cv.draw_text(*title_layout_, {px + kPadX, panel_rect_.y + kPadY},
                     pal.text_primary);
    }
    if (close_btn_)
    {
        close_btn_->paint(ctx);
    }
    cv.fill_rect({px + kPadX, panel_rect_.y + kHeaderH - 1.0f, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.border);

    const float scroll_top = panel_rect_.y + kHeaderH;
    const float viewport_h = std::max(0.0f, panel_rect_.h - kHeaderH);
    cv.push_clip_rect({px, scroll_top, kPanelW, viewport_h});

    if (rows_.empty())
    {
        if (!empty_layout_)
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Body;
            ts.halign    = tk::TextHAlign::Center;
            ts.max_width = kPanelW - kPadX * 2.0f;
            empty_layout_ = ctx.factory.build_text(tk::tr("No pending requests"), ts);
        }
        if (empty_layout_)
        {
            cv.draw_text(*empty_layout_, {px + kPadX, scroll_top + kPadY},
                         pal.text_muted);
        }
    }
    else
    {
        for (auto* row : rows_)
        {
            row->paint(ctx);
        }
    }

    cv.pop_clip();
}

bool KnockRequestsPanel::on_pointer_down(tk::Point local)
{
    if (!open_)
        return false;
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};
    press_backdrop_ = rect_contains(backdrop_rect_, w) && !rect_contains(panel_rect_, w);
    return press_backdrop_ || rect_contains(panel_rect_, w);
}

void KnockRequestsPanel::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
        return;
    press_backdrop_ = false;
    const tk::Point w{local.x + bounds_.x, local.y + bounds_.y};
    if (inside_self && !rect_contains(panel_rect_, w) && on_close)
        on_close();
}

bool KnockRequestsPanel::on_wheel(tk::Point /*local*/, float /*dx*/, float dy,
                                  bool /*is_touchpad*/)
{
    if (!open_)
        return false;
    scroll_offset_ += dy * 20.0f;
    scroll_offset_ = std::max(0.0f, scroll_offset_);
    return true;
}

} // namespace tesseract::views
