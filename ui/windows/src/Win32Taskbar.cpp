#include "Win32Taskbar.h"
#include "Win32PackageContext.h"
#include "resource.h"

#include "tk/i18n.h"
#include <tesseract/paths.h>
#include <tesseract/tray_icon.h>

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <unordered_set>
#include <wrl/client.h>

#if !defined(__MINGW32__)
#include "winrt_coroutine_shim.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.StartScreen.h>
#endif

namespace win32
{
namespace
{
constexpr wchar_t kMessageClass[] = L"TesseractTaskbarMessageWindow";
constexpr UINT_PTR kFlushTimer = 1;
constexpr UINT_PTR kErrorTimer = 2;
constexpr wchar_t kRecentRegistryKey[] = L"Software\\Tesseract";
constexpr wchar_t kRecentRegistryValue[] = L"TaskbarRecentRooms";
constexpr std::size_t kRecentRoomLimit = 8;
constexpr char kRecentFileMagic[] = "TesseractRecentRooms 1";

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

std::string to_utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::filesystem::path recent_file_path()
{
    return tesseract::data_dir() / "taskbar-recent-rooms";
}

std::uint64_t room_hash(const std::wstring& room_id)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : to_utf8(room_id))
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring hash_name(const std::wstring& room_id)
{
    wchar_t value[17]{};
    swprintf_s(value, L"%016llx",
               static_cast<unsigned long long>(room_hash(room_id)));
    return value;
}

const wchar_t* image_extension(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
        bytes[2] == 'N' && bytes[3] == 'G') return L".png";
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
        bytes[2] == 0xff) return L".jpg";
    if (bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' &&
        bytes[2] == 'F') return L".gif";
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return L".bmp";
    return nullptr;
}

COLORREF badge_colour(unsigned int colour)
{
    return RGB((colour >> 16) & 0xffu, (colour >> 8) & 0xffu, colour & 0xffu);
}

HICON make_dot_icon(COLORREF colour)
{
    constexpr int side = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = side;
    bi.bV5Height = -side;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP colour_bitmap = CreateDIBSection(
        dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!colour_bitmap || !bits)
        return nullptr;
    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + side * side, 0u);
    const int cr = GetRValue(colour), cg = GetGValue(colour), cb = GetBValue(colour);
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            const int dx = x - 10, dy = y - 10;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= 30)
                pixels[y * side + x] = 0xFFFFFFFFu;
            if (d2 <= 18)
                pixels[y * side + x] = 0xFF000000u |
                    (static_cast<std::uint32_t>(cr) << 16) |
                    (static_cast<std::uint32_t>(cg) << 8) |
                    static_cast<std::uint32_t>(cb);
        }
    }
    HBITMAP mask = CreateBitmap(side, side, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, colour_bitmap};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(colour_bitmap);
    return icon;
}

void set_tip(THUMBBUTTON& button, const wchar_t* text)
{
    wcsncpy_s(button.szTip, text, _TRUNCATE);
}

Microsoft::WRL::ComPtr<IShellLinkW>
make_task_link(const wchar_t* exe, const wchar_t* arguments,
               const wchar_t* title, const wchar_t* aumid)
{
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return {};
    link->SetPath(exe);
    link->SetArguments(arguments);
    link->SetIconLocation(exe, 0);
    Microsoft::WRL::ComPtr<IPropertyStore> props;
    if (SUCCEEDED(link.As(&props)))
    {
        PROPVARIANT value{};
        if (SUCCEEDED(InitPropVariantFromString(title, &value)))
        {
            props->SetValue(PKEY_Title, value);
            PropVariantClear(&value);
        }
        if (SUCCEEDED(InitPropVariantFromString(aumid, &value)))
        {
            props->SetValue(PKEY_AppUserModel_ID, value);
            PropVariantClear(&value);
        }
        props->Commit();
    }
    return link;
}
} // namespace

Win32Taskbar::Win32Taskbar(HINSTANCE instance)
    : instance_(instance), aumid_(package_context::effective_aumid())
{
    load_recent_rooms_();
    taskbar_button_created_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&taskbar_))))
    {
        if (FAILED(taskbar_->HrInit()))
        {
            taskbar_->Release();
            taskbar_ = nullptr;
        }
    }
    unread_icon_ = make_dot_icon(badge_colour(tesseract::kBadgeColorUnread));
    highlight_icon_ = make_dot_icon(badge_colour(tesseract::kBadgeColorMention));
    next_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_NEXT),
                                               IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    mic_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_MIC),
                                              IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    mic_off_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_MIC_OFF),
                                                  IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    video_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_VIDEO),
                                                IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    video_off_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_VIDEO_OFF),
                                                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    hangup_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_TASK_HANGUP),
                                                 IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Win32Taskbar::message_wnd_proc_;
    wc.hInstance = instance_;
    wc.lpszClassName = kMessageClass;
    RegisterClassExW(&wc);
    message_hwnd_ = CreateWindowExW(0, kMessageClass, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, instance_, this);
}

