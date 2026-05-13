#pragma once

// Vertical list of `UserInfo` rows, one per signed-in account. Mounted by
// the per-platform left-click popover (Qt6 QFrame, GTK4 GtkPopover, macOS
// NSPanel, Win32 child WS_POPUP). The active account row carries the
// indicator dot; selecting any other row fires `on_select(user_id)` and the
// host calls `MainWindow::switch_active_account` then dismisses the popover.

#include "UserInfo.h"
#include "tk/canvas.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views {

struct AccountEntry {
    std::string user_id;        // canonical "@alice:example.org"
    std::string display_name;
    std::string avatar_url;     // mxc://… (empty → initials fallback)
    bool        active = false; // true on the row representing the foreground account
};

class AccountPicker : public tk::Widget {
public:
    AccountPicker();
    ~AccountPicker() override = default;

    /// Replace the current row set. Rebuilds child `UserInfo` widgets in
    /// place; safe to call on every popover open.
    void set_entries(std::vector<AccountEntry> entries);

    /// Resolves an mxc:// to a decoded image. Threaded down to every child
    /// `UserInfo`.
    using ImageProvider = UserInfo::ImageProvider;
    void set_image_provider(ImageProvider p);

    /// Fired when the user selects a row. The host typically dismisses the
    /// popover and calls `switch_active_account`.
    std::function<void(const std::string& user_id)> on_select;

    const std::vector<AccountEntry>& entries() const { return entries_; }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    void rebuild_rows();

    std::vector<AccountEntry> entries_;
    ImageProvider             image_provider_;
    std::vector<UserInfo*>    rows_;   // borrowed; ownership in `children()`
};

} // namespace tesseract::views
