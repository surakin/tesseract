#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Win32PickerPopup.h"
#include "app/RoomWindowBase.h"
#include "tk/host_win32.h"
#include "views/EmojiPicker.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"
#include "views/StickerPicker.h"

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
    void repaint_anim_frame() override;

protected:
    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return text_area_.get();
    }
    tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                      std::size_t size, bool compress) override
    {
        return surface_ ? surface_->host().encode_for_send(data, size, compress)
                        : tk::EncodedImage{};
    }

private:
    static LRESULT CALLBACK wnd_proc_(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_msg_(HWND, UINT, WPARAM, LPARAM);
    void show_mention_popup_(tk::Rect cursor_rect, int rows);
    void hide_mention_popup_();
    void show_slash_popup_(tk::Rect cursor_rect, int rows);
    void show_shortcode_popup_(tk::Rect cursor_rect, int rows);
    void show_gif_popup_();
    void hide_gif_popup_();

    // Build the emoji + sticker pickers (lazy, on first use). The emoji picker
    // doubles as the reaction picker via pending_reaction_event_id_.
    void ensure_pickers_();
    // Emoji-picker selection handlers: insert into the composer, or — when
    // pending_reaction_event_id_ is set — send a reaction to that event.
    void on_picker_glyph_(const std::string& glyph);
    void on_picker_emoticon_(const tesseract::ImagePackImage& img);
    // Native tooltip (reaction / read-receipt hover), owned by hwnd_.
    void show_tooltip_(const std::string& text, tk::Rect anchor);
    void hide_tooltip_();
    bool mention_popup_visible_() const
    {
        return mention_popup_hwnd_ && IsWindowVisible(mention_popup_hwnd_);
    }

    MainWindow* parent_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> surface_;
    std::unique_ptr<tk::NativeTextArea> text_area_;
    HWND mention_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    HWND slash_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> slash_popup_surface_;
    tesseract::views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    HWND shortcode_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> shortcode_popup_surface_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    HWND gif_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> gif_popup_surface_;
    tesseract::views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;

    // Emoji / sticker pickers (own their own popup windows). The picker
    // widgets are borrowed (owned by the popup's surface). The emoji popup is
    // reused as the reaction picker.
    std::unique_ptr<Win32PickerPopup> emoji_popup_;
    std::unique_ptr<Win32PickerPopup> sticker_popup_;
    tesseract::views::EmojiPicker* emoji_picker_ = nullptr;     // borrowed
    tesseract::views::StickerPicker* sticker_picker_ = nullptr; // borrowed
    // Set by on_add_reaction_requested; consumed by the next picker selection
    // to send a reaction instead of inserting text. Empty in compose mode.
    std::string pending_reaction_event_id_;

    // Native tracking tooltip + its UTF-16 text backing store.
    HWND tooltip_hwnd_ = nullptr;
    std::vector<wchar_t> tooltip_text_;

    static constexpr const wchar_t* kClassName = L"TesseractRoomWnd";
    static bool class_registered_;
};

} // namespace win32
