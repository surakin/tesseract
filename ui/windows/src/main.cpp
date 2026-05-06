#include "MainWindow.h"
#include <stdexcept>

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE /*hPrevInstance*/,
    LPWSTR    /*lpCmdLine*/,
    int       nCmdShow)
{
    // Enable common controls (status bar, etc.)
    INITCOMMONCONTROLSEX icce{ sizeof(icce), ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icce);

    if (!win32::MainWindow::register_class(hInstance))
        return 1;

    win32::MainWindow window(hInstance);
    if (!window.create(nCmdShow))
        return 1;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
