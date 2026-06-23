#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/CallWindowBase.h"
#include "tk/host_win32.h"

#include <memory>
#include <string>

namespace win32 { class MainWindow; }

namespace win32
{

// Secondary call pop-out window for the Win32 shell.
// Hosts a tk::win32::Surface whose root is a CallOverlayWidget in Popout mode.
//
// Lifecycle mirrors RoomWindow: WM_CLOSE → DestroyWindow; WM_DESTROY fires
// on_window_closed so ShellBase can switch back to Docked mode.
class CallWindow : public tesseract::CallWindowBase
{
public:
    explicit CallWindow(MainWindow* parent_shell);
    ~CallWindow() override;

    void bring_to_front()               override;
    void close_window()                 override;
    void apply_theme(const tk::Theme&)  override;
    void request_relayout()             override;
    void request_repaint()              override;

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);

    HWND                              hwnd_    = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;

    static constexpr const wchar_t* kClassName = L"TesseractCallWnd";
    static bool class_registered_;
};

} // namespace win32
#endif // TESSERACT_CALLS_ENABLED
