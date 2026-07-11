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
// Embedded as a fixed-height child widget (not a whole SettingsPage) inside
// ImagePacksSection, so the section itself stays a plain non-scrolling VBox
// like every other settings tab; this widget carries its own internal
// scroll region, mirroring UserPackEditor's same-shaped fixed viewport.

#include "tk/layout.h"
#include "tk/widget.h"

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

class KnownPacksList : public tk::VBox
{
public:
    KnownPacksList();

    void set_packs(std::vector<KnownPackRow> packs);

    // Fired when the user toggles a row's checkbox.
    std::function<void(std::string room_id, std::string state_key, bool subscribed)>
        on_subscription_toggled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;

private:
    void rebuild_();

    std::vector<KnownPackRow> packs_;
    std::vector<tk::CheckButton*> rows_; // borrowed; owned via add_child
    tk::Label* empty_label_ = nullptr;

    float scroll_y_ = 0.0f;
    float content_height_ = 0.0f;

    static constexpr float kViewportH = 200.0f;
};

} // namespace tesseract::views
