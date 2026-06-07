#include "MainWindow.h"
#include "resource.h"
#include <ole2.h>
// <shobjidl.h> (not the SDK-only <ShObjIdl_core.h> split) so the include
// resolves on mingw-w64's case-sensitive, single-header layout too; it still
// declares SetCurrentProcessExplicitAppUserModelID on the Windows SDK.
#include <shobjidl.h>
// C++/WinRT is Windows-SDK-only; skip it on the mingw cross-build (COM is
// initialised with CoInitializeEx below instead of winrt::init_apartment).
#if !defined(__MINGW32__)
#include "winrt_coroutine_shim.h" // must precede any <winrt/...> include
#include <winrt/base.h>
#endif
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <stdexcept>
#include "tk/i18n.h"
#include <tesseract/client.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace
{

// Materialise the embedded app-icon PNG (IDR_TOAST_ICON) to a stable file and
// return its path. The toast AppUserModelId IconUri must point at an image
// file — an .exe path does not render — and the install ships only the exe, so
// the icon is extracted on disk at startup. Returns empty on failure (the
// caller then falls back to the exe path).
std::wstring write_toast_icon_png(HINSTANCE hInstance)
{
    HRSRC res =
        FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_TOAST_ICON), RT_RCDATA);
    if (!res)
    {
        return {};
    }
    HGLOBAL handle = LoadResource(hInstance, res);
    DWORD size = SizeofResource(hInstance, res);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0)
    {
        return {};
    }
    std::error_code ec;
    std::filesystem::path dir = tesseract::data_dir();
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return {};
    }
    std::filesystem::path file = dir / L"toast-icon.png";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return {};
    }
    out.write(static_cast<const char*>(data),
              static_cast<std::streamsize>(size));
    if (!out)
    {
        return {};
    }
    return file.wstring();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow)
{
    // Parse command-line arguments to detect a matrix: URI.
    std::string startup_uri;
    {
        int nArgs = 0;
        LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (szArgList && nArgs >= 2)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, szArgList[1], -1,
                                          nullptr, 0, nullptr, nullptr);
            if (len > 1)
            {
                std::string arg(static_cast<std::size_t>(len - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, szArgList[1], -1,
                                    arg.data(), len, nullptr, nullptr);
                if (tesseract::Client::parse_matrix_link(arg).kind
                    != tesseract::Client::MatrixLink::Kind::Unknown)
                {
                    startup_uri = std::move(arg);
                }
            }
        }
        if (szArgList)
            LocalFree(szArgList);
    }

    // Single-instance guard: if another process already holds this mutex,
    // find its main window, bring it to the foreground, and exit.
    HANDLE single_inst_mutex =
        CreateMutexW(nullptr, TRUE, L"io.gnomos.Tesseract.SingleInstanceMutex");
    if (!single_inst_mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW(L"TesseractMainWnd", nullptr))
        {
            if (IsIconic(existing))
            {
                ShowWindow(existing, SW_RESTORE);
            }
            SetForegroundWindow(existing);
            if (!startup_uri.empty())
            {
                COPYDATASTRUCT cds{};
                cds.dwData = 1; // matrix URI
                cds.cbData = static_cast<DWORD>(startup_uri.size() + 1);
                cds.lpData = startup_uri.data();
                SendMessageW(existing, WM_COPYDATA,
                             reinterpret_cast<WPARAM>(nullptr),
                             reinterpret_cast<LPARAM>(&cds));
            }
        }
        if (single_inst_mutex)
        {
            CloseHandle(single_inst_mutex);
        }
        return 0;
    }

    // Register matrix: URI scheme handler under HKCU (no admin required).
    {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exe_path + L"\" \"%1\"";

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\matrix",
                            0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr,
                            &key, nullptr) == ERROR_SUCCESS)
        {
            const wchar_t desc[] = L"URL:matrix Protocol";
            RegSetValueExW(key, nullptr, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(desc), sizeof(desc));
            RegSetValueExW(key, L"URL Protocol", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(L""), sizeof(wchar_t));
            HKEY cmd_key = nullptr;
            if (RegCreateKeyExW(key, L"shell\\open\\command", 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &cmd_key, nullptr) == ERROR_SUCCESS)
            {
                RegSetValueExW(cmd_key, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(cmd.c_str()),
                               static_cast<DWORD>((cmd.size() + 1) *
                                                  sizeof(wchar_t)));
                RegCloseKey(cmd_key);
            }
            RegCloseKey(key);
        }
    }

    // OLE init on the UI thread — required for OLE drag-and-drop (the
    // Surface registers an IDropTarget per HWND). OleInitialize is a
    // superset of CoInitializeEx(COINIT_APARTMENTTHREADED) and matches
    // OleUninitialize 1:1 below.
    if (FAILED(OleInitialize(nullptr)))
    {
        return 1;
    }

    // Required for WinRT toast notifications: associates toasts with this
    // process and initialises the COM apartment for C++/WinRT calls.
    SetCurrentProcessExplicitAppUserModelID(L"io.gnomos.Tesseract");
