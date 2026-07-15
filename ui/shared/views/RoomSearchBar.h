#pragma once

// Per-room "find in conversation" search bar — a compact docked strip mounted
// directly under RoomHeader when in-room search is active. Shows a search field
// (host-overlaid NativeTextField), match count, UP/DOWN navigation buttons, a
// Paginate checkbox, and a close button. Results are highlighted in the timeline
// by MessageListView; this widget only displays the count and navigation.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/svg.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class RoomSearchBar : public tk::Widget
{
public:
    static constexpr float kStripH = 44.0f;

    // `host` is nullable: when null (e.g. unit tests constructing the bar
    // directly), the search field is skipped — search_field() stays null.
    explicit RoomSearchBar(tk::Host* host = nullptr);
    ~RoomSearchBar() override = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open();
    void close();
    bool is_open() const { return is_open_; }

    // ── Data ──────────────────────────────────────────────────────────────
    // Host pipes NativeTextField changes in here; fires on_query_changed.
    void set_query(const std::string& q);
    const std::string& query() const { return query_; }

    // Shell pushes status back after each search completes.
    // current: 1-based index of focused match (0 = no matches / searching).
    // total:   total number of matches found (0 = none).
    // searching: true while a search is in flight.
    // at_start: true when pagination found the beginning of room history.
    void set_match_status(int current, int total, bool searching, bool at_start);

    // Whether the Paginate checkbox is currently checked.
    bool paginate_enabled() const;

    // ── Native-field rect delegation (driven by the host every frame) ─────
    // Returns empty rect when the bar is closed.
    tk::Rect search_field_rect() const { return field_rect_; }
    bool search_field_visible() const { return is_open_; }

    // The self-owned search field, or null when constructed without a
    // Host. Unlike the other search overlays, this field has no popup-nav
    // (Up/Down are the strip's own chevron buttons) and no submit action.
    tk::TextField* search_field() const
    {
        return search_field_;
    }

    void on_theme_changed(const tk::Theme& t) override;

    // ── Callbacks ─────────────────────────────────────────────────────────
    std::function<void()> on_close;
    std::function<void(const std::string& query)> on_query_changed;
    // delta: -1 = navigate to older match (UP), +1 = navigate to newer (DOWN).
    std::function<void(int delta)> on_navigate;
    std::function<void(bool enabled)> on_paginate_toggled;

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    // ── Test-only accessors ───────────────────────────────────────────────
    tk::Rect up_btn_rect_for_test() const { return up_btn_ ? up_btn_->bounds() : tk::Rect{}; }
    tk::Rect down_btn_rect_for_test() const { return down_btn_ ? down_btn_->bounds() : tk::Rect{}; }
    tk::Rect close_btn_rect_for_test() const { return close_btn_ ? close_btn_->bounds() : tk::Rect{}; }

private:
    bool is_open_ = false;
    std::string query_;
    std::string count_text_ = "Type to search";

    // Child widgets (owned by widget tree).
    tk::Label*       count_label_ = nullptr;
    tk::Button*      up_btn_      = nullptr;
    tk::Button*      down_btn_    = nullptr;
    tk::CheckButton* paginate_cb_ = nullptr;
    tk::Button*      close_btn_   = nullptr;

    // Icon caches (tint-aware SVG rendering, recolors on theme switch).
    tk::IconCache up_icon_;
    tk::IconCache down_icon_;
    tk::IconCache close_icon_;

    // Widest count_label_ width seen so far; never shrinks so the text field
    // doesn't jitter as the match count grows during pagination.
    float count_label_max_w_ = 0.0f;

    // World-space rect reported to the host for the native text field overlay.
    tk::Rect field_rect_{};
    tk::TextField* search_field_ = nullptr; // owned via add_child when host provided
};

} // namespace tesseract::views
