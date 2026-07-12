#include "QuickSwitcher.h"

#include "media_utils.h"
#include "text_util.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kQuickSwitcherAvatarSize = 28.0f;
constexpr float kQuickSwitcherPadX = 12.0f;
constexpr float kQuickSwitcherAvatarGap = 12.0f;
constexpr float kQuickSwitcherFieldH = 34.0f;

// Case-insensitive substring match (byte-level ASCII approximation). Mirrors
using tesseract::text::name_matches;

} // namespace

// ─────────────────────────────────────────────────────────────────────────

class QuickSwitcher::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(QuickSwitcher& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.active_count_();
    }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        if (owner_.mode_ == Mode::User)
        {
            paint_user_row(index, ctx, bounds, selected, hovered);
            return;
        }
        if (index >= owner_.filtered_.size())
        {
            return;
        }
        const auto& room = owner_.filtered_[index];

        // Row background — selection > hover.
        if (selected)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
        }

        // Avatar circle (left-aligned, vertically centred).
        const float avatar_cx = bounds.x + kQuickSwitcherPadX + kQuickSwitcherAvatarSize * 0.5f;
        const float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = room.effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
            if (!avatar && owner_.on_room_avatar_needed)
            {
                owner_.on_room_avatar_needed(room);
            }
        }

        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kQuickSwitcherAvatarSize,
                    room.name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        // Room name — single line, ellipsised to the row width.
        const float text_x = bounds.x + kQuickSwitcherPadX + kQuickSwitcherAvatarSize + kQuickSwitcherAvatarGap;
        const float text_w = std::max(0.0f, bounds.x + bounds.w - kQuickSwitcherPadX - text_x);

        // Single-line, ellipsised. Note: do NOT set max_height/valign here — a
        // bounded box makes measure().h fill the row, collapsing the manual
        // vertical-centre offset below to ~0 (text would stick to the top).
        tk::TextStyle ns{};
        ns.role = tk::FontRole::SidebarName;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(
            room.name.empty() ? std::string("Unnamed room") : room.name, ns);
        if (name_lo)
        {
            const tk::Size sz = name_lo->measure();
            ctx.canvas.draw_text(*name_lo,
                                 {text_x, bounds.y + (bounds.h - sz.h) * 0.5f},
                                 ctx.theme.palette.text_primary);
        }
    }

    // User-mode row: avatar + display name (top) + mxid (muted, below).
    void paint_user_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                        bool selected, bool hovered)
    {
        if (index >= owner_.user_results_.size())
        {
            return;
        }
        const auto& user = owner_.user_results_[index];

        if (selected)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
        }

        const float avatar_cx = bounds.x + kQuickSwitcherPadX + kQuickSwitcherAvatarSize * 0.5f;
        const float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        if (owner_.avatar_provider_ && !user.avatar_url.empty())
        {
            avatar = owner_.avatar_provider_(user.avatar_url);
            if (!avatar && owner_.on_user_avatar_needed)
            {
                owner_.on_user_avatar_needed(user.avatar_url);
            }
        }

        const std::string& disc_name =
            user.display_name.empty() ? user.user_id : user.display_name;
        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kQuickSwitcherAvatarSize,
                    disc_name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        const float text_x = bounds.x + kQuickSwitcherPadX + kQuickSwitcherAvatarSize + kQuickSwitcherAvatarGap;
        const float text_w = std::max(0.0f, bounds.x + bounds.w - kQuickSwitcherPadX - text_x);

        // Display name (primary, upper line).
        tk::TextStyle ns{};
        ns.role = tk::FontRole::SidebarName;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(disc_name, ns);

        // mxid (muted, lower line).
        tk::TextStyle ms{};
        ms.role = tk::FontRole::Small;
        ms.trim = tk::TextTrim::Ellipsis;
        ms.max_width = text_w;
        auto id_lo = ctx.factory.build_text(user.user_id, ms);

        const float name_h = name_lo ? name_lo->measure().h : 0.0f;
        const float id_h = id_lo ? id_lo->measure().h : 0.0f;
        const float gap = 1.0f;
        const float block_h = name_h + gap + id_h;
        float y = bounds.y + (bounds.h - block_h) * 0.5f;
        if (name_lo)
        {
            ctx.canvas.draw_text(*name_lo, {text_x, y},
                                 ctx.theme.palette.text_primary);
        }
        y += name_h + gap;
        if (id_lo)
        {
            ctx.canvas.draw_text(*id_lo, {text_x, y},
                                 ctx.theme.palette.text_muted);
        }
    }

