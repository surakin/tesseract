#include "MainWindow.h"
#include <ole2.h>
#include <stdexcept>

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE /*hPrevInstance*/,
    LPWSTR    /*lpCmdLine*/,
    int       nCmdShow)
{
    // OLE init on the UI thread — required for OLE drag-and-drop (the
    // Surface registers an IDropTarget per HWND). OleInitialize is a
    // superset of CoInitializeEx(COINIT_APARTMENTTHREADED) and matches
    // OleUninitialize 1:1 below.
    if (FAILED(OleInitialize(nullptr))) return 1;

    // Opt into per-monitor v2 DPI awareness so DWM hands us crisp pixels on
    // mixed-DPI setups. Dynamic-loaded: the call only exists on Win10 1703+,
    // and falls back silently to the manifested awareness (or none).
    using SetCtxFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setCtx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    // Enable common controls (status bar, etc.)
    INITCOMMONCONTROLSEX icce{ sizeof(icce), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icce);

    int exit_code = 1;
    if (win32::MainWindow::register_class(hInstance)) {
        win32::MainWindow window(hInstance);
        if (window.create(nCmdShow)) {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exit_code = static_cast<int>(msg.wParam);
        }
    }

    OleUninitialize();
    return exit_code;
}