Win32Taskbar::~Win32Taskbar()
{
    if (message_hwnd_)
        DestroyWindow(message_hwnd_);
    for (HICON icon : {unread_icon_, highlight_icon_, next_icon_, mic_icon_,
                       mic_off_icon_, video_icon_, video_off_icon_, hangup_icon_})
        if (icon) DestroyIcon(icon);
    if (taskbar_)
        taskbar_->Release();
}

LRESULT CALLBACK Win32Taskbar::message_wnd_proc_(HWND hwnd, UINT msg,
                                                  WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<Win32Taskbar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Win32Taskbar*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && msg == WM_TIMER)
    {
        if (wp == kFlushTimer)
        {
            KillTimer(hwnd, kFlushTimer);
            self->progress_flush_armed_ = false;
            self->flush_progress_();
            return 0;
        }
        if (wp == kErrorTimer)
        {
            KillTimer(hwnd, kErrorTimer);
            self->clear_error_();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Win32Taskbar::register_main_window(HWND hwnd,
                                         std::function<void()> next_unread)
{
    Entry entry;
    entry.role = Role::Main;
    entry.next_unread = std::move(next_unread);
    windows_[hwnd] = std::move(entry);
}

void Win32Taskbar::register_room_window(HWND hwnd)
{
    windows_[hwnd] = Entry{Role::Room};
}

void Win32Taskbar::register_call_window(HWND hwnd,
    std::function<void()> toggle_audio, std::function<void()> toggle_video,
    std::function<void()> hang_up)
{
    Entry entry;
    entry.role = Role::Call;
    entry.toggle_audio = std::move(toggle_audio);
    entry.toggle_video = std::move(toggle_video);
    entry.hang_up = std::move(hang_up);
    windows_[hwnd] = std::move(entry);
}

void Win32Taskbar::unregister_window(HWND hwnd) { windows_.erase(hwnd); }

void Win32Taskbar::on_taskbar_button_created(HWND hwnd)
{
    auto it = windows_.find(hwnd);
    if (it == windows_.end())
        return;
    it->second.ready = true;
    it->second.toolbar_added = false;
    apply_window_(hwnd, it->second);
}

void Win32Taskbar::apply_window_(HWND hwnd, Entry& entry)
{
    if (!taskbar_ || !entry.ready)
        return;
    apply_overlay_(hwnd);
    apply_progress_(hwnd);
    apply_toolbar_(hwnd, entry);
}

void Win32Taskbar::apply_overlay_(HWND hwnd)
{
    if (!taskbar_)
        return;
    HICON icon = has_highlight_ ? highlight_icon_ : (has_unread_ ? unread_icon_ : nullptr);
    const std::wstring description = has_highlight_
        ? to_wide(tk::tr("Mentions"))
        : (has_unread_ ? to_wide(tk::tr("Unread messages")) : std::wstring{});
    taskbar_->SetOverlayIcon(hwnd, icon, description.c_str());
}

void Win32Taskbar::apply_progress_(HWND hwnd)
{
    if (!taskbar_)
        return;
    auto snapshot = progress_error_ ? error_snapshot_ : progress_.snapshot();
    if (!snapshot.active || snapshot.total == 0)
    {
        taskbar_->SetProgressState(hwnd, TBPF_NOPROGRESS);
        return;
    }
    taskbar_->SetProgressState(hwnd, progress_error_ ? TBPF_ERROR : TBPF_NORMAL);
    taskbar_->SetProgressValue(hwnd, snapshot.current, snapshot.total);
}

void Win32Taskbar::apply_toolbar_(HWND hwnd, Entry& entry)
{
    if (!taskbar_ || entry.role == Role::Room)
        return;
    std::array<THUMBBUTTON, 3> buttons{};
    UINT count = 0;
    if (entry.role == Role::Main)
    {
        auto& b = buttons[0];
        b.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        b.iId = kNextUnreadButton;
        b.hIcon = next_icon_;
        b.dwFlags = entry.next_unread_available ? THBF_ENABLED : THBF_DISABLED;
        const auto tip = to_wide(tk::tr("Next unread room"));
        set_tip(b, tip.c_str());
        count = 1;
    }
    else
    {
        auto& mic = buttons[0];
        mic.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        mic.iId = kMuteButton;
        mic.hIcon = entry.audio_muted ? mic_off_icon_ : mic_icon_;
        mic.dwFlags = THBF_ENABLED;
        const auto mic_tip = to_wide(entry.audio_muted
            ? tk::tr("Unmute microphone") : tk::tr("Mute microphone"));
        set_tip(mic, mic_tip.c_str());
        auto& video = buttons[1];
        video.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        video.iId = kVideoButton;
        video.hIcon = entry.video_muted ? video_off_icon_ : video_icon_;
        video.dwFlags = entry.show_video ? THBF_ENABLED : THBF_HIDDEN;
        const auto video_tip = to_wide(entry.video_muted
            ? tk::tr("Turn camera on") : tk::tr("Turn camera off"));
        set_tip(video, video_tip.c_str());
        auto& hang = buttons[2];
        hang.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        hang.iId = kHangupButton;
        hang.hIcon = hangup_icon_;
        hang.dwFlags = THBF_ENABLED;
        const auto hang_tip = to_wide(tk::tr("Hang up"));
        set_tip(hang, hang_tip.c_str());
        count = 3;
    }
    if (!entry.toolbar_added)
    {
        if (SUCCEEDED(taskbar_->ThumbBarAddButtons(hwnd, count, buttons.data())))
            entry.toolbar_added = true;
    }
    else
        taskbar_->ThumbBarUpdateButtons(hwnd, count, buttons.data());
}

bool Win32Taskbar::handle_thumbnail_command(HWND hwnd, WPARAM wparam)
{
    if (HIWORD(wparam) != THBN_CLICKED)
        return false;
    auto it = windows_.find(hwnd);
    if (it == windows_.end())
        return false;
    auto& e = it->second;
    switch (LOWORD(wparam))
    {
    case kNextUnreadButton: if (e.next_unread) e.next_unread(); return true;
    case kMuteButton: if (e.toggle_audio) e.toggle_audio(); return true;
    case kVideoButton: if (e.toggle_video) e.toggle_video(); return true;
    case kHangupButton: if (e.hang_up) e.hang_up(); return true;
    default: return false;
    }
}

void Win32Taskbar::set_unread(bool unread, bool highlight)
{
    has_unread_ = unread;
    has_highlight_ = highlight;
    for (auto& [hwnd, entry] : windows_)
        if (entry.ready) apply_overlay_(hwnd);
}

void Win32Taskbar::set_next_unread_available(HWND hwnd, bool available)
{
    auto it = windows_.find(hwnd);
    if (it == windows_.end() || it->second.role != Role::Main) return;
    it->second.next_unread_available = available;
    if (it->second.ready) apply_toolbar_(hwnd, it->second);
}

void Win32Taskbar::set_call_state(HWND hwnd, bool audio_muted,
                                   bool video_muted, bool show_video)
{
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return;
    it->second.audio_muted = audio_muted;
    it->second.video_muted = video_muted;
    it->second.show_video = show_video;
    if (it->second.ready) apply_toolbar_(hwnd, it->second);
}

void Win32Taskbar::arm_progress_flush_()
{
    if (!message_hwnd_ || progress_flush_armed_) return;
    progress_flush_armed_ = SetTimer(message_hwnd_, kFlushTimer, 50, nullptr) != 0;
    if (!progress_flush_armed_) flush_progress_();
}

void Win32Taskbar::upload_progress(std::uint64_t id, std::uint64_t current,
                                    std::uint64_t total)
{
    if (progress_error_)
    {
        progress_error_ = false;
        if (message_hwnd_) KillTimer(message_hwnd_, kErrorTimer);
    }
    progress_.update(id, current, total);
    arm_progress_flush_();
}

void Win32Taskbar::upload_finished(std::uint64_t id, bool ok)
{
    auto before = progress_.finish(id);
    if (!before) return;
    if (!ok && before->active)
    {
        progress_error_ = true;
        error_snapshot_ = *before;
        if (message_hwnd_) SetTimer(message_hwnd_, kErrorTimer, 3000, nullptr);
    }
    flush_progress_();
}

void Win32Taskbar::flush_progress_()
{
    for (auto& [hwnd, entry] : windows_)
        if (entry.ready) apply_progress_(hwnd);
}

void Win32Taskbar::clear_error_()
{
    progress_error_ = false;
    error_snapshot_ = {};
    flush_progress_();
}

void Win32Taskbar::rebuild_jump_list(const std::wstring& quick_switcher,
                                      const std::wstring& message_search,
                                      const std::wstring& settings)
{
    quick_switcher_label_ = quick_switcher;
    message_search_label_ = message_search;
    settings_label_ = settings;
    rebuild_jump_list_();
}

void Win32Taskbar::record_recent_room(const std::wstring& room_id,
                                       const std::wstring& title)
{
    if (room_id.empty()) return;
    recent_rooms_.erase(
        std::remove_if(recent_rooms_.begin(), recent_rooms_.end(),
                       [&](const RecentRoom& room) { return room.room_id == room_id; }),
        recent_rooms_.end());
    recent_rooms_.insert(recent_rooms_.begin(),
                         RecentRoom{room_id, title.empty() ? room_id : title});
    if (recent_rooms_.size() > kRecentRoomLimit)
        recent_rooms_.resize(kRecentRoomLimit);
    save_recent_rooms_();
    if (!quick_switcher_label_.empty()) rebuild_jump_list_();
}

void Win32Taskbar::load_recent_rooms_()
{
    std::ifstream input(recent_file_path(), std::ios::binary);
    std::string magic;
    if (input && std::getline(input, magic) && magic == kRecentFileMagic)
    {
        std::string room_id;
        std::string title;
        while (recent_rooms_.size() < kRecentRoomLimit &&
               input >> std::quoted(room_id) >> std::quoted(title))
        {
            if (!room_id.empty())
                recent_rooms_.push_back({to_wide(room_id), to_wide(title)});
        }
        return;
    }
    import_legacy_recent_rooms_();
    if (!recent_rooms_.empty()) save_recent_rooms_();
}

void Win32Taskbar::import_legacy_recent_rooms_()
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRecentRegistryKey,
                     kRecentRegistryValue, RRF_RT_REG_MULTI_SZ, nullptr,
                     nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return;
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRecentRegistryKey,
                     kRecentRegistryValue, RRF_RT_REG_MULTI_SZ, nullptr,
                     data.data(), &bytes) != ERROR_SUCCESS)
        return;
    const wchar_t* cursor = data.data();
    const wchar_t* end = data.data() + data.size();
    while (cursor < end && *cursor && recent_rooms_.size() < kRecentRoomLimit)
    {
        std::wstring room_id(cursor);
        cursor += room_id.size() + 1;
        if (cursor >= end || !*cursor) break;
        std::wstring title(cursor);
        cursor += title.size() + 1;
        recent_rooms_.push_back({std::move(room_id), std::move(title)});
    }
}

