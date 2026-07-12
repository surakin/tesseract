#pragma once
#include <QDialog>
#include <QPointer>
#include <functional>
#include <memory>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/JoinRoomView.h"

namespace tesseract
{
class Client;
}

/// Centred dialog for looking up and joining a room (MSC3266).
/// Hosts the shared tesseract::views::JoinRoomView inside a tk::qt6::Surface
/// with a native QLineEdit overlaid on the alias-field rect.
class JoinRoomDialog : public QDialog
{
    Q_OBJECT
public:
    explicit JoinRoomDialog(QWidget* parent = nullptr);

    void setClient(tesseract::Client* c);
    void setAvatarProvider(
        std::function<const tk::Image*(const std::string& mxc_url)> fn);

    /// Open (or bring to front) the dialog, resetting to Idle state.
    void openDialog();

    /// Open the dialog pre-filled with `prefill` (room ID or alias).
    void openDialogWithPrefill(const std::string& prefill);

    /// Re-skin the dialog surface when the theme preference changes.
    void set_theme(const tk::Theme& t);

    /// Fired when join succeeds; shell should navigate to this room ID.
    std::function<void(const std::string& room_id)> onJoined;
    /// Fired when the user clicks a link in the room topic; shell should
    /// route http(s) URLs to the browser and matrix: URIs in-app.
    std::function<void(const std::string& url)> onOpenUrl;

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlay();

    tesseract::Client* client_ = nullptr;
    tk::qt6::Surface* surface_ = nullptr;
    tesseract::views::JoinRoomView* shared_ = nullptr; // borrowed
    // shared_ptr (not unique_ptr) so JoinRoomView can hold a weak_ptr for
    // theming — see tk::Widget::on_theme_changed / apply_theme.
    std::shared_ptr<tk::NativeTextField> alias_field_;
    uint32_t gen_ = 0; // guards stale async callbacks
};
