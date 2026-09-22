#include "SpaceRootView.h"
#include "html_spans.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

// Autolinked (plain-text URL/matrix.to) topic display, scrollable via the
// owning tk::ScrollView. tk::Label has no rich-text/link support, so this is
// a small dedicated widget instead — mirrors RoomInfoPanel's own topic-link
// handling (autolink_plain_to_spans + build_rich_text + link_at hit-testing)
// at a much smaller scope (no HTML topic support needed here; SpaceRootView
// never had it, and adding it is out of scope for this fix).
class SpaceRootView::TopicLinkLabel : public tk::Widget
{
protected:
    TopicLinkLabel() = default;
    TK_WIDGET_FACTORY_FRIEND(TopicLinkLabel)

public:
    void set_topic(std::string topic)
    {
        if (topic == topic_)
            return;
        topic_ = std::move(topic);
        spans_ = autolink_plain_to_spans(topic_);
        layout_.reset();
    }

    void set_colour(tk::Color c)
    {
        colour_ = c;
    }

    std::function<void(std::string)> on_link_clicked;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override
    {
        ensure_layout_(ctx.factory, constraints.w);
        return layout_ ? layout_->measure() : tk::Size{};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        ensure_layout_(ctx.factory, bounds_.w);
        if (layout_)
            ctx.canvas.draw_text(*layout_, {bounds_.x, bounds_.y}, colour_);
    }

    bool on_pointer_down(tk::Point local) override
    {
        press_url_ = layout_ ? layout_->link_at(local) : std::string{};
        return true; // consume regardless — a scrollable text block with no
                     // link under the press still shouldn't leak the click.
    }

    void on_pointer_up(tk::Point local, bool inside_self) override
    {
        if (inside_self && !press_url_.empty() && layout_ &&
            layout_->link_at(local) == press_url_ && on_link_clicked)
        {
            on_link_clicked(press_url_);
        }
        press_url_.clear();
    }

    // Fired whenever the hovered link changes (including to/from empty) so
    // the shell can swap the native cursor — mirrors RoomInfoPanel's
    // identical on_link_hovered mechanism (cursor-setting is platform-
    // specific, wired per shell, not centrally).
    std::function<void(std::string)> on_link_hovered;

    bool on_pointer_move(tk::Point local) override
    {
        const std::string url = layout_ ? layout_->link_at(local) : std::string{};
        if (url != hover_url_)
        {
            hover_url_ = url;
            if (on_link_hovered) on_link_hovered(hover_url_);
        }
        return false;
    }

    void on_pointer_leave() override
    {
        if (!hover_url_.empty())
        {
            hover_url_.clear();
            if (on_link_hovered) on_link_hovered(hover_url_);
        }
    }

private:
    void ensure_layout_(tk::CanvasFactory& factory, float w)
    {
        if (layout_ && cached_w_ == w)
            return;
        cached_w_ = w;
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        st.wrap = true;
        st.max_width = w;
        // autolink_plain_to_spans() returns an empty vector when the topic
        // has no links at all (not a single plain-text span) — fall back to
        // a plain build_text so a link-free topic still renders, matching
        // RoomInfoPanel's identical fallback for the same helper.
        layout_ = spans_.empty() ? factory.build_text(topic_, st)
                                  : factory.build_rich_text(spans_, st);
    }

