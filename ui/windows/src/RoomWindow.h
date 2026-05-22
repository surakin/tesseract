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
#include "views/MentionController.h"
#include "views/MentionPopup.h"

#include <memory>
#include <string>

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

protected:
    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return text_area_.get();
    }

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);
    void show_mention_popup_(tk::Rect cursor_rect, int rows);
    void hide_mention_popup_();
    bool mention_popup_visible_() const
    {
        return mention_popup_hwnd_ && IsWindowVisible(mention_popup_hwnd_);
    }

    MainWindow* parent_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;
    std::unique_ptr<tk::NativeTextArea> text_area_;
    HWND mention_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    static constexpr const wchar_t* kClassName = L"TesseractRoomWnd";
    static bool class_registered_;
};

} // namespace win32