private:
    QuickSwitcher& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

QuickSwitcher::QuickSwitcher() : adapter_(std::make_unique<Adapter>(*this))
{
    set_visible(false);

    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= active_count_())
        {
            return;
        }
        list_->set_selected_index(idx);
        activate_selected();
    };
    list_ = add_child(std::move(list));
}

QuickSwitcher::~QuickSwitcher() = default;

void QuickSwitcher::set_rooms_provider(RoomsProvider p)
{
    rooms_provider_ = std::move(p);
}

void QuickSwitcher::set_recent_provider(RoomsProvider p)
{
    recent_provider_ = std::move(p);
}

bool QuickSwitcher::show_recent_() const
{
    return mode_ == Mode::Room && query_.empty() && !recent_.empty();
}

std::size_t QuickSwitcher::active_count_() const
{
    return mode_ == Mode::User ? user_results_.size() : filtered_.size();
}

void QuickSwitcher::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void QuickSwitcher::open()
{
    all_rooms_ = rooms_provider_ ? rooms_provider_()
                                 : std::vector<tesseract::RoomInfo>{};
    recent_ = recent_provider_ ? recent_provider_()
                               : std::vector<tesseract::RoomInfo>{};
    query_.clear();
    mode_ = Mode::Room;
    user_results_.clear();
    pressed_chip_ = -1;
    is_open_ = true;
    set_visible(true);
    refilter_();
}

void QuickSwitcher::close()
{
    if (!is_open_)
    {
        return;
    }
    is_open_ = false;
    set_visible(false);
    query_.clear();
    mode_ = Mode::Room;
    user_results_.clear();
    press_outside_ = false;
    if (on_close)
    {
        on_close();
    }
}

void QuickSwitcher::set_query(const std::string& q)
{
    query_ = q;

    // A leading '@' switches to user mode: the shell sources/filters the user
    // roster and live-resolves a typed mxid, then pushes rows via
    // set_user_results(). The switcher itself does no user filtering.
    if (!q.empty() && q.front() == '@')
    {
        mode_ = Mode::User;
        if (on_user_query_changed)
        {
            on_user_query_changed(q);
        }
        return;
    }

    if (mode_ == Mode::User)
    {
        mode_ = Mode::Room;
        user_results_.clear();
    }
    refilter_();
}

void QuickSwitcher::set_user_results(std::vector<UserEntry> users)
{
    // Ignore late/async results that arrive after the query left user mode.
    if (mode_ != Mode::User)
    {
        return;
    }
    user_results_ = std::move(users);
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(user_results_.empty() ? -1 : 0);
        list_->scroll_to_top();
    }
}

void QuickSwitcher::refilter_()
{
    filtered_.clear();
    for (const auto& r : all_rooms_)
    {
        if (name_matches(r.name, query_))
        {
            filtered_.push_back(r);
        }
    }
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(filtered_.empty() ? -1 : 0);
        list_->scroll_to_top();
    }
}

void QuickSwitcher::on_theme_changed(const tk::Theme& t)
{
    if (auto field = native_field_.lock())
        field->set_text_color(t.palette.text_primary);
}

void QuickSwitcher::move_selection(int delta)
{
    if (!list_ || active_count_() == 0)
    {
        return;
    }
    const int n = static_cast<int>(active_count_());
    int cur = list_->selected_index();
    if (cur < 0)
    {
        cur = 0;
    }
    int next = cur + delta;
    next = std::max(0, std::min(n - 1, next));
    list_->set_selected_index(next);
    list_->scroll_to_index(next);
}

void QuickSwitcher::activate_selected()
{
    const int sel = list_ ? list_->selected_index() : -1;
    if (sel < 0 || static_cast<std::size_t>(sel) >= active_count_())
    {
        return;
    }

    if (mode_ == Mode::User)
    {
        const std::string mxid =
            user_results_[static_cast<std::size_t>(sel)].user_id;
        if (on_user_selected)
        {
            on_user_selected(mxid);
        }
        close();
        return;
    }

    const std::string room_id = filtered_[static_cast<std::size_t>(sel)].id;
    if (on_room_selected)
    {
        on_room_selected(room_id);
    }
    close();
}

// ── Layout + paint ────────────────────────────────────────────────────────

