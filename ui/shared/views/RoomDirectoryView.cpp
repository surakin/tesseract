#include "RoomDirectoryView.h"

#include "media_utils.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <optional>
#include <string>

namespace tesseract::views
{

namespace
{

constexpr float kPadX = 20.0f;
constexpr float kPadY = 16.0f;
constexpr float kGap = 10.0f;
constexpr float kDirSmallGap = 6.0f;
constexpr float kDirInputH = 32.0f;
// Two lines' worth at Body role — long messages (e.g. "invalid server
// name: …") wrap instead of being clipped or overflowing the button row.
constexpr float kDirStatusH = 40.0f;
constexpr float kBtnH = 32.0f;
constexpr float kBtnW = 96.0f;
constexpr float kSearchBtnW = 88.0f;
constexpr float kRadius = 6.0f;
constexpr float kBorderW = 1.0f;

constexpr float kRowH = 84.0f;
constexpr float kRowPad = 12.0f;
constexpr float kRowAvatarD = 44.0f;
constexpr float kRowPillH = 18.0f;
constexpr float kRowPillPadX = 8.0f;

} // namespace

// ── Adapter ─────────────────────────────────────────────────────────────

class RoomDirectoryView::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(RoomDirectoryView& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.items_.size();
    }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    std::string row_key(std::size_t index) const override
    {
        return index < owner_.items_.size() ? owner_.items_[index].room_id
                                            : std::string{};
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        if (index >= owner_.items_.size())
        {
            return;
        }
        const auto& e = owner_.items_[index];
        const auto& pal = ctx.theme.palette;

        if (selected)
        {
            ctx.canvas.fill_rect(bounds, pal.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(bounds, pal.sidebar_hover);
        }

        const tk::Point av_centre{bounds.x + kRowPad + kRowAvatarD * 0.5f,
                                  bounds.y + bounds.h * 0.5f};
        const tk::Image* img = nullptr;
        if (owner_.avatar_provider_ && !e.avatar_url.empty())
        {
            img = owner_.avatar_provider_(e.avatar_url);
            if (!img && owner_.on_avatar_needed)
            {
                owner_.on_avatar_needed(e.avatar_url);
            }
        }
        std::string_view disp =
            e.name.empty() ? std::string_view("#") : std::string_view(e.name);
        draw_avatar(ctx.canvas, img, av_centre, kRowAvatarD, disp, pal.accent,
                    tk::Color{255, 255, 255, 255});

        const float text_x = bounds.x + kRowPad + kRowAvatarD + kRowPad;
        const float text_w = bounds.x + bounds.w - kRowPad - text_x;
        float y = bounds.y + 10.0f;

        // Room name.
        {
            tk::TextStyle ts;
            ts.role = tk::FontRole::Title;
            ts.halign = tk::TextHAlign::Leading;
            ts.trim = tk::TextTrim::Ellipsis;
            ts.max_width = text_w;
            auto lo = ctx.factory.build_text(
                e.name.empty() ? e.room_id : e.name, ts);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {text_x, y}, pal.text_primary);
                y += lo->measure().h + 4.0f;
            }
        }

        // Join-rule pill + member count.
        {
            const std::string label = join_rule_label(e.join_rule);
            const tk::Color bg = join_rule_bg(e.join_rule);
            tk::TextStyle pts;
            pts.role = tk::FontRole::Small;
            auto plo = ctx.factory.build_text(label, pts);
            float advance = 0.0f;
            if (plo)
            {
                const float pw = plo->measure().w + kRowPillPadX * 2.0f;
                const tk::Rect pill{text_x, y, pw, kRowPillH};
                ctx.canvas.fill_rounded_rect(pill, kRowPillH * 0.5f, bg);
                ctx.canvas.draw_text(
                    *plo,
                    {text_x + kRowPillPadX,
                     y + (kRowPillH - plo->measure().h) * 0.5f},
                    tk::Color{255, 255, 255, 255});
                advance = pw + kDirSmallGap;
            }
            const std::string members = tk::trf(
                tk::trn("{0} member", "{0} members",
                        static_cast<long>(e.joined_members)),
                {std::to_string(e.joined_members)});
            tk::TextStyle mts;
            mts.role = tk::FontRole::Small;
            mts.halign = tk::TextHAlign::Leading;
            auto mlo = ctx.factory.build_text(members, mts);
            if (mlo)
            {
                ctx.canvas.draw_text(
                    *mlo,
                    {text_x + advance,
                     y + (kRowPillH - mlo->measure().h) * 0.5f},
                    pal.text_secondary);
            }
            y += kRowPillH + 4.0f;
        }

