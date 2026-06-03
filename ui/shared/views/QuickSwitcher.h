#pragma once

// Ctrl+K quick switcher — a centred command-palette overlay. Paints a dim
// backdrop over the whole surface with a centred card: a search field at the
// top (a host-overlaid NativeTextField, positioned from search_field_rect())
// and a scrollable room list below. The list shows recent rooms when the query
// is empty and name-filtered rooms as the user types. Enter / click jumps to
// the selected room; Up/Down navigate; Escape (routed by the shell) closes.
//
// Mounted as the topmost child of MainAppWidget — set_visible(false) by
// default, arranged at full bounds, painted last (highest z-order). The inner
// tk::ListView is a child so it handles row clicks + wheel scrolling via the
// normal dispatch path; this widget only adds the backdrop / click-outside
// behaviour and keyboard-driven selection.
//
// Avatar bytes live in a host-side cache; pass the same provider lambda the
// rest of the app uses (returns a decoded tk::Image* for an mxc_url, or
// nullptr for the initials-disc fallback).

#include "tk/canvas.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class QuickSwitcher : public tk::Widget
{
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
    // Returns the room list to show, already in the order the switcher should
    // display it (the shell sorts most-recent-first). Pulled fresh on open().
    using RoomsProvider = std::function<std::vector<tesseract::RoomInfo>()>;

    QuickSwitcher();
    ~QuickSwitcher() override; // out-of-line — Adapter is opaque here

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open();
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // ── Data + query ──────────────────────────────────────────────────────
    void set_rooms_provider(RoomsProvider p);
    // Recent (most-recently-visited) rooms, in visit order — feeds the
    // horizontal "Recent" strip shown when the query is empty.
    void set_recent_provider(RoomsProvider p);
    // Host pipes its NativeTextField's text changes in here.
    void set_query(const std::string& q);
    void set_avatar_provider(AvatarProvider p);

    // ── Keyboard navigation (driven by the field's popup-nav callback) ────
    void move_selection(int delta);
    void activate_selected();

    // ── Native-field rect delegation (mirrors RoomListView::search_field_rect)
    tk::Rect search_field_rect() const
    {
        return search_field_rect_;
    }
    bool search_field_visible() const
    {
        return is_open_;
    }

    // ── Callbacks ─────────────────────────────────────────────────────────
    // Fires when the overlay should be dismissed (Escape, outside click).
    std::function<void()> on_close;
    // Fires when the user activates a room (Enter / click).
    std::function<void(const std::string& room_id)> on_room_selected;
    // Fires from paint for a visible row whose avatar isn't cached yet.
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

    static constexpr float kCardW = 560.0f;
    static constexpr float kCardMaxH = 480.0f;
    static constexpr float kHeaderH = 52.0f;
    static constexpr float kRowH = 44.0f;

    // Horizontal "Recent" strip (shown only when the query is empty).
    static constexpr float kRecentStripH = 92.0f; // caption + chip row
    static constexpr float kRecentChipW = 64.0f;
    static constexpr float kRecentChipGap = 8.0f;
    static constexpr float kRecentAvatar = 40.0f;

private:
    class Adapter;
    friend class Adapter;

    // Rebuild filtered_ from all_rooms_ + query_, then reset the inner list's
    // selection to the first row and scroll to the top.
    void refilter_();

    // True when the Recent strip should be shown (query empty + recent_ non-empty).
    bool show_recent_() const;
    // Paint the horizontal "Recent" strip and (re)populate recent_chips_.
    void paint_recent_strip_(tk::PaintCtx& ctx);

    bool is_open_ = false;
    std::string query_;
    std::vector<tesseract::RoomInfo> all_rooms_;
    std::vector<tesseract::RoomInfo> filtered_;
    std::vector<tesseract::RoomInfo> recent_;

    RoomsProvider rooms_provider_;
    RoomsProvider recent_provider_;
    AvatarProvider avatar_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_ = nullptr;

    tk::Rect card_rect_{};
    tk::Rect search_field_rect_{};
    tk::Rect recent_strip_rect_{};
    // Per-chip hit rects (widget-local) + room id, rebuilt each paint.
    std::vector<std::pair<tk::Rect, std::string>> recent_chips_;
    // Index of the chip currently pressed (-1 = none).
    int pressed_chip_ = -1;
    // True while a pointer-down landed on the dim backdrop (outside the card);
    // a pointer-up that also lands outside dismisses the overlay.
    bool press_outside_ = false;
};

} // namespace tesseract::views