tk::Size QuickSwitcher::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void QuickSwitcher::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!list_)
    {
        return;
    }

    const float margin = 40.0f;
    const float cw = std::min(kCardW, std::max(0.0f, bounds.w - 2 * margin));

    const float strip_h = show_recent_() ? kRecentStripH : 0.0f;
    const float chrome_h = kHeaderH + strip_h;

    const float list_content =
        active_count_() == 0 ? kRowH
                             : static_cast<float>(active_count_()) * kRowH;
    const float max_h = std::min(kCardMaxH, std::max(0.0f, bounds.h - 2 * margin));
    float ch =
        chrome_h + std::min(list_content, std::max(0.0f, max_h - chrome_h));
    ch = std::max(ch, chrome_h + kRowH);
    ch = std::min(ch, max_h);

    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    // Bias the card slightly above centre (command-palette convention).
    float cy = bounds.y + (bounds.h - ch) * 0.38f;
    cy = std::max(cy, bounds.y + margin);

    card_rect_ = {cx, cy, cw, ch};
    search_field_rect_ = {cx + kQuickSwitcherPadX, cy + (kHeaderH - kQuickSwitcherFieldH) * 0.5f,
                          std::max(0.0f, cw - 2 * kQuickSwitcherPadX), kQuickSwitcherFieldH};
    recent_strip_rect_ =
        strip_h > 0.0f ? tk::Rect{cx, cy + kHeaderH, cw, strip_h} : tk::Rect{};

    const tk::Rect list_bounds{cx, cy + chrome_h, cw,
                               std::max(0.0f, ch - chrome_h)};
    list_->arrange(ctx, list_bounds);
}

void QuickSwitcher::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
    {
        return;
    }

    // Dim backdrop over the whole surface.
    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));

    // Card.
    ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f,
                                   ctx.theme.palette.popup_border, 1.0f);

    // Header strip background + the search-field card (the OS draws the native
    // text field on top of search_field_rect_).
    tk::Rect sep{card_rect_.x, card_rect_.y + kHeaderH - 1.0f, card_rect_.w,
                 1.0f};
    ctx.canvas.fill_rect(sep, ctx.theme.palette.separator);
    if (!search_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(search_field_rect_, 6.0f,
                                     ctx.theme.palette.compose_card_bg);
        ctx.canvas.stroke_rounded_rect(search_field_rect_, 6.0f,
                                       ctx.theme.palette.border, 1.0f);
    }

    // Recent strip (only when the query is empty). Rebuilt each paint so the
    // hit rects stay in sync with what's drawn.
    recent_chips_.clear();
    if (show_recent_() && !recent_strip_rect_.empty())
    {
        paint_recent_strip_(ctx);
    }

    if (active_count_() == 0)
    {
        tk::TextStyle es{};
        es.role = tk::FontRole::Body;
        const std::string empty_msg =
            mode_ == Mode::User
                ? std::string(
                      "No matching users — type a full @user:server to chat")
                : std::string("No rooms");
        auto empty_lo = ctx.factory.build_text(empty_msg, es);
        if (empty_lo)
        {
            const tk::Size sz = empty_lo->measure();
            ctx.canvas.draw_text(
                *empty_lo,
                {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                 card_rect_.y + kHeaderH +
                     (card_rect_.h - kHeaderH - sz.h) * 0.5f},
                ctx.theme.palette.text_muted);
        }
        return;
    }

    // Clip the list to the card's rounded shape so its square row backgrounds
    // don't paint over the rounded bottom corners. (ListView pushes its own
    // rect clip inside this one.)
    if (list_ && list_->visible())
    {
        ctx.canvas.push_clip_rounded_rect(card_rect_, 10.0f);
        list_->paint(ctx);
        ctx.canvas.pop_clip();
    }
}

