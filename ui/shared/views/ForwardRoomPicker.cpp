#include "ForwardRoomPicker.h"

#include "media_utils.h"
#include "text_util.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kForwardRoomPickerAvatarSize = 28.0f;
constexpr float kForwardRoomPickerPadX       = 12.0f;
constexpr float kForwardRoomPickerAvatarGap  = 12.0f;
constexpr float kForwardRoomPickerFieldH     = 34.0f;
constexpr float kCheckSize  = 18.0f;
constexpr float kCheckPadR  = 12.0f;
constexpr float kForwardRoomPickerBtnH       = 32.0f;
constexpr float kBtnMinW    = 80.0f;
constexpr float kForwardRoomPickerBtnRadius  = 6.0f;
constexpr float kForwardRoomPickerBtnGap     = 8.0f;

using tesseract::text::name_matches;

} // namespace

// ─────────────────────────────────────────────────────────────────────────

class ForwardRoomPicker::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(ForwardRoomPicker& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.row_count_(); }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool /*selected*/, bool hovered) override
    {
        const auto* room = owner_.room_at_(index);
        if (!room)
            return;
        const bool checked = owner_.is_row_selected_(index);

        // Thin divider above the first unselected row.
        if (owner_.is_divider_above_(index))
        {
            ctx.canvas.fill_rect({bounds.x, bounds.y, bounds.w, 1.0f},
                                 ctx.theme.palette.separator);
        }

        if (checked)
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        else if (hovered)
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);

        const float avatar_cx = bounds.x + kForwardRoomPickerPadX + kForwardRoomPickerAvatarSize * 0.5f;
        const float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = room->effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
            if (!avatar && owner_.on_room_avatar_needed)
                owner_.on_room_avatar_needed(*room);
        }
        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kForwardRoomPickerAvatarSize,
                    room->name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        const float check_total = kCheckPadR + kCheckSize + kCheckPadR;
        const float text_x = bounds.x + kForwardRoomPickerPadX + kForwardRoomPickerAvatarSize + kForwardRoomPickerAvatarGap;
        const float text_w =
            std::max(0.0f, bounds.x + bounds.w - text_x - check_total);
        tk::TextStyle ns{};
        ns.role      = tk::FontRole::SidebarName;
        ns.trim      = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(
            room->name.empty() ? tk::tr("Unnamed room") : room->name, ns);
        if (name_lo)
        {
            const tk::Size sz = name_lo->measure();
            ctx.canvas.draw_text(
                *name_lo,
                {text_x, bounds.y + (bounds.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }

        const float r  = kCheckSize * 0.5f;
        const float cx = bounds.x + bounds.w - kCheckPadR - r;
        const float cy = bounds.y + bounds.h * 0.5f;
        const tk::Rect check_rect{cx - r, cy - r, kCheckSize, kCheckSize};
        if (checked)
        {
            ctx.canvas.fill_rounded_rect(check_rect, r, ctx.theme.palette.accent);
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Small;
            auto tick = ctx.factory.build_text(std::string("✓"), ts);
            if (tick)
            {
                const tk::Size tsz = tick->measure();
                ctx.canvas.draw_text(
                    *tick, {cx - tsz.w * 0.5f, cy - tsz.h * 0.5f},
                    ctx.theme.palette.text_on_accent);
            }
        }
        else
        {
            ctx.canvas.stroke_rounded_rect(check_rect, r,
                                           ctx.theme.palette.popup_border, 1.5f);
        }
    }

private:
    ForwardRoomPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

ForwardRoomPicker::ForwardRoomPicker()
    : adapter_(std::make_unique<Adapter>(*this))
{
    tk::Widget::set_visible(false);

    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kForwardRoomPickerFieldH);
        field->set_placeholder(tk::tr("Search rooms\xe2\x80\xa6"));
        field->set_on_changed([this](const std::string& q) { set_query(q); });
        field->set_on_submit([this] { confirm(); });
        field->set_visible(false);
        search_field_ = add_child(std::move(field));
    }

    auto list = tk::create_widget<tk::ListView>(this);
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= row_count_())
            return;
        const auto* room = room_at_(static_cast<std::size_t>(idx));
        if (!room)
            return;
        const std::string id = room->id;
        if (selected_ids_.count(id))
        {
            selected_ids_.erase(id);
            selected_order_.erase(
                std::remove_if(selected_order_.begin(), selected_order_.end(),
                               [&id](const auto& r) { return r.id == id; }),
                selected_order_.end());
        }
        else
        {
            selected_ids_.insert(id);
            selected_order_.push_back(*room);
        }
        refilter_();
    };
    list_ = add_child(std::move(list));
}

