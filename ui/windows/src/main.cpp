#include "MainWindow.h"
#include "Win32PackageContext.h"
#include "Win32Taskbar.h"
#include "app/AccountManager.h"
#include "resource.h"
#include <ole2.h>
// <shobjidl.h> declares SetCurrentProcessExplicitAppUserModelID on the Windows SDK.
#include <shobjidl.h>
#include "winrt_coroutine_shim.h" // must precede any <winrt/...> include
#include <winrt/base.h>
#include <filesystem>
#include <fstream>
#include <mfapi.h>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <vector>
#include "tk/i18n.h"
#include "app/Launch.h"
#include "app/UpdateChecker.h"
#include <tesseract/client.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

namespace
{

std::wstring to_wide(const std::string& text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), count);
    return result;
}

std::string to_utf8(const wchar_t* text)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 1) return {};
    // `len` includes the terminating NUL. Give the conversion API room for
    // it, then remove it.
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    out.pop_back();
    return out;
}

// argv[1..] as UTF-8, the form tesseract::prepare_launch() takes on every
// platform. Empty arguments are kept (e.g. `--profile ""`) so the parser
// sees exactly what was typed.
std::vector<std::string> command_line_utf8_args()
{
    std::vector<std::string> args;
    int nArgs = 0;
    LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (szArgList)
    {
        for (int i = 1; i < nArgs; ++i)
        {
            args.push_back(to_utf8(szArgList[i]));
        }
        LocalFree(szArgList);
    }
    return args;
}

// Console output for --help / --version / warnings. tesseract.exe is a
// GUI-subsystem binary, so it has no console of its own: output goes to an
// inherited redirect (`tesseract --help > out.txt`, pipes) when there is one,
// else to the launching terminal's console via AttachConsole. cmd.exe and
// PowerShell don't wait for GUI apps, so the text can land after the prompt
// has already been redrawn — a known limitation of GUI-subsystem apps.
void write_console(DWORD which, std::string_view text)
{
    static const bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE)
    {
        if (!attached) return;
        static HANDLE conout = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                                           FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
        h = conout;
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(h, &mode))
    {
        // A real console: write UTF-16 so non-ASCII (translated help)
        // renders regardless of the console code page.
        const std::wstring wide = to_wide(std::string(text));
        WriteConsoleW(h, wide.data(), static_cast<DWORD>(wide.size()), &written,
                      nullptr);
    }
    else
    {
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written,
                  nullptr);
    }
}

