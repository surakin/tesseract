#pragma once

// Ctrl+Shift+F global message search — a centred command-palette overlay,
// modelled on QuickSwitcher. Paints a dim backdrop over the whole surface with
// a centred card: a search field at the top (a host-overlaid NativeTextField,
// positioned from search_field_rect()) and a scrollable results list below.
//
// Unlike QuickSwitcher (which filters local room data), the result list is
// fed by the backend full-text index: the shell pipes the field's text changes
// in via set_query(), debounces, calls Client::search_messages(), and pushes
// the hits back through set_results(). Each row shows the room name, sender and
// a snippet of the matching message; Enter / click fires on_result_activated
// so the shell can open the room and scroll to the event. Up/Down navigate;
// Escape (routed by the shell) closes.
//
// Mounted as the topmost child of MainAppWidget — set_visible(false) by
// default, arranged at full bounds, painted last (highest z-order). The inner
// tk::ListView is a child so it handles row clicks + wheel scrolling via the
// normal dispatch path; this widget only adds the backdrop / click-outside
// behaviour and keyboard-driven selection.

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/list_view.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class MessageSearchView : public tk::Widget
{
public:
    MessageSearchView();
    ~MessageSearchView() override; // out-of-line — Adapter is opaque here

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open();
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // ── Data + query ──────────────────────────────────────────────────────
    // Host pipes its NativeTextField's text changes in here. Fires
    // on_query_changed (which the shell debounces into Client::search_messages).
    void set_query(const std::string& q);
    const std::string& query() const
    {
        return query_;
    }
    // Replace the result list. `for_query` lets the shell tag results with the
    // query they belong to so a stale response (a slower earlier search) can be
    // dropped — applied only when it matches the current query.
    void set_results(std::vector<tesseract::SearchHit> results,
                     const std::string& for_query);

    // ── Keyboard navigation (driven by the field's popup-nav callback) ────
    void move_selection(int delta);
    void activate_selected();

    // ── Native-field rect delegation (mirrors QuickSwitcher) ──────────────
    tk::Rect search_field_rect() const
    {
        return search_field_rect_;
    }
    bool search_field_visible() const
    {
        return is_open_;
    }

    // Weak reference to the shell-owned tk::NativeTextField; call once,
    // right after make_text_field(), so on_theme_changed() has something
    // to push colors onto.
    void set_native_field(std::weak_ptr<tk::NativeTextField> field)
    {
        native_field_ = std::move(field);
    }

    void on_theme_changed(const tk::Theme& t) override;

    // ── Callbacks ─────────────────────────────────────────────────────────
    // Fires when the overlay should be dismissed (Escape, outside click).
    std::function<void()> on_close;
    // Fires on every query change. The shell debounces and runs the search.
    std::function<void(const std::string& query)> on_query_changed;
    // Fires when the user activates a result (Enter / click): open the room and
    // scroll to the event.
    std::function<void(const std::string& room_id, const std::string& event_id)>
        on_result_activated;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

    static constexpr float kCardW = 620.0f;
    static constexpr float kCardMaxH = 520.0f;
    static constexpr float kHeaderH = 52.0f;
    static constexpr float kRowH = 58.0f; // two text lines per result

private:
    class Adapter;
    friend class Adapter;

    std::size_t result_count_() const
    {
        return results_.size();
    }

    bool is_open_ = false;
    std::string query_;
    std::vector<tesseract::SearchHit> results_;
    // True once at least one search has come back for the current query — drives
    // the empty-state message ("No matches" vs "Type to search").
    bool have_searched_ = false;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_ = nullptr;

    tk::Rect card_rect_{};
    tk::Rect search_field_rect_{};
    std::weak_ptr<tk::NativeTextField> native_field_; // see set_native_field()
    // True while a pointer-down landed on the dim backdrop (outside the card);
    // a pointer-up that also lands outside dismisses the overlay.
    bool press_outside_ = false;
    // Monotonically non-decreasing within an open session: once the card has
    // grown to accommodate results it never shrinks back when the user keeps
    // typing (which clears results and re-triggers arrange before the next
    // search response arrives).
    float max_card_h_ = 0.0f;
};

} // namespace tesseract::views