        // Topic (single line, ellipsized).
        if (!e.topic.empty())
        {
            tk::TextStyle ts;
            ts.role = tk::FontRole::Small;
            ts.halign = tk::TextHAlign::Leading;
            ts.trim = tk::TextTrim::Ellipsis;
            ts.max_width = text_w;
            auto lo = ctx.factory.build_text(e.topic, ts);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {text_x, y}, pal.text_secondary);
            }
        }
    }

private:
    RoomDirectoryView& owner_;
};

// ── RoomDirectoryView ───────────────────────────────────────────────────

RoomDirectoryView::RoomDirectoryView() : adapter_(std::make_unique<Adapter>(*this))
{
    if (host())
    {
        auto server = tk::create_widget<tk::TextField>(this, kDirInputH);
        server->set_on_changed(
            [this](const std::string&)
            {
                fields_dirty_ = true;
                if (on_field_edited) on_field_edited();
            });
        server->set_on_submit([this] { search_now(); });
        // Losing focus commits immediately — the debounce (on_field_edited,
        // wired by the host) is for the "still typing" case; tabbing/
        // clicking away means the user is done editing. Gated on
        // fields_dirty_ so clicking a result row (which blurs whichever
        // field was focused) doesn't restart an unedited search and reset
        // the list's scroll position.
        server->set_on_focus_changed(
            [this](bool focused) { if (!focused && fields_dirty_) search_now(); });
        server_field_ = add_child(std::move(server));

        auto search = tk::create_widget<tk::TextField>(this, kDirInputH);
        search->set_placeholder(tk::tr("Search rooms\xe2\x80\xa6"));
        search->set_on_changed(
            [this](const std::string&)
            {
                fields_dirty_ = true;
                if (on_field_edited) on_field_edited();
            });
        search->set_on_submit([this] { search_now(); });
        search->set_on_focus_changed(
            [this](bool focused) { if (!focused && fields_dirty_) search_now(); });
        search_field_ = add_child(std::move(search));
    }

    // Explicit button — Enter-to-submit alone isn't discoverable; mirrors
    // JoinRoomView's "Look up" button for the same reason.
    auto search_btn = tk::create_widget<tk::Button>(
        this, tk::tr("Search"), std::function<void()>{},
        tk::Button::Variant::Primary);
    search_btn->set_on_click([this] { search_now(); });
    search_btn_ = add_child(std::move(search_btn));

    auto list = tk::create_widget<tk::ListView>(this);
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx) { select_row_(idx); };
    list->on_near_bottom = [this] { request_next_page_(); };
    list_view_ = add_child(std::move(list));

    auto join = tk::create_widget<tk::Button>(
        this, tk::tr("Join"), std::function<void()>{},
        tk::Button::Variant::Primary);
    join->set_on_click(
        [this]
        {
            if (joining_ || selected_index_ < 0 ||
                static_cast<std::size_t>(selected_index_) >= items_.size())
            {
                return;
            }
            joining_ = true;
            if (join_btn_) join_btn_->set_enabled(false);
            if (status_lbl_)
            {
                status_lbl_->set_text(tk::tr("Joining room\xe2\x80\xa6"));
                status_lbl_->set_visible(true);
            }
            if (on_join_requested)
            {
                on_join_requested(
                    items_[static_cast<std::size_t>(selected_index_)].room_id,
                    active_server_);
            }
        });
    join->set_enabled(false);
    join_btn_ = add_child(std::move(join));

    auto cancel = tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    cancel->set_on_click([this] { if (on_cancel) on_cancel(); });
    cancel_btn_ = add_child(std::move(cancel));

    auto status = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
    status->set_halign(tk::TextHAlign::Center);
    status->set_wrap(true);
    status->set_visible(false);
    status_lbl_ = add_child(std::move(status));
}

RoomDirectoryView::~RoomDirectoryView() = default;

void RoomDirectoryView::open()
{
    is_open_ = true;
    set_visible(true);
    if (!has_searched_)
    {
        search_now();
        return;
    }
    // Reopening with cached results (tab switched away and back) — clear
    // whatever transient status text is still showing from before (a
    // leftover "Joining room…", or an error from a join that failed the
    // last time this tab was open), without discarding the cached list or
    // triggering a new search. Mirrors set_results()'s own empty/non-empty
    // status handling.
    joining_ = false;
    error_msg_.clear();
    if (join_btn_)
    {
        join_btn_->set_enabled(selected_index_ >= 0 &&
                               static_cast<std::size_t>(selected_index_) < items_.size());
    }
    if (status_lbl_)
    {
        if (items_.empty())
        {
            status_lbl_->set_text(tk::tr("No rooms found."));
            status_lbl_->set_visible(true);
        }
        else
        {
            status_lbl_->set_visible(false);
        }
    }
}