#if defined(__MINGW32__)
    // No C++/WinRT on mingw: initialise the STA COM apartment directly.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#else
    winrt::init_apartment(winrt::apartment_type::single_threaded);
#endif

    // Register the AUMID in the current-user registry so the WinRT toast
    // notification infrastructure can resolve it.  Non-packaged (classic Win32)
    // apps must have an entry under HKCU\Software\Classes\AppUserModelId\<aumid>
    // or ToastNotificationManager silently drops every Show() call.
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Classes\\AppUserModelId\\io.gnomos.Tesseract", 0,
                nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                nullptr) == ERROR_SUCCESS)
        {
            const wchar_t display[] = L"Tesseract";
            RegSetValueExW(key, L"DisplayName", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(display),
                           sizeof(display));
            // IconUri must be an image file — a bare .exe path does not render
            // in the toast / Action Centre. Materialise the embedded app icon
            // and point at the PNG; fall back to the exe path if that fails.
            std::wstring icon_uri = write_toast_icon_png(hInstance);
            if (icon_uri.empty())
            {
                wchar_t exe_path[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH))
                {
                    icon_uri = exe_path;
                }
            }
            if (!icon_uri.empty())
            {
                RegSetValueExW(key, L"IconUri", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(icon_uri.c_str()),
                               static_cast<DWORD>((icon_uri.size() + 1) *
                                                  sizeof(wchar_t)));
            }
            RegCloseKey(key);
        }
    }

    // Opt into per-monitor v2 DPI awareness so DWM hands us crisp pixels on
    // mixed-DPI setups. Dynamic-loaded: the call only exists on Win10 1703+,
    // and falls back silently to the manifested awareness (or none).
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        if (auto setCtx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(u32, "SetProcessDpiAwarenessContext")))
        {
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    // Enable common controls (status bar, etc.)
    INITCOMMONCONTROLSEX icce{sizeof(icce),
                              ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icce);

    // i18n: initialise locale before any views are constructed.
    // Load persisted settings first so the saved language preference is
    // available when choosing the locale.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());
    {
        // .mo files live next to the exe in i18n/
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
        std::string i18n_dir = (exe_dir / "i18n").string();

        std::string lang = tesseract::Settings::instance().language;
        if (lang == "auto" || lang.empty())
        {
            wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {};
            GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH);
            // Convert to narrow string (locale names are ASCII-safe: "en-US", "es-MX", etc.)
            char narrow[LOCALE_NAME_MAX_LENGTH] = {};
            WideCharToMultiByte(CP_UTF8, 0, locale_name, -1, narrow, sizeof(narrow), nullptr, nullptr);
            // Replace '-' with '_' to match gettext convention (en-US -> en_US)
            for (char* p = narrow; *p; ++p) { if (*p == '-') *p = '_'; }
            lang = narrow;
        }
        tk::set_locale(i18n_dir, lang);
    }

    int exit_code = 1;
    if (win32::MainWindow::register_class(hInstance))
    {
        win32::MainWindow window(hInstance);
        if (window.create(nCmdShow))
        {
            if (!startup_uri.empty())
            {
                window.open_matrix_link(startup_uri);
            }

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                if (window.pre_translate_message(&msg))
                {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exit_code = static_cast<int>(msg.wParam);
        }
    }

    OleUninitialize();
    ReleaseMutex(single_inst_mutex);
    CloseHandle(single_inst_mutex);
    return exit_code;
}