void Win32Taskbar::save_recent_rooms_() const
{
    const auto path = recent_file_path();
    const auto temporary = path.wstring() + L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << kRecentFileMagic << '\n';
    for (const auto& room : recent_rooms_)
    {
        output << std::quoted(to_utf8(room.room_id)) << ' '
               << std::quoted(to_utf8(room.title)) << '\n';
    }
    output.close();
    if (!output) return;
    MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Win32Taskbar::rebuild_jump_list_()
{
    if (package_context::is_packaged())
    {
        rebuild_packaged_jump_list_();
        return;
    }
    rebuild_classic_jump_list_();
}

void Win32Taskbar::rebuild_classic_jump_list_()
{
    Microsoft::WRL::ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        return;
    list->SetAppID(aumid_.c_str());
    UINT slots = 0;
    Microsoft::WRL::ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed)))) return;

    std::unordered_set<std::wstring> removed_arguments;
    if (removed)
    {
        UINT count = 0;
        removed->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&link))))
            {
                wchar_t args[2048]{};
                if (SUCCEEDED(link->GetArguments(args, 2048)))
                    removed_arguments.emplace(args);
            }
        }
    }

    Microsoft::WRL::ComPtr<IObjectCollection> tasks;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks))))
    {
        list->AbortList();
        return;
    }
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
    {
        list->AbortList();
        return;
    }
    const auto add_task = [&](const std::wstring& args, const std::wstring& title) {
        if (removed_arguments.contains(args)) return;
        if (auto link = make_task_link(exe, args.c_str(), title.c_str(),
                                       aumid_.c_str()))
            tasks->AddObject(link.Get());
    };
    add_task(L"--open-quick-switcher", quick_switcher_label_);
    add_task(L"--open-message-search", message_search_label_);
    add_task(L"--open-settings", settings_label_);

    if (!recent_rooms_.empty())
    {
        Microsoft::WRL::ComPtr<IObjectCollection> recent;
        if (SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&recent))))
        {
            UINT added = 0;
            for (const auto& room : recent_rooms_)
            {
                const std::wstring args = L"--open-room=\"" + room.room_id + L"\"";
                if (removed_arguments.contains(args)) continue;
                if (auto link = make_task_link(exe, args.c_str(),
                                               room.title.c_str(),
                                               aumid_.c_str()))
                {
                    recent->AddObject(link.Get());
                    ++added;
                }
            }
            Microsoft::WRL::ComPtr<IObjectArray> recent_array;
            const auto category = to_wide(tk::tr("Recent rooms"));
            if (added > 0 && SUCCEEDED(recent.As(&recent_array)))
                list->AppendCategory(category.c_str(), recent_array.Get());
        }
    }
    Microsoft::WRL::ComPtr<IObjectArray> array;
    if (SUCCEEDED(tasks.As(&array)) && SUCCEEDED(list->AddUserTasks(array.Get())))
        list->CommitList();
    else
        list->AbortList();
}

