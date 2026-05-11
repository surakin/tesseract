#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <atomic>
#include <string>
#include <thread>

namespace win32 {

/// Modal dialog driving the Matrix key-recovery flow (Step 6).
///
/// Two states toggled in place (form vs. waiting). recover() runs on a
/// std::thread and posts WM_APP messages back to the dialog HWND. The
/// waiting state's Close button is disabled until on_backup_progress
/// reaches BackupState::Enabled.
class RecoveryDialog {
public:
    RecoveryDialog(HWND hParent, HINSTANCE hInst, tesseract::Client& client);
    ~RecoveryDialog();

    RecoveryDialog(const RecoveryDialog&)            = delete;
    RecoveryDialog& operator=(const RecoveryDialog&) = delete;

    bool run();

    /// Forward a backup-progress update from the MainWindow event handler.
    /// Must be called on the GUI thread.
    void set_progress(const tesseract::BackupProgress& progress);

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    static bool register_class(HINSTANCE);

    void on_create();
    void on_command(WPARAM wParam);
    void on_recover_completed(bool ok, std::wstring text);

    void show_form();
    void show_waiting();
    void set_error(const std::wstring& msg);
    void start_recover();
    void join_worker();

    HWND      hParent_ = nullptr;
    HINSTANCE hInst_   = nullptr;
    HWND      hwnd_    = nullptr;

    HWND      hIntro_    = nullptr;
    HWND      hKeyLabel_ = nullptr;
    HWND      hKeyEdit_  = nullptr;
    HWND      hError_    = nullptr;
    HWND      hStatus_   = nullptr;
    HWND      hVerify_   = nullptr;
    HWND      hSkip_     = nullptr;
    HWND      hClose_    = nullptr;

    tesseract::Client& client_;

    std::thread       worker_;
    std::atomic<bool> cancelled_   { false };
    bool              accepted_    = false;
    bool              recover_done_ = false;

    static constexpr const wchar_t* CLASS_NAME = L"TesseractRecoveryDlg";
    static constexpr int IDC_KEY    = 301;
    static constexpr int IDC_VERIFY = 302;
    static constexpr int IDC_SKIP   = 303;
    static constexpr int IDC_CLOSE  = 304;
};

// Distinct from LoginDialog's WM_APP range so a stale message can't leak.
constexpr UINT WM_RECOVERY_DONE = WM_APP + 200;

} // namespace win32