    std::string topic_;
    std::vector<tk::TextSpan> spans_;
    std::unique_ptr<tk::TextLayout> layout_;
    float cached_w_ = -1.0f;
    tk::Color colour_{};
    std::string press_url_;
    std::string hover_url_;
};

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

    leave_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "\xF0\x9F\x9A\xAA", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    leave_btn_->set_accessible_name(tk::tr("Leave Space"));
    leave_btn_->set_icon(kLeaveRoomSvg, 16.0f);
    leave_btn_->set_on_click([this]() {
        if (!space_) return;
        if (on_leave_space) on_leave_space(space_->id);
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

    auto add_list = tk::create_widget<SpaceAddRoomList>(this);
    add_list->on_add_requested = [this](std::string room_id)
    {
        if (on_add_room_to_space && space_)
            on_add_room_to_space(space_->id, std::move(room_id));
    };
    add_list->on_room_dropped_for_remove = [this](std::string room_id)
    {
        if (on_remove_room_from_space && space_)
            on_remove_room_from_space(space_->id, std::move(room_id));
    };
    add_list->on_room_avatar_needed = [this](const tesseract::RoomInfo& room)
    {
        if (on_room_avatar_needed) on_room_avatar_needed(room);
    };
    add_list_ = add_child(std::move(add_list));

    auto child_grid = tk::create_widget<SpaceChildRoomGrid>(this);
    child_grid->on_remove_requested = [this](std::string room_id)
    {
        if (on_remove_room_from_space && space_)
            on_remove_room_from_space(space_->id, std::move(room_id));
    };
    child_grid->on_room_dropped_for_add = [this](std::string room_id)
    {
        if (on_add_room_to_space && space_)
            on_add_room_to_space(space_->id, std::move(room_id));
    };
    child_grid->on_unjoined_summary_needed = [this](std::string room_id)
    {
        if (on_child_summary_needed) on_child_summary_needed(std::move(room_id));
    };
    child_grid->on_room_avatar_needed = [this](const tesseract::RoomInfo& room)
    {
        if (on_room_avatar_needed) on_room_avatar_needed(room);
    };
    child_grid_ = add_child(std::move(child_grid));

    auto topic_scroll = tk::create_widget<tk::ScrollView>(this);
    topic_scroll->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
    auto topic_label = tk::create_widget<TopicLinkLabel>(topic_scroll.get());
    topic_label->on_link_clicked = [this](std::string url)
    {
        if (on_link_clicked) on_link_clicked(std::move(url));
    };
    topic_label->on_link_hovered = [this](std::string url)
    {
        if (on_link_hovered) on_link_hovered(std::move(url));
    };
    topic_label_ = topic_label.get();
    topic_scroll->set_child(std::move(topic_label));
    topic_scroll_ = add_child(std::move(topic_scroll));

    set_visible(false);
}

void SpaceRootView::set_candidate_rooms_provider(RoomsProvider p)
{
    if (add_list_) add_list_->set_rooms_provider(std::move(p));
}

void SpaceRootView::set_children(std::vector<SpaceChildRoomGrid::ChildRoomEntry> children)
{
    std::vector<std::string> excluded;
    excluded.reserve(children.size() + 1);
    if (space_) excluded.push_back(space_->id);
    for (const auto& c : children)
        excluded.push_back(c.info.id);

    if (child_grid_) child_grid_->set_children(std::move(children));
    if (add_list_)
    {
        add_list_->set_excluded_room_ids(std::move(excluded));
        add_list_->refresh();
    }
}

void SpaceRootView::set_can_manage_children(bool v)
{
    can_manage_children_ = v;
    if (add_list_) add_list_->set_can_manage(v);
    if (child_grid_) child_grid_->set_can_manage(v);
}

void SpaceRootView::set_space(const tesseract::RoomInfo& space,
                              std::size_t joined_children,
                              std::size_t unjoined_children)
{
    const bool space_changed = !space_ || space_->id != space.id;
    if (space_changed && settings_view_ && settings_view_->is_open())
        settings_view_->close();
    if (space_changed && topic_scroll_)
        topic_scroll_->scroll_to_top();

    space_ = space;
    joined_children_ = joined_children;
    unjoined_children_ = unjoined_children;
    if (topic_label_)
        topic_label_->set_topic(space.topic);
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
    if (add_list_) add_list_->set_avatar_provider(p);
    if (child_grid_) child_grid_->set_avatar_provider(p);
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
    if (leave_btn_)
        leave_btn_->set_visible(!settings_open);
    // Explicitly toggled (not just left unpainted) so dispatch_pointer_down
    // can't still reach them while settings_view_ covers the whole panel, or
    // while the user lacks permission to edit this space's children at all
    // (can_manage_children_) — mirrors settings_btn_/leave_btn_ above.
    const bool show_management = !settings_open && can_manage_children_;
    if (add_list_)
        add_list_->set_visible(show_management);
    if (child_grid_)
        child_grid_->set_visible(show_management);
    if (topic_scroll_)
        topic_scroll_->set_visible(!settings_open);

    if (settings_open)
    {
        settings_view_->arrange(ctx, bounds);
        return;
    }

    constexpr float kBtnSz = 32.0f;
    if (settings_btn_)
        settings_btn_->arrange(ctx, {bounds.x + 8.0f, bounds.y + 8.0f, kBtnSz, kBtnSz});
    if (leave_btn_)
        leave_btn_->arrange(ctx,
            {bounds.x + bounds.w - 8.0f - kBtnSz, bounds.y + 8.0f, kBtnSz, kBtnSz});
}

void SpaceRootView::reset_layouts_()
{
    name_layout_.reset();
    alias_layout_.reset();
    meta_layout_.reset();
    hint_layout_.reset();
    factory_seen_ = nullptr;
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

bool SpaceRootView::on_pointer_down(tk::Point)
{
    return true;
}

// Intentional paint() override: this is a conditional either/or between two
// entirely different rendering modes (delegate solely to settings_view_ and
// return, vs. paint this view's own summary content and settings_btn_),
// not chrome wrapped strictly before/after all children's paint() calls, so
// it can't be expressed via paint_before_children()/paint_after_children().
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

    // Top row: avatar + name + alias on the left, topic/description on the
    // right — two independent columns, not a single stacked block, so a
    // long topic can never push down into (or overlap) the room-management
    // section below regardless of how many lines it wraps to. Falls back to
    // stacked (avatar block above, topic below) on a narrow panel, mirroring
    // the room-management section's own side-by-side/stacked breakpoint.
    const float top_row_x = bounds_.x + kSectionPadX;
    const float top_row_w = std::max(0.0f, bounds_.w - 2.0f * kSectionPadX);
    const bool top_row_side_by_side = top_row_w >= kStackBreakpoint;
    const float left_col_w = top_row_side_by_side
                                 ? std::min(kLeftColumnW, top_row_w * 0.5f)
                                 : top_row_w;
    const float topic_col_w =
        top_row_side_by_side
            ? std::max(0.0f, top_row_w - left_col_w - kSectionGap)
            : top_row_w;
    // Fixed budget for the avatar/name/alias column regardless of window
    // height — matches kSectionMinH's identical "known constant, not
    // text-dependent" rationale below. When stacked, the topic gets its own
    // separate (smaller) fixed budget underneath instead of sharing this one.
    const float avatar_col_h = kAvatarD + kGap + kNameH + kGap * 0.5f +
                               (s.canonical_alias.empty() ? 0.0f : kAliasH + kGap * 0.5f);
    const float top_row_h = top_row_side_by_side
                                ? std::max(avatar_col_h, kTopRowMinH)
                                : avatar_col_h + kGap + kTopicMaxH;

    if (!name_layout_)
    {
        tk::TextStyle name_style{};
        name_style.role = tk::FontRole::Title;
        name_style.trim = tk::TextTrim::Ellipsis;
        name_style.max_width = left_col_w;
        const std::string& nm = s.name.empty() ? s.id : s.name;
        name_layout_ = ctx.factory.build_text(nm, name_style);

        if (!s.canonical_alias.empty())
        {
            tk::TextStyle alias_style{};
            alias_style.role = tk::FontRole::Body;
            alias_style.trim = tk::TextTrim::Ellipsis;
            alias_style.max_width = left_col_w;
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

    const float section_top = bounds_.y + kPadY + top_row_h + kGap;
    const float section_bottom =
        std::max(section_top + kSectionMinH, bottom_block_y - kGap);

    const float cx = bounds_.x + (bounds_.w - content_w) * 0.5f;

    const float av_cx = top_row_x + left_col_w * 0.5f;
    float cy = bounds_.y + kPadY;

    const tk::Point av_centre{av_cx, cy + kAvatarD * 0.5f};
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
        cv.draw_text(*name_layout_, {top_row_x + (left_col_w - nm.w) * 0.5f, cy},
                     pal.text_primary);
        cy += nm.h + kGap * 0.5f;
    }

    if (alias_layout_)
    {
        const auto al = alias_layout_->measure();
        cv.draw_text(*alias_layout_, {top_row_x + (left_col_w - al.w) * 0.5f, cy},
                     pal.text_secondary);
        cy += al.h + kGap * 0.5f;
    }

    if (topic_scroll_)
    {
        const float topic_x = top_row_side_by_side ? top_row_x + left_col_w + kSectionGap
                                                    : top_row_x;
        const float topic_y = top_row_side_by_side ? bounds_.y + kPadY
                                                    : cy + kGap;
        // A real ScrollView + Label child, not a manually clipped/height-
        // capped TextLayout — the topic can be arbitrarily long; if it
        // overflows topic_h_budget the user scrolls it instead of it either
        // truncating or bleeding into the room-management section
        // below/beside it. With no management section to share space with
        // (no edit permission), there's nothing below/beside to protect —
        // let the topic grow all the way down to the bottom block instead
        // of the fixed top-row/kTopicMaxH budget.
        const float topic_h_budget =
            can_manage_children_
                ? (top_row_side_by_side ? top_row_h : kTopicMaxH)
                : std::max(kTopicMaxH, bottom_block_y - kGap - topic_y);
        if (topic_label_)
            topic_label_->set_colour(pal.text_secondary);
        tk::LayoutCtx layout_ctx{ctx.factory, ctx.theme};
        topic_scroll_->arrange(layout_ctx, {topic_x, topic_y, topic_col_w, topic_h_budget});
        topic_scroll_->paint(ctx);
    }

    // Room-management section: SpaceAddRoomList (left) + SpaceChildRoomGrid
    // (right), side by side when there's room, stacked otherwise. Arranged
    // here (not in arrange()) because this view computes all of its content
    // geometry at paint time — see the class comment on that pattern — and
    // only re-arranged on actual change so a real child widget's arrange()
    // isn't paid for on every single frame.
    if (add_list_ && child_grid_ && can_manage_children_)
    {
        const float section_x = bounds_.x + kSectionPadX;
        const float section_w = std::max(0.0f, bounds_.w - 2.0f * kSectionPadX);

        float lists_top = section_top;
        {
            tk::TextStyle ls{};
            ls.role = tk::FontRole::Small;
            ls.wrap = true;
            ls.max_width = section_w;
            if (auto label_lo = ctx.factory.build_text(
                    tk::tr("Drag rooms between the two lists to add or remove "
                          "them, or select one and press Enter to add / "
                          "Delete to remove."),
                    ls))
            {
                cv.draw_text(*label_lo, {section_x, section_top}, pal.text_muted);
                lists_top += label_lo->measure().h + kSectionLabelGap;
            }
        }

        const tk::Rect section{section_x, lists_top, section_w,
                               std::max(0.0f, section_bottom - lists_top)};

        tk::Rect left_rect;
        tk::Rect right_rect;
        if (section.w >= kStackBreakpoint)
        {
            const float left_w = std::min(kLeftColumnW, section.w * 0.5f);
            left_rect = {section.x, section.y, left_w, section.h};
            right_rect = {section.x + left_w + kSectionGap, section.y,
                         std::max(0.0f, section.w - left_w - kSectionGap),
                         section.h};
        }
        else
        {
            const float half_h = std::max(0.0f, (section.h - kSectionGap) * 0.5f);
            left_rect = {section.x, section.y, section.w, half_h};
            right_rect = {section.x, section.y + half_h + kSectionGap,
                         section.w, half_h};
        }

        tk::LayoutCtx layout_ctx{ctx.factory, ctx.theme};
        if (left_rect.x != last_left_rect_.x || left_rect.y != last_left_rect_.y ||
            left_rect.w != last_left_rect_.w || left_rect.h != last_left_rect_.h)
        {
            add_list_->arrange(layout_ctx, left_rect);
            last_left_rect_ = left_rect;
        }
        if (right_rect.x != last_right_rect_.x || right_rect.y != last_right_rect_.y ||
            right_rect.w != last_right_rect_.w || right_rect.h != last_right_rect_.h)
        {
            child_grid_->arrange(layout_ctx, right_rect);
            last_right_rect_ = right_rect;
        }

        add_list_->paint(ctx);
        child_grid_->paint(ctx);
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

    if (leave_btn_) leave_btn_->paint(ctx);
}

} // namespace tesseract::views
