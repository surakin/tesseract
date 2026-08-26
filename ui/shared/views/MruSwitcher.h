#pragma once

// Ctrl+Tab / Ctrl+Shift+Tab MRU room switcher — an Alt-Tab-style hold-and-
// cycle overlay, distinct from QuickSwitcher's Ctrl+K search palette. The
// shell drives it from native, held-modifier-aware key handling (see
// MainAppWidget::begin_mru_cycle/advance_mru_cycle/commit_mru_cycle/
// cancel_mru_cycle): begin_cycle() snapshots the current MRU room list and
// preselects the *previous* room (index 1 — matching standard Alt-Tab
// muscle memory, where the first Tab press already moves off the current
// window); advance() steps the selection while Ctrl stays held; commit()
// (fired on Ctrl-up) or cancel() (fired on Escape or losing focus) closes
// the overlay.
//
// The snapshot is deliberately frozen for the duration of a cycle — taken
// once in begin_cycle(), never re-queried by advance() — so a room reorder
// from an incoming message can't shuffle chips out from under the user's
// thumb mid-hold.
//
// Renders as a centred card containing a single row of chips, painted via
// the same paint_room_chips() helper QuickSwitcher's "Recent" strip uses
// (see room_chip_strip.h), so both overlays share one chip implementation.
// Mounted as a top-level overlay child of MainAppWidget, alongside
// QuickSwitcher.

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class MruSwitcher : public tk::Widget
{
protected:
    MruSwitcher();
    TK_WIDGET_FACTORY_FRIEND(MruSwitcher)

public:
    using RoomsProvider = std::function<std::vector<tesseract::RoomInfo>()>;
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;

    // ── Data ──────────────────────────────────────────────────────────────
    void set_recent_provider(RoomsProvider p);
    void set_avatar_provider(AvatarProvider p);

    // ── Lifecycle ─────────────────────────────────────────────────────────
    // Snapshots recent_provider_() and preselects index 1 (the previous
    // room), regardless of whether the cycle was initiated by Tab or
    // Shift+Tab — matches standard Alt-Tab behavior, where the first press
    // always lands on "the other" most-recent window. Leaves the overlay
    // closed if the snapshot has fewer than 2 rooms (nothing to switch to).
    void begin_cycle();
    // Steps the selection by `delta` within the frozen snapshot, wrapping at
    // both ends. No-op if not open.
    void advance(int delta);
    // Fires on_room_selected() for the current selection, then closes.
    // No-op if not open.
    void commit();
    // Closes without firing on_room_selected().
    void cancel();

    bool is_open() const
    {
        return is_open_;
    }

    // ── Callbacks ─────────────────────────────────────────────────────────
    std::function<void(const std::string& room_id)> on_room_selected;
    std::function<void(const tesseract::RoomInfo&)> on_room_avatar_needed;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;

    // Ceiling for the card's content-driven width (see arrange()) — sized to
    // comfortably fit ShellBase::kRecentRoomsMax (8) chips at once, since the
    // strip never scrolls.
    static constexpr float kCardW = 620.0f;
    static constexpr float kStripH = 84.0f; // chip row, no caption
    static constexpr float kChipW = 64.0f;
    static constexpr float kChipGap = 8.0f;
    static constexpr float kAvatar = 40.0f;

private:
    void close_();

    bool is_open_ = false;
    int selected_ = -1;
    std::vector<tesseract::RoomInfo> rooms_;

    RoomsProvider recent_provider_;
    AvatarProvider avatar_provider_;

    tk::Rect card_rect_{};
    // Per-chip hit rects (world coords) + room id, rebuilt each paint.
    std::vector<std::pair<tk::Rect, std::string>> chips_;
    // True while a pointer-down landed outside the card; a pointer-up that
    // also lands outside cancels the cycle (matches QuickSwitcher's
    // press_outside_ idiom).
    bool press_outside_ = false;
    // Index of the chip currently pressed (-1 = none). A release on the
    // same chip selects + commits it immediately.
    int pressed_chip_ = -1;
};

} // namespace tesseract::views
