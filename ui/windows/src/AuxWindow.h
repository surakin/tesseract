#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/AuxWindowBase.h"
#include "tk/host_win32.h"

#include <memory>
#include <string>

namespace win32 { class MainWindow; }

namespace win32
{

// Generic secondary top-level window for the Win32 shell: hosts a
// tk::win32::Surface whose root is whatever widget the shared layer hands in.
// WM_CLOSE -> DestroyWindow; WM_DESTROY fires on_window_closed.
class AuxWindow : public tesseract::AuxWindowBase
{
public:
    AuxWindow(MainWindow* parent_shell, const std::string& title,
              std::unique_ptr<tk::Widget> root, int width, int height,
              const tk::Theme& theme);
    ~AuxWindow() override;

    void bring_to_front()                override;
    void close_window()                  override;
    void apply_theme(const tk::Theme&)   override;
    void apply_scale_change(float scale) override;
    void request_relayout()              override;
    void request_repaint()               override;

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);

    HWND                                hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;

    static constexpr const wchar_t* kClassName = L"TesseractAuxWnd";
    static bool class_registered_;
};

} // namespace win32