void Win32Taskbar::record_recent_room_avatar(
    const std::wstring& room_id, std::span<const std::uint8_t> bytes)
{
    if (!package_context::is_packaged() || room_id.empty() || bytes.empty())
        return;
    const wchar_t* extension = image_extension(bytes);
    const auto local_state = package_context::local_state_path();
    if (!extension || local_state.empty()) return;

    const auto directory = local_state / L"jump-list";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return;
    const auto stem = L"room-" + hash_name(room_id);
    for (const auto* old_extension : {L".png", L".jpg", L".gif", L".bmp"})
    {
        if (wcscmp(old_extension, extension) != 0)
            std::filesystem::remove(directory / (stem + old_extension), ec);
        ec.clear();
    }
    const auto path = directory / (stem + extension);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return;
    prune_packaged_avatars_();
    if (!quick_switcher_label_.empty()) rebuild_jump_list_();
}

std::wstring Win32Taskbar::packaged_avatar_uri_(
    const std::wstring& room_id) const
{
    const auto directory = package_context::local_state_path() / L"jump-list";
    const auto stem = L"room-" + hash_name(room_id);
    std::error_code ec;
    for (const auto* extension : {L".png", L".jpg", L".gif", L".bmp"})
    {
        if (std::filesystem::exists(directory / (stem + extension), ec))
            return L"ms-appdata:///local/jump-list/" + stem + extension;
        ec.clear();
    }
    return L"ms-appx:///Assets/Room.png";
}

