#pragma once

// KnownPacksList — scrollable checkbox list of every room-sourced image
// pack the aggregator knows about (Client::list_image_packs() filtered to
// PackSourceKind::Room), letting the user explicitly subscribe/unsubscribe
// each one via the account-wide m.image_pack.rooms event (see
// ImagePacksSection.h). Checkbox state reflects ImagePack::is_subscribed
// (explicit subscription), not merely "visible because joined". Toggling
// applies immediately — no Save button, consistent with every other
// checkbox in the global SettingsView.
//
// Embedded (not as a whole SettingsPage) inside ImagePacksSection, marked
// fill_main so it stretches to absorb the page's leftover vertical space
// (see ImagePacksSection.cpp) rather than sizing to its own content; this
// widget carries its own internal scroll region for when the pack list is
// taller than whatever height it's given. kViewportH below is only a
// fallback for the (currently unused) case of measure() being consulted
// without a fill_main parent honoring it.
//
// A tk::ScrollableBase (scroll offset, kinetic momentum, scrollbar thumb
// paint/drag) that owns a tk::VBox child (content_) for the actual row
// layout, rather than inheriting VBox directly — mirrors SettingsPage's
// same restructure; see that class's header comment for the full rationale.

#include "tk/layout.h"
#include "tk/scrollable_base.h"

#include <functional>
#include <string>
#include <vector>

namespace tk
{
class CheckButton;
class Label;
} // namespace tk

namespace tesseract::views
{

struct KnownPackRow
{
    std::string pack_id;
    std::string display_name;
    std::string room_id;
    std::string state_key;
    bool subscribed = false;
};

class KnownPacksList : public tk::ScrollableBase
{
public:
    KnownPacksList();

    void set_packs(std::vector<KnownPackRow> packs);

    // Fired when the user toggles a row's checkbox.
    std::function<void(std::string room_id, std::string state_key, bool subscribed)>
        on_subscription_toggled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_before_children(tk::PaintCtx&) override;
    void     paint_after_children(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

    // content_'s CheckButton rows are real children spanning the full
    // viewport width, including the scrollbar thumb's screen column — see
    // SettingsPage::dispatch_pointer_down's identical doc comment for why
    // this override (checking the thumb first) is needed.
    tk::Widget* dispatch_pointer_down(tk::Point world) override;
    void        on_pointer_drag(tk::Point local) override;
    void        on_pointer_up(tk::Point local, bool inside_self) override;

    // Test-only inspection of the scroll math.
    float scroll_y_for_testing() const { return scroll_y_; }

protected:
    float content_height() const override { return content_height_; }

private:
    void rebuild_();

    // Owns every row/empty-label; handles the actual flex layout.
    tk::VBox* content_ = nullptr;

    std::vector<KnownPackRow> packs_;
    std::vector<tk::CheckButton*> rows_; // borrowed; owned via content_->add_child
    tk::Label* empty_label_ = nullptr;

    // Natural (unclamped-viewport) height of content_, recomputed each
    // arrange()/paint_before_children() re-layout.
    float content_height_ = 0.0f;

    static constexpr float kViewportH = 200.0f;
};

} // namespace tesseract::views
