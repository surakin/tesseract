#pragma once

// A settings-group row for options that only apply after a restart: a small
// note ("Changes take effect after restart.") plus a "Restart now" button row
// that stays hidden until set_pending(true). Shared by LanguageGroup and
// ProxyGroup.

#include "tk/controls.h"
#include "tk/layout.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class RestartNotice : public tk::VBox
{
public:
    RestartNotice();
    ~RestartNotice() override = default;

    // Show / hide the "Restart now" row — set while the saved value differs
    // from the one this process started with.
    void set_pending(bool pending);

    // Replace the note text (default: "Changes take effect after restart.").
    void set_text(std::string text);

    bool row_visible() const
    {
        return restart_row_ && restart_row_->own_visible();
    }
    std::string text() const
    {
        return note_ ? note_->text() : std::string();
    }

    // Fires when the user presses "Restart now".
    std::function<void()> on_restart_requested;

private:
    tk::Label* note_ = nullptr;
    tk::Widget* restart_row_ = nullptr;
};

} // namespace tesseract::views
