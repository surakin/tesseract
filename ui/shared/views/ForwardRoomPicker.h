#pragma once

// Forward-message room picker — a centred modal overlay. Paints a dim backdrop
// over the whole surface with a centred card: a search field at the top (host-
// overlaid NativeTextField), a scrollable multi-select room list in the middle,
// and a Cancel / Forward(N) footer. Selected rooms stay pinned at the top of
// the list (above a divider) regardless of the current filter. Confirming fires
// on_confirmed with the ordered list of selected room IDs.
//
// Mounted as the topmost child of MainAppWidget — set_visible(false) by
// default, arranged at full bounds, painted last (highest z-order).

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{

class ForwardRoomPicker : public tk::Widget
{
protected:
    // host() is nullable: when null (e.g. unit tests constructing the
    // picker detached), the search field is skipped — search_field()
    // stays null.
    ForwardRoomPicker();
    TK_WIDGET_FACTORY_FRIEND(ForwardRoomPicker)

public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    using RoomsProvider = std::function<std::vector<tesseract::RoomInfo>()>;

    ~ForwardRoomPicker() override;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    // exclude_room_id: source room to omit from the picker list.
    void open(const std::string& exclude_room_id);
    void close();
    bool is_open() const { return is_open_; }

    // ── Data ──────────────────────────────────────────────────────────────
    void set_rooms_provider(RoomsProvider p);
    void set_avatar_provider(AvatarProvider p);
    // Host pipes NativeTextField text changes in here.
    void set_query(const std::string& q);

    // ── Native-field rect delegation ──────────────────────────────────────
    tk::Rect search_field_rect() const { return search_field_rect_; }
    bool search_field_visible() const { return is_open_ && !forwarding_; }

    // The self-owned search field, or null when constructed without a
    // Host. The shell pushes its Up/Down/Escape handling onto it via
    // push_popup_nav() (Tab/Shift-Tab are already claimed internally).
    tk::TextField* search_field() const
    {
        return search_field_;
    }

    void on_theme_changed(const tk::Theme& t) override;

    // Hiding the picker (close()) doesn't cascade to the search field —
    // tk::Widget::set_visible is deliberately non-virtual/non-cascading —
    // so this shadow hides it explicitly, mirroring QuickSwitcher's idiom.
    void set_visible(bool v);

    // ── Keyboard ──────────────────────────────────────────────────────────
    void move_selection(int delta);
    void confirm(); // forward to all selected rooms

    // ── Async forwarding state ─────────────────────────────────────────────
    // Call set_forwarding() after confirm() fires on_confirmed; the picker
    // stays open showing progress. Call add_forward_error() per failure and
    // mark_complete() when all pending forwards have resolved.
    void set_forwarding(int room_count);
    void add_forward_error(const std::string& room_name,
                           const std::string& detail);
    void mark_complete();

    // ── Callbacks ─────────────────────────────────────────────────────────
    // Fires when the user confirms. room_ids is in selection order.
    std::function<void(std::vector<std::string> room_ids)> on_confirmed;
    // Fires when the overlay should be dismissed (Cancel, Escape, outside click).
    std::function<void()> on_close;
    // Fires from paint for a visible row whose avatar isn't cached yet.
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

    static constexpr float kCardW    = 560.0f;
    static constexpr float kCardMaxH = 520.0f;
    static constexpr float kHeaderH  = 52.0f;
    static constexpr float kFooterH  = 52.0f;
    static constexpr float kRowH     = 44.0f;

private:
    class Adapter;
    friend class Adapter;

    // Total display row count: selected_order_ + filtered_unselected_.
    std::size_t row_count_() const;
    // Room at display index (selected rows first, then filtered unselected).
    const tesseract::RoomInfo* room_at_(std::size_t index) const;
    // Whether display index `index` falls in the selected section.
    bool is_row_selected_(std::size_t index) const;
    // Whether a divider should be drawn above `index` (boundary between sections).
    bool is_divider_above_(std::size_t index) const;

    // Rebuild filtered_unselected_ from all_rooms_ minus selected_ids_,
    // applying query_. Resets the list to the top.
    void refilter_();

    // Set by open(); consumed by the next paint(). Deferred rather than
    // focused synchronously inside open() because arrange() — which
    // positions search_field_'s native overlay via set_rect() — hasn't run
    // yet at that point. Mirrors QuickSwitcher::pending_focus_'s identical
    // rationale.
    bool pending_focus_ = false;

    bool is_open_ = false;
    std::string query_;
    std::string exclude_room_id_;

    // All rooms (minus excluded), fetched on open().
    std::vector<tesseract::RoomInfo> all_rooms_;
    // Selected rooms in selection order (pinned at top of the list).
    std::vector<tesseract::RoomInfo> selected_order_;
    // Fast membership check for selected_order_.
    std::unordered_set<std::string> selected_ids_;
    // Filtered unselected rooms (query-matched, excludes selected_ids_).
    std::vector<tesseract::RoomInfo> filtered_unselected_;

    RoomsProvider  rooms_provider_;
    AvatarProvider avatar_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_ = nullptr;

    tk::Rect card_rect_{};
    tk::Rect search_field_rect_{};
    tk::TextField* search_field_ = nullptr; // owned via add_child when host provided
    tk::Rect cancel_btn_rect_{};
    tk::Rect confirm_btn_rect_{};

    bool press_outside_ = false;
    bool press_cancel_  = false;
    bool press_confirm_ = false;
    bool press_dismiss_ = false;

    bool                     forwarding_      = false;
    int                      forward_errors_  = 0;
    std::string              forwarding_status_;
    std::vector<std::string> error_lines_;
    tk::Rect                 dismiss_btn_rect_{};
};

} // namespace tesseract::views
