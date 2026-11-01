#pragma once

// Strip shown above the message list of a room that has been upgraded
// (tombstoned). It replaces the composer's role: the old room is read-only
// history, and the one button follows the upgrade to the replacement room —
// joining it first when the user hasn't yet.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class RoomReplacedBanner : public tk::Widget
{
public:
    RoomReplacedBanner();
    ~RoomReplacedBanner() override = default;

    // `successor_joined` picks the button label: "Go to the new room" when
    // the user is already in it, "Join the new room" otherwise.
    void set_successor_joined(bool joined);
    bool successor_joined() const
    {
        return successor_joined_;
    }
    std::string label_text() const;
    std::string action_text() const;

    std::function<void()> on_open; // follow the upgrade to the new room

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

    tk::Role access_role() const override
    {
        return tk::Role::Group;
    }
    std::string access_name() const override
    {
        return label_text();
    }

    void paint_before_children(tk::PaintCtx&) override;

    static constexpr float kHeight = 48.0f;

private:
    void apply_();

    bool successor_joined_ = false;

    tk::Label* label_ = nullptr;   // borrowed
    tk::Button* action_ = nullptr; // borrowed
};

} // namespace tesseract::views
