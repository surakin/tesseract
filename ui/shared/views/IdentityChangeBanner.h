#pragma once

// Strip shown above an encrypted room's message list while a member's
// cryptographic identity has changed (MSC4153 identity pinning). Shows the
// first warning of the room's current set; its one button resolves it —
// "OK" accepts the new identity, "Withdraw verification" handles a user who
// was verified before the change (which blocks sending in
// exclude-insecure-devices mode until resolved). The next warning, if any,
// appears once the SDK reports the resolved one gone.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class IdentityChangeBanner : public tk::Widget
{
public:
    IdentityChangeBanner();
    ~IdentityChangeBanner() override = default;

    // Replace the room's warning set. Hides the banner when empty.
    void set_warnings(std::vector<tesseract::IdentityWarning> warnings);
    const std::vector<tesseract::IdentityWarning>& warnings() const
    {
        return warnings_;
    }
    bool has_warning() const
    {
        return !warnings_.empty();
    }
    std::string label_text() const;

    // Fired with the shown warning when its button is clicked.
    std::function<void(const tesseract::IdentityWarning&)> on_resolve;

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

    std::vector<tesseract::IdentityWarning> warnings_;

    tk::Label* label_ = nullptr;   // borrowed
    tk::Button* action_ = nullptr; // borrowed
};

} // namespace tesseract::views