void RoomDirectoryView::close()
{
    if (!is_open_)
    {
        return;
    }
    is_open_ = false;
    set_visible(false);
    if (on_close)
    {
        on_close();
    }
}

void RoomDirectoryView::set_visible(bool v)
{
    tk::Widget::set_visible(v);
    if (!v)
    {
        if (server_field_) server_field_->set_visible(false);
        if (search_field_) search_field_->set_visible(false);
    }
}

void RoomDirectoryView::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void RoomDirectoryView::set_is_room_joined(
    std::function<bool(const std::string&)> p)
{
    is_room_joined_ = std::move(p);
    update_join_button_label_();
}

void RoomDirectoryView::set_homeserver_placeholder(std::string domain)
{
    homeserver_placeholder_ = std::move(domain);
    if (server_field_)
    {
        server_field_->set_placeholder(homeserver_placeholder_.empty()
                                           ? std::string(tk::tr("Homeserver"))
                                           : homeserver_placeholder_);
    }
}

void RoomDirectoryView::search_now()
{
    joining_ = false;
    fields_dirty_ = false;
    has_searched_ = true;
    loading_ = true;
    reached_end_ = true; // until the first page reports otherwise
    error_msg_.clear();
    items_.clear();
    selected_index_ = -1;
    if (list_view_)
    {
        list_view_->invalidate_data();
        list_view_->scroll_to_top();
    }
    if (join_btn_) join_btn_->set_enabled(false);
    update_join_button_label_();
    if (status_lbl_)
    {
        status_lbl_->set_text(tk::tr("Loading rooms\xe2\x80\xa6"));
        status_lbl_->set_visible(true);
    }
    active_request_id_ = ++next_request_id_;
    active_server_ = server_field_ ? server_field_->text() : std::string{};
    if (on_search_requested)
    {
        const std::string filter = search_field_ ? search_field_->text() : std::string{};
        on_search_requested(active_request_id_, filter, active_server_);
    }
}

void RoomDirectoryView::request_next_page_()
{
    if (loading_ || reached_end_ || !has_searched_)
    {
        return;
    }
    loading_ = true;
    if (on_next_page_requested)
    {
        on_next_page_requested(active_request_id_);
    }
}

void RoomDirectoryView::select_row_(int idx)
{
    // A join is in flight for the previous selection — don't let picking a
    // different row re-enable the button underneath it.
    if (joining_)
    {
        return;
    }
    if (idx < 0 || static_cast<std::size_t>(idx) >= items_.size())
    {
        return;
    }
    selected_index_ = idx;
    if (list_view_) list_view_->set_selected_index(idx);
    if (join_btn_) join_btn_->set_enabled(true);
    update_join_button_label_();
}

void RoomDirectoryView::set_join_failed(std::string message)
{
    joining_ = false;
    if (join_btn_)
    {
        join_btn_->set_enabled(selected_index_ >= 0 &&
                               static_cast<std::size_t>(selected_index_) < items_.size());
    }
    if (status_lbl_)
    {
        status_lbl_->set_text(message);
        status_lbl_->set_visible(!message.empty());
    }
    error_msg_ = std::move(message);
}

void RoomDirectoryView::update_join_button_label_()
{
    if (!join_btn_) return;
    const bool joined =
        selected_index_ >= 0 &&
        static_cast<std::size_t>(selected_index_) < items_.size() &&
        is_room_joined_ &&
        is_room_joined_(items_[static_cast<std::size_t>(selected_index_)].room_id);
    join_btn_->set_label(joined ? tk::tr("Go") : tk::tr("Join"));
}

void RoomDirectoryView::set_results(std::uint64_t request_id,
                                    std::vector<tesseract::RoomDirectoryEntry> entries,
                                    bool reached_end)
{
    if (request_id != active_request_id_)
    {
        return; // stale response — a newer search has since been issued
    }
    loading_ = false;
    reached_end_ = reached_end;
    error_msg_.clear();
    items_.insert(items_.end(), std::make_move_iterator(entries.begin()),
                 std::make_move_iterator(entries.end()));
    if (list_view_) list_view_->invalidate_data();
    if (status_lbl_)
    {
        if (items_.empty())
        {
            status_lbl_->set_text(tk::tr("No rooms found."));
            status_lbl_->set_visible(true);
        }
        else
        {
            status_lbl_->set_visible(false);
        }
    }
}