ForwardRoomPicker::~ForwardRoomPicker() = default;

void ForwardRoomPicker::set_rooms_provider(RoomsProvider p)
{
    rooms_provider_ = std::move(p);
}

void ForwardRoomPicker::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void ForwardRoomPicker::open(const std::string& exclude_room_id)
{
    exclude_room_id_ = exclude_room_id;
    all_rooms_.clear();
    if (rooms_provider_)
    {
        for (auto& r : rooms_provider_())
        {
            if (r.id != exclude_room_id_)
                all_rooms_.push_back(std::move(r));
        }
    }
    query_.clear();
    selected_order_.clear();
    selected_ids_.clear();
    press_outside_ = press_cancel_ = press_confirm_ = false;
    is_open_ = true;
    set_visible(true);
    if (search_field_)
    {
        search_field_->set_visible(true);
        search_field_->set_text("");
        // Deferred to the next paint() rather than focused synchronously
        // here — see pending_focus_'s doc comment.
        pending_focus_ = true;
    }
    refilter_();
}

void ForwardRoomPicker::close()
{
    if (!is_open_)
        return;
    is_open_ = false;
    pending_focus_ = false;
    set_visible(false);
    query_.clear();
    selected_order_.clear();
    selected_ids_.clear();
    press_outside_ = press_cancel_ = press_confirm_ = press_dismiss_ = false;
    forwarding_     = false;
    forward_errors_ = 0;
    forwarding_status_.clear();
    error_lines_.clear();
    if (on_close)
        on_close();
}

void ForwardRoomPicker::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v && search_field_)
        search_field_->set_visible(false);
}

void ForwardRoomPicker::on_theme_changed(const tk::Theme& t)
{
    if (search_field_)
        search_field_->set_text_color(t.palette.text_primary);
}

void ForwardRoomPicker::set_query(const std::string& q)
{
    query_ = q;
    refilter_();
    // set_query() is reached from the native search field's own on_changed
    // callback, which the host never otherwise sees — unlike a click, which
    // gets a free repaint from the host's own pointer-dispatch machinery.
    // Mirrors TabbedGridPicker::refresh_grid()'s identical rationale.
    if (host())
        host()->request_repaint();
}

void ForwardRoomPicker::refilter_()
{
    filtered_unselected_.clear();
    for (const auto& r : all_rooms_)
    {
        if (!selected_ids_.count(r.id) && name_matches(r.name, query_))
            filtered_unselected_.push_back(r);
    }
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(row_count_() == 0 ? -1 : 0);
    }
}

std::size_t ForwardRoomPicker::row_count_() const
{
    return selected_order_.size() + filtered_unselected_.size();
}

const tesseract::RoomInfo* ForwardRoomPicker::room_at_(std::size_t index) const
{
    if (index < selected_order_.size())
        return &selected_order_[index];
    const std::size_t u = index - selected_order_.size();
    if (u < filtered_unselected_.size())
        return &filtered_unselected_[u];
    return nullptr;
}

bool ForwardRoomPicker::is_row_selected_(std::size_t index) const
{
    return index < selected_order_.size();
}

bool ForwardRoomPicker::is_divider_above_(std::size_t index) const
{
    return !selected_order_.empty() && !filtered_unselected_.empty()
           && index == selected_order_.size();
}

void ForwardRoomPicker::move_selection(int delta)
{
    if (!list_ || row_count_() == 0)
        return;
    const int n   = static_cast<int>(row_count_());
    int cur       = list_->selected_index();
    if (cur < 0)
        cur = 0;
    const int next = std::max(0, std::min(n - 1, cur + delta));
    list_->set_selected_index(next);
    list_->scroll_to_index(next);
}