void QuickSwitcher::paint_recent_strip_(tk::PaintCtx& ctx)
{
    const tk::Rect strip = recent_strip_rect_;
    const auto& pal = ctx.theme.palette;

    constexpr float kCaptionH = 16.0f;
    constexpr float kTopPad = 8.0f;
    constexpr float kLabelGap = 4.0f;
    constexpr float kLabelH = 14.0f;

    // "Recent" caption.
    {
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Small;
        auto cap = ctx.factory.build_text(std::string("Recent"), cs);
        if (cap)
        {
            ctx.canvas.draw_text(*cap, {strip.x + kQuickSwitcherPadX, strip.y + kTopPad},
                                 pal.text_muted);
        }
    }

    const float chip_y = strip.y + kTopPad + kCaptionH;
    const float chip_h = kRecentAvatar + kLabelGap + kLabelH;
    const float avail = strip.w - 2 * kQuickSwitcherPadX;
    const int max_fit = avail > 0.0f
                            ? static_cast<int>((avail + kRecentChipGap) /
                                               (kRecentChipW + kRecentChipGap))
                            : 0;
    const int count =
        std::min(static_cast<int>(recent_.size()), std::max(0, max_fit));

    float x = strip.x + kQuickSwitcherPadX;
    for (int i = 0; i < count; ++i)
    {
        const auto& room = recent_[static_cast<std::size_t>(i)];
        const tk::Rect chip{x, chip_y, kRecentChipW, chip_h};

        if (pressed_chip_ == i)
        {
            ctx.canvas.fill_rounded_rect(chip, 8.0f, pal.sidebar_hover);
        }

        // Avatar (centred horizontally, at the top of the chip).
        const float acx = chip.x + chip.w * 0.5f;
        const float acy = chip_y + kRecentAvatar * 0.5f;
        const tk::Image* avatar = nullptr;
        const std::string& mxc = room.effective_avatar_url();
        if (avatar_provider_ && !mxc.empty())
        {
            avatar = avatar_provider_(mxc);
            if (!avatar && on_room_avatar_needed)
                on_room_avatar_needed(room);
        }
        draw_avatar(ctx.canvas, avatar, {acx, acy}, kRecentAvatar, room.name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        // Name label below the avatar (centred, single line, ellipsised).
        tk::TextStyle ls{};
        ls.role = tk::FontRole::Small;
        ls.halign = tk::TextHAlign::Center;
        ls.trim = tk::TextTrim::Ellipsis;
        ls.max_width = chip.w;
        auto lo = ctx.factory.build_text(
            room.name.empty() ? std::string("Unnamed") : room.name, ls);
        if (lo)
        {
            ctx.canvas.draw_text(*lo,
                                 {chip.x, chip_y + kRecentAvatar + kLabelGap},
                                 pal.text_primary);
        }

        recent_chips_.push_back({chip, room.id});
        x += kRecentChipW + kRecentChipGap;
    }

    // Separator below the strip.
    tk::Rect ssep{strip.x, strip.y + strip.h - 1.0f, strip.w, 1.0f};
    ctx.canvas.fill_rect(ssep, pal.separator);
}

// ── Pointer ───────────────────────────────────────────────────────────────

bool QuickSwitcher::on_pointer_down(tk::Point local)
{
    if (!is_open_)
    {
        return false;
    }
    // `local` is widget-local; stored rects (card_rect_, recent_chips_) are in
    // world coords. The dispatch hands us (world - bounds_); convert back.
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    auto contains = [](const tk::Rect& r, tk::Point p)
    {
        return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
    };

    // Recent strip chip?
    pressed_chip_ = -1;
    for (int i = 0; i < static_cast<int>(recent_chips_.size()); ++i)
    {
        if (contains(recent_chips_[static_cast<std::size_t>(i)].first, world))
        {
            pressed_chip_ = i;
            press_outside_ = false;
            return true;
        }
    }

    press_outside_ = !contains(card_rect_, world);
    // Always consume so the press never falls through to widgets behind the
    // modal backdrop.
    return true;
}

void QuickSwitcher::on_pointer_up(tk::Point local, bool inside_self)
{
    if (pressed_chip_ >= 0)
    {
        const int chip = pressed_chip_;
        pressed_chip_ = -1;
        const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
        if (chip < static_cast<int>(recent_chips_.size()))
        {
            const auto& [rect, room_id] =
                recent_chips_[static_cast<std::size_t>(chip)];
            const bool on_chip = world.x >= rect.x && world.x < rect.x + rect.w &&
                                 world.y >= rect.y && world.y < rect.y + rect.h;
            if (on_chip)
            {
                if (on_room_selected)
                {
                    on_room_selected(room_id);
                }
                close();
            }
        }
        return;
    }
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
        {
            close();
        }
    }
}

bool QuickSwitcher::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/)
{
    // Swallow wheel events that reach the backdrop so the content behind the
    // modal doesn't scroll. (Wheel over the inner list is consumed by the
    // ListView child before reaching here.)
    return is_open_;
}

} // namespace tesseract::views