void RoomDirectoryView::set_search_failed(std::uint64_t request_id,
                                          std::string message)
{
    if (request_id != active_request_id_)
    {
        return;
    }
    loading_ = false;
    error_msg_ = std::move(message);
    if (status_lbl_)
    {
        status_lbl_->set_text(error_msg_);
        status_lbl_->set_visible(true);
    }
}

bool RoomDirectoryView::join_button_enabled() const
{
    return join_btn_ && join_btn_->visible() && join_btn_->enabled();
}

void RoomDirectoryView::trigger_join_for_test()
{
    if (join_btn_) join_btn_->click();
}

void RoomDirectoryView::on_theme_changed(const tk::Theme& t)
{
    if (server_field_) server_field_->set_text_color(t.palette.text_primary);
    if (search_field_) search_field_->set_text_color(t.palette.text_primary);
}

tk::Size RoomDirectoryView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, constraints.h};
}

void RoomDirectoryView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const float x = bounds.x + kPadX;
    const float w = bounds.w - kPadX * 2.0f;
    const float fields_w = w - kSearchBtnW - kGap * 2.0f;
    const float field_w = (fields_w - kGap) * 0.5f;
    const float y0 = bounds.y + kPadY;

    if (server_field_)
    {
        server_field_->set_visible(is_open_);
        server_field_->arrange(ctx, {x, y0, field_w, kDirInputH});
    }
    if (search_field_)
    {
        search_field_->set_visible(is_open_);
        search_field_->arrange(ctx, {x + field_w + kGap, y0, field_w, kDirInputH});
    }
    if (search_btn_)
    {
        search_btn_->arrange(
            ctx, {x + field_w * 2.0f + kGap * 2.0f, y0, kSearchBtnW, kDirInputH});
    }

    const float btn_row_y = bounds.y + bounds.h - kPadY - kBtnH;
    const float status_y = btn_row_y - kDirSmallGap - kDirStatusH;
    const float list_y = y0 + kDirInputH + kGap;
    const float list_h = std::max(0.0f, status_y - kGap - list_y);

    if (list_view_)
    {
        list_view_->arrange(ctx, {x, list_y, w, list_h});
    }
    if (status_lbl_)
    {
        status_lbl_->arrange(ctx, {x, status_y, w, kDirStatusH});
    }

    float btn_x = bounds.x + bounds.w - kPadX - kBtnW;
    if (join_btn_)
    {
        join_btn_->arrange(ctx, {btn_x, btn_row_y, kBtnW, kBtnH});
    }
    btn_x -= (kBtnW + kDirSmallGap);
    if (cancel_btn_)
    {
        cancel_btn_->arrange(ctx, {btn_x, btn_row_y, kBtnW, kBtnH});
    }
}

void RoomDirectoryView::paint(tk::PaintCtx& ctx)
{
    if (!visible())
    {
        return;
    }
    const auto& pal = ctx.theme.palette;

    if (server_field_ && server_field_->visible() && !server_field_->bounds().empty())
    {
        ctx.canvas.fill_rounded_rect(server_field_->bounds(), kRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(server_field_->bounds(), kRadius,
                                       pal.border, kBorderW);
        server_field_->paint(ctx);
    }
    if (search_field_ && search_field_->visible() && !search_field_->bounds().empty())
    {
        ctx.canvas.fill_rounded_rect(search_field_->bounds(), kRadius, pal.bg);
        ctx.canvas.stroke_rounded_rect(search_field_->bounds(), kRadius,
                                       pal.border, kBorderW);
        search_field_->paint(ctx);
    }
    if (search_btn_)
    {
        search_btn_->paint(ctx);
    }
    if (list_view_)
    {
        list_view_->paint(ctx);
    }
    if (status_lbl_ && status_lbl_->visible())
    {
        status_lbl_->set_colour(!error_msg_.empty()
                                    ? std::optional<tk::Color>(pal.destructive)
                                    : std::nullopt);
        status_lbl_->paint(ctx);
    }
    if (join_btn_ && join_btn_->visible())
    {
        join_btn_->paint(ctx);
    }
    if (cancel_btn_ && cancel_btn_->visible())
    {
        cancel_btn_->paint(ctx);
    }
}

} // namespace tesseract::views