void ForwardRoomPicker::confirm()
{
    if (selected_ids_.empty())
        return;
    std::vector<std::string> ids;
    ids.reserve(selected_order_.size());
    for (const auto& r : selected_order_)
        ids.push_back(r.id);
    if (on_confirmed)
        on_confirmed(std::move(ids));
}

void ForwardRoomPicker::set_forwarding(int room_count)
{
    forwarding_     = true;
    forward_errors_ = 0;
    error_lines_.clear();
    forwarding_status_ = tk::trf(
        tk::trn("Forwarding to {0} room…", "Forwarding to {0} rooms…",
                static_cast<long>(room_count)),
        {std::to_string(room_count)});
}

void ForwardRoomPicker::add_forward_error(const std::string& room_name,
                                          const std::string& detail)
{
    ++forward_errors_;
    error_lines_.push_back(
        tk::trf(tk::tr("Failed to forward to {0}: {1}"), {room_name, detail}));
}

void ForwardRoomPicker::mark_complete()
{
    if (forward_errors_ == 0)
    {
        close();
        return;
    }
    // Errors present — switch to error-display mode; user must dismiss.
    forwarding_status_.clear();
}

// ── Layout + paint ────────────────────────────────────────────────────────

tk::Size ForwardRoomPicker::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void ForwardRoomPicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!list_)
        return;

    constexpr float margin = 40.0f;
    const float cw = std::min(kCardW, std::max(0.0f, bounds.w - 2.0f * margin));
    const float chrome_h = kHeaderH + kFooterH;

    const float list_content = row_count_() == 0
        ? kRowH
        : static_cast<float>(row_count_()) * kRowH;
    const float max_h =
        std::min(kCardMaxH, std::max(0.0f, bounds.h - 2.0f * margin));
    float ch =
        chrome_h + std::min(list_content, std::max(0.0f, max_h - chrome_h));
    ch = std::max(ch, chrome_h + kRowH);
    ch = std::min(ch, max_h);

    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    float cy = bounds.y + (bounds.h - ch) * 0.38f;
    cy = std::max(cy, bounds.y + margin);

    card_rect_         = {cx, cy, cw, ch};
    search_field_rect_ = {cx + kForwardRoomPickerPadX,
                          cy + (kHeaderH - kForwardRoomPickerFieldH) * 0.5f,
                          std::max(0.0f, cw - 2.0f * kForwardRoomPickerPadX),
                          kForwardRoomPickerFieldH};

    if (search_field_)
    {
        // Hidden while a forward is in flight (or errored) — the field
        // would otherwise float above the progress/error body.
        search_field_->set_visible(!forwarding_);
        if (!forwarding_)
            search_field_->arrange(ctx, search_field_rect_);
    }

    // Footer button rects — right-aligned, vertically centred in the footer.
    const float footer_y    = cy + ch - kFooterH;
    const float btn_cy      = footer_y + (kFooterH - kForwardRoomPickerBtnH) * 0.5f;
    const float confirm_w   = 112.0f; // room for "Forward (99)"
    confirm_btn_rect_ = {cx + cw - kForwardRoomPickerPadX - confirm_w, btn_cy, confirm_w, kForwardRoomPickerBtnH};
    cancel_btn_rect_  = {confirm_btn_rect_.x - kForwardRoomPickerBtnGap - kBtnMinW,
                         btn_cy, kBtnMinW, kForwardRoomPickerBtnH};
    // Dismiss button uses same geometry as confirm; shown in error state.
    dismiss_btn_rect_ = confirm_btn_rect_;

    const tk::Rect list_bounds{cx, cy + kHeaderH, cw,
                                std::max(0.0f, ch - chrome_h)};
    list_->arrange(ctx, list_bounds);
}

