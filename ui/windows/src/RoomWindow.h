#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/ComposerPopups.h"
#include "app/RoomWindowBase.h"
#include "CustomTitleBar.h"
#include "tk/host_win32.h"
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/RoomMediaView.h"

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
    void apply_scale_change(float scale) override;
    void repaint_anim_frame() override;

protected:
    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id, const std::string& message) override;

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);
    // Media-viewer OS full-screen for this pop-out: caption strip suppressed,
    // window borderless over the monitor.
    void set_window_fullscreen_impl_(bool on);

    MainWindow* parent_;
    HWND hwnd_ = nullptr;
    CustomTitleBar title_bar_;
    // Tracks WM_ACTIVATE so the custom title bar can dim its text/icon to
    // match Win11's inactive-caption convention.
    bool is_active_ = true;
    bool fs_active_ = false;
    WINDOWPLACEMENT fs_saved_placement_{};
    std::unique_ptr<tk::win32::Surface> surface_;
    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* text_area_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr;    // borrowed

    // Composer popups (@mention, /command, :shortcode:, /gif). Declared after
    // surface_ so the popups are destroyed before the Host they came from.
    std::unique_ptr<tesseract::ComposerPopups> popups_;

    static constexpr const wchar_t* kClassName = L"TesseractRoomWnd";
    static bool class_registered_;
};

} // namespace win32