// Per-profile so named profiles run side by side.
std::wstring single_instance_mutex_name()
{
    return L"io.gnomos.Tesseract.SingleInstanceMutex" +
           to_wide(tesseract::profile_suffix());
}

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
    const std::vector<std::string> utf8_args = command_line_utf8_args();
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    // Must run before prepare_launch() loads settings: keep the fixture
    // isolated from real sessions, settings, caches, and geometry.
    // Environment changes are process-local.
    if (tesseract::parse_launch_args(utf8_args).screenshot_dir)
    {
        const auto isolated =
            std::filesystem::temp_directory_path() / L"TesseractScreenshotMode";
        SetEnvironmentVariableW(L"APPDATA", isolated.c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA", isolated.c_str());
    }
#endif

    // Shared startup pipeline (ui/shared/app/Launch.h): parses argv, selects
    // --profile, loads settings + locale, and handles --help / --version /
    // --logoutall.
    tesseract::LaunchHooks hooks;
    hooks.detect_system_lang = []
    {
        wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {};
        GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH);
        // Convert to narrow string (locale names are ASCII-safe: "en-US", "es-MX", etc.)
        char narrow[LOCALE_NAME_MAX_LENGTH] = {};
        WideCharToMultiByte(CP_UTF8, 0, locale_name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        // Replace '-' with '_' to match gettext convention (en-US -> en_US)
        for (char* p = narrow; *p; ++p) { if (*p == '-') *p = '_'; }
        return std::string(narrow);
    };
    hooks.i18n_dir = []
    {
        // .mo files live next to the exe in i18n/
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        return (std::filesystem::path(exe_path).parent_path() / "i18n").string();
    };
    hooks.write_stdout = [](std::string_view text)
    { write_console(STD_OUTPUT_HANDLE, text); };
    hooks.write_stderr = [](std::string_view text)
    { write_console(STD_ERROR_HANDLE, text); };
    hooks.acquire_instance_lock = []
    {
        // Held (leaked) until exit, like the normal-startup mutex below.
        HANDLE m = CreateMutexW(nullptr, TRUE, single_instance_mutex_name().c_str());
        return m && GetLastError() != ERROR_ALREADY_EXISTS;
    };
    hooks.program_name = "tesseract.exe";
    const tesseract::LaunchPlan plan = tesseract::prepare_launch(utf8_args, hooks);
    if (plan.exit_code)
    {
        return *plan.exit_code;
    }
    const tesseract::LaunchArgs& launch = plan.args;
    std::string startup_uri = launch.matrix_uri.value_or(std::string{});
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    const bool screenshot_mode = launch.screenshot_dir.has_value();
#endif

    // Single-instance guard (per --profile): if another process already
    // holds this mutex, find its main window, bring it to the foreground,
    // and exit.
    HANDLE single_inst_mutex = CreateMutexW(
        nullptr, TRUE,
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        screenshot_mode ? L"io.gnomos.Tesseract.ScreenshotModeMutex" :
#endif
        single_instance_mutex_name().c_str());
    if (!single_inst_mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        if (screenshot_mode)
        {
            if (single_inst_mutex)
                CloseHandle(single_inst_mutex);
            return 0;
        }
#endif
        // A hidden/autostart launch with nothing to forward has no meaningful
        // action against an already-running instance — exit quietly without
        // forwarding or raising it.
        if (plan.should_raise_existing_instance())
        {
            if (HWND existing = FindWindowW(win32::MainWindow::class_name(), nullptr))
            {
                // The running instance may be hidden to the tray (not just
                // minimized) — relaunching it should bring it back, the same
                // as clicking the tray icon would.
                if (!IsWindowVisible(existing))
                {
                    ShowWindow(existing, SW_SHOW);
                }
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
                if (launch.action == tesseract::LaunchAction::Room &&
                    launch.room_id)
                {
                    // lpData is non-const PVOID; send from a local copy.
                    std::string room_id = *launch.room_id;
                    COPYDATASTRUCT cds{};
                    cds.dwData = 3; // recent room ID
                    cds.cbData = static_cast<DWORD>(room_id.size() + 1);
                    cds.lpData = room_id.data();
                    SendMessageW(existing, WM_COPYDATA,
                                 reinterpret_cast<WPARAM>(nullptr),
                                 reinterpret_cast<LPARAM>(&cds));
                }
                else if (launch.action != tesseract::LaunchAction::None)
                {
                    tesseract::LaunchAction action = launch.action;
                    COPYDATASTRUCT cds{};
                    cds.dwData = 2; // typed launch action
                    cds.cbData = sizeof(action);
                    cds.lpData = &action;
                    SendMessageW(existing, WM_COPYDATA,
                                 reinterpret_cast<WPARAM>(nullptr),
                                 reinterpret_cast<LPARAM>(&cds));
                }
            }
        }
        if (single_inst_mutex)
        {
            CloseHandle(single_inst_mutex);
        }
        return 0;
    }

    // MSIX installs are updated by the Store or App Installer, not by the
    // GitHub release check (which would point at the NSIS installer).
    if (win32::package_context::is_packaged())
        tesseract::disable_update_checks();

    // Packaged installs declare matrix: in AppxManifest.xml. Only the NSIS /
    // developer build owns the equivalent HKCU registration.
    if (
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        !screenshot_mode &&
#else
        true &&
#endif
        !win32::package_context::is_packaged()
    ) {
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
    // effective_aumid() is per --profile for unpackaged builds, so each
    // profile gets its own taskbar group and Jump List.
    const std::wstring aumid = win32::package_context::effective_aumid();
    if (!win32::package_context::is_packaged())
        SetCurrentProcessExplicitAppUserModelID(aumid.c_str());
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Register the AUMID in the current-user registry so the WinRT toast
    // notification infrastructure can resolve it.  Non-packaged (classic Win32)
    // apps must have an entry under HKCU\Software\Classes\AppUserModelId\<aumid>
    // or ToastNotificationManager silently drops every Show() call.
    if (
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        !screenshot_mode &&
#else
        true &&
#endif
        !win32::package_context::is_packaged()
    ) {
        HKEY key = nullptr;
        const std::wstring aumid_key = L"Software\\Classes\\AppUserModelId\\" + aumid;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER, aumid_key.c_str(), 0,
                nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                nullptr) == ERROR_SUCCESS)
        {
            const std::wstring display =
                L"Tesseract" + (tesseract::profile().empty()
                                    ? std::wstring{}
                                    : L" (" + to_wide(tesseract::profile()) + L")");
            RegSetValueExW(key, L"DisplayName", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(display.c_str()),
                           static_cast<DWORD>((display.size() + 1) *
                                              sizeof(wchar_t)));
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

    // Settings, crash handler and locale were set up by prepare_launch().

    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    win32::Win32Taskbar taskbar(hInstance);
    taskbar.rebuild_jump_list(
        to_wide(tk::tr("Open quick switcher")),
        to_wide(tk::tr("Search messages")),
        to_wide(tk::tr("Open settings")));

    int exit_code = 1;
    if (win32::MainWindow::register_class(hInstance))
    {
        tesseract::AccountManager account_manager;
        win32::MainWindow window(account_manager, hInstance, taskbar,
                                 plan.start_hidden(), launch.action,
                                 launch.room_id.value_or(std::string{})
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                                 , launch.screenshot_dir
                                     ? std::filesystem::u8path(*launch.screenshot_dir)
                                     : std::filesystem::path{}
#endif
        );
        // start_login()'s async restore completion force-shows the window
        // (ShowWindow(hwnd_, SW_SHOW)) if there's no saved session to
        // restore; otherwise it stays hidden through a successful silent
        // restore.
        if (window.create(
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                screenshot_mode ? SW_SHOW :
#endif
                (plan.start_hidden() ? SW_HIDE : nCmdShow)))
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

    MFShutdown();
    OleUninitialize();
    ReleaseMutex(single_inst_mutex);
    CloseHandle(single_inst_mutex);
    return exit_code;
}