void ForwardRoomPicker::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;

    // See pending_focus_'s doc comment: open() defers this because the
    // native search field's overlay isn't positioned (arrange() hasn't run
    // yet) at the point open() itself runs. paint() always follows arrange()
    // in the measure/arrange/paint pipeline, so search_field_rect_ (and the
    // native overlay's real geometry) is valid by now.
    if (pending_focus_)
    {
        pending_focus_ = false;
        if (search_field_)
            search_field_->set_focused(true);
    }

    if (forwarding_)
    {
        ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));
        ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f,
                                       ctx.theme.palette.popup_border, 1.0f);
        ctx.canvas.fill_rect(
            {card_rect_.x, card_rect_.y + kHeaderH - 1.0f, card_rect_.w, 1.0f},
            ctx.theme.palette.separator);
        const float footer_y_f = card_rect_.y + card_rect_.h - kFooterH;
        ctx.canvas.fill_rect(
            {card_rect_.x, footer_y_f, card_rect_.w, 1.0f},
            ctx.theme.palette.separator);

        const float body_y_f = card_rect_.y + kHeaderH;
        const float body_h_f = card_rect_.h - kHeaderH - kFooterH;

        if (!forwarding_status_.empty())
        {
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Body;
            auto lo = ctx.factory.build_text(forwarding_status_, ts);
            if (lo)
            {
                const tk::Size sz = lo->measure();
                ctx.canvas.draw_text(
                    *lo,
                    {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                     body_y_f + (body_h_f - sz.h) * 0.5f},
                    ctx.theme.palette.text_primary);
            }
        }
        else if (!error_lines_.empty())
        {
            constexpr float kLineH = 24.0f;
            constexpr float kPad   = 16.0f;
            float ey = body_y_f + kPad;
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Body;
            for (const auto& line : error_lines_)
            {
                auto lo = ctx.factory.build_text(line, ts);
                if (lo)
                    ctx.canvas.draw_text(*lo, {card_rect_.x + kPad, ey},
                                        ctx.theme.palette.destructive);
                ey += kLineH;
            }

            ctx.canvas.fill_rounded_rect(
                dismiss_btn_rect_, kForwardRoomPickerBtnRadius,
                press_dismiss_ ? ctx.theme.palette.sidebar_hover
                               : ctx.theme.palette.compose_card_bg);
            ctx.canvas.stroke_rounded_rect(dismiss_btn_rect_, kForwardRoomPickerBtnRadius,
                                           ctx.theme.palette.border, 1.0f);
            {
                tk::TextStyle bs{};
                bs.role = tk::FontRole::Body;
                auto lo = ctx.factory.build_text(tk::tr("Dismiss"), bs);
                if (lo)
                {
                    const tk::Size sz = lo->measure();
                    ctx.canvas.draw_text(
                        *lo,
                        {dismiss_btn_rect_.x + (dismiss_btn_rect_.w - sz.w) * 0.5f,
                         dismiss_btn_rect_.y + (dismiss_btn_rect_.h - sz.h) * 0.5f},
                        ctx.theme.palette.text_primary);
                }
            }
        }
        return;
    }

    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));

    ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f,
                                   ctx.theme.palette.popup_border, 1.0f);

    ctx.canvas.fill_rect(
        {card_rect_.x, card_rect_.y + kHeaderH - 1.0f, card_rect_.w, 1.0f},
        ctx.theme.palette.separator);
    if (!search_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(search_field_rect_, 6.0f,
                                     ctx.theme.palette.compose_card_bg);
        ctx.canvas.stroke_rounded_rect(search_field_rect_, 6.0f,
                                       ctx.theme.palette.border, 1.0f);
    }

    const float footer_y = card_rect_.y + card_rect_.h - kFooterH;
    ctx.canvas.fill_rect(
        {card_rect_.x, footer_y, card_rect_.w, 1.0f},
        ctx.theme.palette.separator);

    ctx.canvas.fill_rounded_rect(
        cancel_btn_rect_, kForwardRoomPickerBtnRadius,
        press_cancel_ ? ctx.theme.palette.sidebar_hover
                      : ctx.theme.palette.compose_card_bg);
    ctx.canvas.stroke_rounded_rect(cancel_btn_rect_, kForwardRoomPickerBtnRadius,
                                   ctx.theme.palette.border, 1.0f);
    {
        tk::TextStyle cs{};
        cs.role  = tk::FontRole::Body;
        auto lo  = ctx.factory.build_text(tk::tr("Cancel"), cs);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(
                *lo,
                {cancel_btn_rect_.x + (cancel_btn_rect_.w - sz.w) * 0.5f,
                 cancel_btn_rect_.y + (cancel_btn_rect_.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }
    }

    const bool can_forward = !selected_ids_.empty();
    const tk::Color confirm_bg =
        can_forward
            ? (press_confirm_ ? ctx.theme.palette.accent_pressed
                              : ctx.theme.palette.accent)
            : ctx.theme.palette.sidebar_hover;
    ctx.canvas.fill_rounded_rect(confirm_btn_rect_, kForwardRoomPickerBtnRadius, confirm_bg);
    {
        const std::string label =
            can_forward
                ? tk::trf(tk::tr("Forward ({0})"),
                          {std::to_string(selected_ids_.size())})
                : tk::tr("Forward");
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Body;
        auto lo = ctx.factory.build_text(label, cs);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            const tk::Color txt = can_forward ? ctx.theme.palette.text_on_accent
                                              : ctx.theme.palette.text_muted;
            ctx.canvas.draw_text(
                *lo,
                {confirm_btn_rect_.x + (confirm_btn_rect_.w - sz.w) * 0.5f,
                 confirm_btn_rect_.y + (confirm_btn_rect_.h - sz.h) * 0.5f},
                txt);
        }
    }

    if (row_count_() == 0)
    {
        tk::TextStyle es{};
        es.role  = tk::FontRole::Body;
        auto lo  = ctx.factory.build_text(tk::tr("No rooms"), es);
        if (lo)
        {
            const tk::Size sz    = lo->measure();
            const float body_h   = card_rect_.h - kHeaderH - kFooterH;
            ctx.canvas.draw_text(
                *lo,
                {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                 card_rect_.y + kHeaderH + (body_h - sz.h) * 0.5f},
                ctx.theme.palette.text_muted);
        }
        return;
    }

    if (list_ && list_->visible())
    {
        ctx.canvas.push_clip_rounded_rect(card_rect_, 10.0f);
        list_->paint(ctx);
        ctx.canvas.pop_clip();
    }
}

