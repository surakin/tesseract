#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/RoomWindowBase.h"
#include "tk/host_win32.h"
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/RoomMediaView.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"

#include <memory>
#include <string>
#include <vector>

namespace win32
{
class MainWindow;
}

namespace win32
{

// A secondary (pop-out) room window for the Win32 shell.
//
// Lifecycle:
//   MainWindow::create_secondary_room_window_() allocates this via `new`.
//   ShellBase::open_room_in_new_window() wraps it in a unique_ptr and stores
//   it in owned_secondary_windows_.  When the OS window closes (WM_DESTROY)
//   schedule_self_close_() posts a deferred call to release_owned_window_(),
//   which destroys the C++ object safely outside the message handler.
class RoomWindow : public tesseract::RoomWindowBase
{
public:
    RoomWindow(MainWindow* parent, const std::string& room_id);
    ~RoomWindow() override;

    void bring_to_front() override;
    void close_window() override;
    void request_relayout() override;
    void update_window_title_(const std::string& name) override;
    void apply_theme(const tk::Theme& t) override;
    void repaint_anim_frame() override;

protected:
    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    // compose_text_area_() uses RoomWindowBase's default (self-owned via
    // room_view_->compose_bar()->text_area()) — no override needed.
    tesseract::views::ForwardRoomPicker* forward_picker_() override
    {
        return forward_picker_widget_;
    }
    tesseract::views::RoomMediaView* room_media_view_() override
    {
        return room_media_view_widget_;
    }
    void focus_forward_picker_field_() override
    {
        if (!forward_picker_widget_)
            return;
        if (auto* f = forward_picker_widget_->search_field())
        {
            f->set_text("");
            f->set_focused(true);
        }
    }
    void hide_forward_picker_field_() override
    {
        if (forward_picker_widget_)
            if (auto* f = forward_picker_widget_->search_field())
                f->set_visible(false);
        if (hwnd_)
        {
            SetFocus(hwnd_);
        }
    }
    tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                      std::size_t size, bool compress) override
    {
        return surface_ ? surface_->host().encode_for_send(data, size, compress)
                        : tk::EncodedImage{};
    }
    bool
    put_image_on_clipboard_(std::span<const std::uint8_t> bytes) override
    {
        return surface_ && surface_->host().set_clipboard_image(bytes);
    }
    void set_clipboard_text_(std::string_view text) override
    {
        if (surface_)
            surface_->host().set_clipboard_text(text);
    }
    void show_toast_(std::string message) override
    {
        if (surface_)
            surface_->host().show_toast(std::move(message));
    }

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);
    void show_mention_popup_(tk::Rect cursor_rect, int rows);
    void hide_mention_popup_();
    void show_slash_popup_(tk::Rect cursor_rect, int rows);
    void show_shortcode_popup_(tk::Rect cursor_rect, int rows);
    void show_gif_popup_();
    void hide_gif_popup_();

    bool mention_popup_visible_() const
    {
        return mention_popup_ && mention_popup_->visible();
    }

    MainWindow* parent_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;
    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* text_area_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr; // borrowed
    tesseract::views::ConfirmDialog* confirm_dialog_widget_ = nullptr; // borrowed
    std::unique_ptr<tk::PopupSurfaceHandle> mention_popup_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> slash_popup_;
    tesseract::views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> shortcode_popup_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> gif_popup_;
    tesseract::views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;

    static constexpr const wchar_t* kClassName = L"TesseractRoomWnd";
    static bool class_registered_;
};

} // namespace win32
