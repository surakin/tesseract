#pragma once

// Vertical list of `UserInfo` rows, one per signed-in account. Mounted by
// the per-platform left-click popover (Qt6 QFrame, GTK4 GtkPopover, macOS
// NSPanel, Win32 child WS_POPUP). The active account row carries the
// indicator dot; selecting any other row fires `on_select(user_id)` and the
// host calls `MainWindow::switch_active_account` then dismisses the popover.
// At most `kMaxVisibleRows` rows are shown at once; beyond that the rows
// scroll inside a tk::ScrollView. Every shell sizes its popup from
// measure(), so the cap applies on all four platforms.

#include "UserInfo.h"
#include "tk/canvas.h"
#include "tk/widget.h"

#include <cstddef>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
class ScrollView;
}

namespace tesseract::views
{

struct AccountEntry
{
    std::string user_id; // canonical "@alice:example.org"
    std::string display_name;
    std::string avatar_url; // mxc://… (empty → initials fallback)
    bool active = false; // true on the row representing the foreground account
    bool has_unread = false; // true if this account has an unread notification
};

class AccountPicker : public tk::Widget
{
public:
    static constexpr std::size_t kMaxVisibleRows = 8;

    AccountPicker();
    ~AccountPicker() override = default;

    /// Replace the current row set. Updates the child `UserInfo` rows in
    /// place when the count is unchanged, rebuilds them otherwise; safe to
    /// call on every popover open.
    void set_entries(std::vector<AccountEntry> entries);

    /// Resolves an mxc:// to a decoded image. Threaded down to every child
    /// `UserInfo`.
    using ImageProvider = UserInfo::ImageProvider;
    void set_image_provider(ImageProvider p);

    /// Fired when the user selects a row. The host typically dismisses the
    /// popover and calls `switch_active_account`.
    std::function<void(const std::string& user_id)> on_select;

    /// Forwarded from every row's UserInfo::on_avatar_needed (see there) so
    /// the host can request a fetch on a cache miss, e.g. right after a
    /// display scale change flushed the avatar cache.
    std::function<void(const std::string& /*mxc*/)> on_avatar_needed;

    const std::vector<AccountEntry>& entries() const
    {
        return entries_;
    }

    const std::vector<UserInfo*>& rows() const
    {
        return rows_;
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;

private:
    void rebuild_rows();
    void bind_row_(UserInfo& row, const AccountEntry& e);

    std::vector<AccountEntry> entries_;
    ImageProvider image_provider_;
    tk::ScrollView* scroll_ = nullptr; // owned via children()
    tk::Widget* column_ = nullptr; // owned by scroll_; parent of every row
    std::vector<UserInfo*> rows_; // borrowed; ownership in column_
};

} // namespace tesseract::views