// ── Pointer ───────────────────────────────────────────────────────────────

bool ForwardRoomPicker::on_pointer_down(tk::Point local)
{
    if (!is_open_)
        return false;

    // Stored rects are in world coords; local is widget-local (world - bounds_).
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    auto hit = [](const tk::Rect& r, tk::Point p)
    {
        return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
    };

    if (forwarding_)
    {
        // Only the Dismiss button is active; dismiss only exists when errors shown.
        press_dismiss_ = error_lines_.empty() ? false : hit(dismiss_btn_rect_, world);
        return true;
    }

    press_cancel_  = hit(cancel_btn_rect_, world);
    press_confirm_ = hit(confirm_btn_rect_, world) && !selected_ids_.empty();
    press_outside_ = !press_cancel_ && !press_confirm_ && !hit(card_rect_, world);

    return true; // always consume — modal backdrop
}

void ForwardRoomPicker::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!is_open_)
        return;

    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};

    auto hit = [](const tk::Rect& r, tk::Point p)
    {
        return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
    };

    if (forwarding_)
    {
        if (press_dismiss_ && hit(dismiss_btn_rect_, world))
            close();
        press_dismiss_ = false;
        return;
    }

    if (press_cancel_ && hit(cancel_btn_rect_, world))
    {
        press_cancel_ = false;
        close();
        return;
    }
    if (press_confirm_ && hit(confirm_btn_rect_, world) && !selected_ids_.empty())
    {
        press_confirm_ = false;
        confirm();
        return;
    }
    if (press_outside_ && inside_self)
    {
        press_outside_ = false;
        close();
        return;
    }
    press_cancel_ = press_confirm_ = press_outside_ = false;
}

bool ForwardRoomPicker::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/)
{
    // dispatch_wheel already tried the list child; if we're here the list
    // didn't consume (empty or at boundary).  Modal overlay — eat the event.
    return is_open_;
}

} // namespace tesseract::views
