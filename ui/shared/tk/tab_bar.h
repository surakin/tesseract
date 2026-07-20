#pragma once

// Horizontal room-tab bar. Shows one tab per open room (avatar + display name
// + close button). Self-hides when only one tab is present. Supports
// horizontal scroll when tabs overflow the available width.
//
// Consumers:
//   tab_bar->add_tab(room_id, display_name, avatar);
//   tab_bar->set_active(room_id);
//   tab_bar->on_tab_selected = [](const std::string& id) { … };
//   tab_bar->on_tab_closed   = [](const std::string& id) { … };

#include "widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

class TabBar : public Widget
{
public:
    // Height of the bar in logical pixels.
    static constexpr float kHeight = 40.0f;

protected:
    TabBar();
    TK_WIDGET_FACTORY_FRIEND(TabBar)

public:
    ~TabBar() override = default;

    // Add a tab. If this makes count > 1 the bar becomes visible.
    // The new tab is NOT automatically made active.
    // avatar may be nullptr — falls back to initials disc.
    void add_tab(std::string room_id, std::string display_name,
                 const Image* avatar);

    // Remove the tab for room_id. If count drops to 1 the bar hides itself.
    // No-op if room_id is absent.
    void remove_tab(const std::string& room_id);

    // Highlight the tab for room_id. No-op if absent.
    void set_active(const std::string& room_id);

    // Update display_name and avatar for an existing tab. No-op if absent.
    void update_tab(const std::string& room_id, std::string display_name,
                    const Image* avatar);

    // Remove all tabs and reset scroll. Use before a full rebuild.
    void clear();

    // How many tabs are currently held.
    int item_count() const
    {
        return static_cast<int>(items_.size());
    }

    // room_id of the tab at position i (no bounds check — caller guards).
    const std::string& room_id_at(int i) const
    {
        return items_[i].room_id;
    }

    // Fires when the user clicks a tab body (not the × button).
    std::function<void(const std::string& room_id)> on_tab_selected;

    // Fires when the user clicks the × button on a tab.
    // Only armed when item_count() > 1 — the last tab cannot be closed.
    std::function<void(const std::string& room_id)> on_tab_closed;

    // tk::Widget overrides.
    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(Point local, float dx, float dy, bool is_touchpad = false) override;

    // Keyboard-focusable whenever there's more than one tab to cycle
    // between (a single tab has nothing to Tab-stop for). Left/Right moves
    // the active tab, mirroring the mouse-click path exactly: only fires
    // on_tab_selected, it does not call set_active() itself — the app's own
    // on_tab_selected handler is responsible for calling set_active() back
    // in, same as a real click. Guards against ctrl/alt/meta so it doesn't
    // shadow the global Alt+Left/Right history-navigation shortcut.
    bool focusable() const override
    {
        return item_count() > 1;
    }
    // Gated on has_focus(): without it, this is reachable not only as the
    // genuinely tk-focused widget but also via Host::dispatch_key_down's
    // root-wide broadcast fallback (fired whenever the ACTUALLY focused
    // widget doesn't consume a key) — an unfocused tab bar would silently
    // switch its active tab on a stray Left/Right meant elsewhere.
    bool on_key_down(const KeyEvent& e) override
    {
        if (!has_focus()) return false;
        if (e.ctrl || e.alt || e.meta) return false;
        if (e.key != Key::Left && e.key != Key::Right) return false;
        const int n = item_count();
        if (n == 0) return false;
        const int delta = (e.key == Key::Right) ? 1 : -1;
        const int next = (active_idx_ < 0) ? 0 : (active_idx_ + delta + n) % n;
        if (on_tab_selected) on_tab_selected(items_[next].room_id);
        return true;
    }

private:
    // ── Visual constants (all logical pixels) ────────────────────────────
    static constexpr float kAvatarSz = 20.0f;
    static constexpr float kCloseSz = 16.0f;
    static constexpr float kPadOuter = 4.0f;
    static constexpr float kPadInner = 8.0f;
    static constexpr float kTabMin = 120.0f;
    static constexpr float kTabMax = 240.0f;
    static constexpr float kRadius = 4.0f;

    // Fixed per-tab chrome: left-pad + avatar + inner-gap + inner-gap + × + right-pad.
    static constexpr float kChrome =
        kPadOuter + kAvatarSz + kPadInner + kPadInner + kCloseSz + kPadOuter;

    struct TabItem
    {
        std::string room_id;
        std::string display_name;
        const Image* avatar = nullptr; // non-owning
        float x = 0.f;                 // left edge in scroll-space
        float width = 0.f;             // set by arrange()
        bool hovered = false;
        bool close_hovered = false;
        // Cached name TextLayout; rebuilt when display_name or width changes.
        std::unique_ptr<TextLayout> layout;
        float layout_max_w = -1.f;
        std::string layout_name;
    };

    // Rect of the × button for tab i, in scroll-space x-coordinates.
    Rect close_scroll_rect_(int i) const;

    // True when widget-local `local` lands on the × close button of tab `idx`.
    bool close_button_hit_(int idx, Point local) const;

    // Index of the tab that contains scroll_x_local (= pointer.x + scroll_x_),
    // or -1 if none.
    int tab_at_(float scroll_x_local) const;

    void clamp_scroll_();

    // Build t.layout for the given max_w if stale. Called from arrange() and
    // paint() — both contexts expose factory under ctx.factory.
    template <typename Ctx>
    void ensure_layout_(Ctx& ctx, TabItem& t, float max_w);

    std::vector<TabItem> items_;
    int active_idx_ = -1;
    float scroll_x_ = 0.f;
    float total_width_ = 0.f; // sum of all tab widths; set by arrange()
    int pressed_idx_ = -1;
    bool pressed_close_ = false;

    // Cached "×" layout (built once in arrange, reused in paint).
    std::unique_ptr<TextLayout> close_layout_;
};

} // namespace tk
