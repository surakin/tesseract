#pragma once

// Slim reminder strip shown above the message list while this device's
// encryption isn't sorted out (recovery never set up, or this device can't
// read encrypted history yet) and the user has closed the encryption dialog.
// It has no flow of its own: its one button reopens EncryptionSetupOverlay,
// which is the single place every encryption interaction happens.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class EncryptionReminderBanner : public tk::Widget
{
public:
    enum class Kind
    {
        SetupNeeded, // no recovery set up for the account yet
        Locked,      // this device can't read encrypted messages yet
    };

    EncryptionReminderBanner();
    ~EncryptionReminderBanner() override = default;

    void set_kind(Kind k);
    Kind kind() const { return kind_; }
    std::string label_text() const;

    std::function<void()> on_open;    // the action button: reopen the dialog
    std::function<void()> on_dismiss; // ✕: snooze the reminder

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

    tk::Role access_role() const override { return tk::Role::Group; }
    std::string access_name() const override { return label_text(); }

    void paint_before_children(tk::PaintCtx&) override;

    static constexpr float kHeight = 48.0f;

private:
    void apply_kind_();

    Kind kind_ = Kind::Locked;

    tk::Label*  label_   = nullptr; // borrowed
    tk::Button* action_  = nullptr; // borrowed
    tk::Button* dismiss_ = nullptr; // borrowed
};

} // namespace tesseract::views
