#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <optional>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace win32
{

class TaskbarProgressModel
{
public:
    struct Snapshot
    {
        std::uint64_t current = 0;
        std::uint64_t total   = 0;
        bool active = false;
    };

    void update(std::uint64_t request_id, std::uint64_t current,
                std::uint64_t total)
    {
        if (request_id == 0 || total == 0) return;
        transfers_[request_id] = {std::min(current, total), total};
    }
    std::optional<Snapshot> finish(std::uint64_t request_id)
    {
        if (!transfers_.contains(request_id)) return std::nullopt;
        auto before = snapshot();
        transfers_.erase(request_id);
        return before;
    }
    Snapshot snapshot() const
    {
        Snapshot out;
        out.active = !transfers_.empty();
        for (const auto& [_, transfer] : transfers_)
        {
            out.current += std::min(transfer.current,
                std::numeric_limits<std::uint64_t>::max() - out.current);
            out.total += std::min(transfer.total,
                std::numeric_limits<std::uint64_t>::max() - out.total);
        }
        return out;
    }
    void clear() { transfers_.clear(); }

private:
    struct Transfer { std::uint64_t current, total; };
    std::unordered_map<std::uint64_t, Transfer> transfers_;
};

class Win32Taskbar
{
public:
    enum class Role { Main, Room, Call };

    static constexpr UINT kNextUnreadButton = 5101;
    static constexpr UINT kMuteButton       = 5102;
    static constexpr UINT kVideoButton      = 5103;
    static constexpr UINT kHangupButton     = 5104;

    explicit Win32Taskbar(HINSTANCE instance);
    ~Win32Taskbar();
    Win32Taskbar(const Win32Taskbar&) = delete;
    Win32Taskbar& operator=(const Win32Taskbar&) = delete;

    UINT taskbar_button_created_message() const { return taskbar_button_created_; }

    void register_main_window(HWND hwnd, std::function<void()> next_unread);
    void register_room_window(HWND hwnd);
    void register_call_window(HWND hwnd,
                              std::function<void()> toggle_audio,
                              std::function<void()> toggle_video,
                              std::function<void()> hang_up);
    void unregister_window(HWND hwnd);
    void on_taskbar_button_created(HWND hwnd);
    bool handle_thumbnail_command(HWND hwnd, WPARAM wparam);

    void set_unread(bool has_unread, bool has_highlight);
    void set_next_unread_available(HWND hwnd, bool available);
    void set_call_state(HWND hwnd, bool audio_muted, bool video_muted,
                        bool show_video);

    void upload_progress(std::uint64_t request_id, std::uint64_t current,
                         std::uint64_t total);
    void upload_finished(std::uint64_t request_id, bool ok);

    void rebuild_jump_list(const std::wstring& quick_switcher,
                           const std::wstring& message_search,
                           const std::wstring& settings);
    void record_recent_room(const std::wstring& room_id,
                            const std::wstring& title);
    void record_recent_room_avatar(const std::wstring& room_id,
                                   std::span<const std::uint8_t> bytes);

private:
    struct Entry
    {
        Role role = Role::Room;
        bool ready = false;
        bool toolbar_added = false;
        bool audio_muted = false;
        bool video_muted = false;
        bool show_video = true;
        bool next_unread_available = false;
        std::function<void()> next_unread;
        std::function<void()> toggle_audio;
        std::function<void()> toggle_video;
        std::function<void()> hang_up;
    };

    static LRESULT CALLBACK message_wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    void apply_window_(HWND hwnd, Entry& entry);
    void apply_overlay_(HWND hwnd);
    void apply_progress_(HWND hwnd);
    void apply_toolbar_(HWND hwnd, Entry& entry);
    void flush_progress_();
    void arm_progress_flush_();
    void clear_error_();
    void rebuild_jump_list_();
    void rebuild_packaged_jump_list_();
    void rebuild_classic_jump_list_();
    void load_recent_rooms_();
    void save_recent_rooms_() const;
    void import_legacy_recent_rooms_();
    std::wstring packaged_avatar_uri_(const std::wstring& room_id) const;
    void prune_packaged_avatars_() const;

    struct RecentRoom
    {
        std::wstring room_id;
        std::wstring title;
    };

    HINSTANCE instance_ = nullptr;
    HWND message_hwnd_ = nullptr;
    UINT taskbar_button_created_ = 0;
    ITaskbarList3* taskbar_ = nullptr;
    HICON unread_icon_ = nullptr;
    HICON highlight_icon_ = nullptr;
    HICON next_icon_ = nullptr;
    HICON mic_icon_ = nullptr;
    HICON mic_off_icon_ = nullptr;
    HICON video_icon_ = nullptr;
    HICON video_off_icon_ = nullptr;
    HICON hangup_icon_ = nullptr;
    bool has_unread_ = false;
    bool has_highlight_ = false;
    bool progress_flush_armed_ = false;
    bool progress_error_ = false;
    TaskbarProgressModel::Snapshot error_snapshot_;
    TaskbarProgressModel progress_;
    std::unordered_map<HWND, Entry> windows_;
    std::vector<RecentRoom> recent_rooms_;
    std::wstring aumid_;
    std::wstring quick_switcher_label_;
    std::wstring message_search_label_;
    std::wstring settings_label_;
};

} // namespace win32