void Win32Taskbar::prune_packaged_avatars_() const
{
    const auto directory = package_context::local_state_path() / L"jump-list";
    std::unordered_set<std::wstring> keep;
    for (const auto& room : recent_rooms_)
        keep.emplace(L"room-" + hash_name(room.room_id));
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(directory, ec))
    {
        if (!item.is_regular_file()) continue;
        if (!keep.contains(item.path().stem().wstring()))
            std::filesystem::remove(item.path(), ec);
        ec.clear();
    }
}

void Win32Taskbar::rebuild_packaged_jump_list_()
{
#if !defined(__MINGW32__)
    try
    {
        using winrt::Windows::Foundation::Uri;
        using winrt::Windows::UI::StartScreen::JumpList;
        using winrt::Windows::UI::StartScreen::JumpListItem;
        if (!JumpList::IsSupported()) return;
        auto list = JumpList::LoadCurrentAsync().get();
        std::unordered_set<std::wstring> removed;
        for (const auto& item : list.Items())
            if (item.RemovedByUser()) removed.emplace(item.Arguments().c_str());
        list.Items().Clear();

        const auto append = [&](const std::wstring& arguments,
                                const std::wstring& title,
                                const std::wstring& group,
                                const std::wstring& logo)
        {
            if (removed.contains(arguments)) return;
            auto item = JumpListItem::CreateWithArguments(arguments, title);
            if (!group.empty()) item.GroupName(group);
            if (!logo.empty()) item.Logo(Uri(logo));
            list.Items().Append(item);
        };
        append(L"--open-quick-switcher", quick_switcher_label_, L"",
               L"ms-appx:///Assets/Task.png");
        append(L"--open-message-search", message_search_label_, L"",
               L"ms-appx:///Assets/Task.png");
        append(L"--open-settings", settings_label_, L"",
               L"ms-appx:///Assets/Task.png");
        const auto group = to_wide(tk::tr("Recent rooms"));
        for (const auto& room : recent_rooms_)
        {
            append(L"--open-room=\"" + room.room_id + L"\"", room.title,
                   group, packaged_avatar_uri_(room.room_id));
        }
        list.SaveAsync().get();
    }
    catch (const winrt::hresult_error&)
    {
        // Package identity can exist on shells that do not expose JumpList.
    }
#endif
}

} // namespace win32
