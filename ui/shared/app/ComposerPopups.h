#pragma once
#include "tk/host.h"
#include "tk/theme.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
class TextArea;
}

namespace tesseract
{

class RoomPane;

namespace views
{
class RoomView;
}

// The composer's four autocomplete popups (@mention, /command, :shortcode:,
// /gif strip) for one room pane, in a pop-out room window or the main window
// (ShellBase::compose_popups_): popup surface + popup widget + controller
// each, the hooks wiring via RoomPane, and the compose text-area callbacks
// that drive them through views/ComposePopups.h. By default it also sends the
// room's typing notice (start/stop) as the compose text goes non-empty/empty;
// the main window replaces that via Options::on_text_changed.
//
// Declare the owning shell's `std::unique_ptr<ComposerPopups>` (including
// ShellBase::compose_popups_) AFTER the Surface/Host it was built from, so the
// popups are destroyed first. The controllers hold raw pointers to the text
// area and popup widgets.
class ComposerPopups
{
public:
    // What differs between the main window's composer and a pop-out's. All
    // optional; the defaults are the pop-out behaviour.
    struct Options
    {
        // Called with the compose text on every change, in place of the
        // built-in typing notice (the main window tracks typing across room
        // switches itself).
        std::function<void(const std::string&)> on_text_changed;
        // Add commands only the main window has (/selfie) to the slash hooks.
        std::function<void(views::SlashCommandController::Hooks&)> extend_slash_hooks;
        // Image lookup for custom-emoticon thumbnails that also fetches on a
        // miss; the default only peeks the shell's cache.
        std::function<const tk::Image*(const std::string&)> shortcode_image_provider;
    };

    // Takes over text_area's on_changed / on_submit / popup-nav callbacks.
    // `host` is the surface's Host that the popup surfaces are made from;
    // pane / room_view / text_area must outlive this object.
    ComposerPopups(tk::Host& host, tk::TextArea* text_area, RoomPane* pane,
                   views::RoomView* room_view);
    ComposerPopups(tk::Host& host, tk::TextArea* text_area, RoomPane* pane,
                   views::RoomView* room_view, Options options);
    ~ComposerPopups();

    ComposerPopups(const ComposerPopups&) = delete;
    ComposerPopups& operator=(const ComposerPopups&) = delete;

    // Close every popup (e.g. on window resize or room switch).
    void hide_all();

    // A bot's command list changed: refresh the slash popup if it is open.
    void on_bot_commands_changed();

    // A decoded image landed: redraw the popup that may show it. Avatars
    // (the @mention list) only repaint; any other image (a custom emoticon in
    // the :shortcode: list) re-measures.
    void on_avatar_loaded();
    void on_image_loaded();

    // Position + show the /gif strip above the compose bar / hide it.
    void show_gif_popup();
    void hide_gif_popup();

    // Async GIF search results; only the controller that issued the request
    // matches, so every pop-out can forward every result.
    void on_gif_results(std::uint64_t request_id, std::vector<GifResult> results);
    void on_gif_search_failed(std::uint64_t request_id, const std::string& message);

    // Re-theme the popup surfaces (all four popup surfaces).
    void apply_theme(const tk::Theme& theme);

    // Animation tick: repaint the /gif strip when it is on screen.
    void repaint_anim_frame();

private:
    void refresh_(tk::PopupSurfaceHandle* popup, bool layout = true);

    tk::TextArea* text_area_;
    RoomPane* pane_;
    views::RoomView* room_view_;
    Options options_;
    bool typing_active_ = false;        // typing-notice debounce
    bool anim_partial_repaint_ = false; // the strip has the shared anim cache

    std::unique_ptr<tk::PopupSurfaceHandle> mention_popup_;
    views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<views::MentionController> mention_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> slash_popup_;
    views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<views::SlashCommandController> slash_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> shortcode_popup_;
    views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<views::ShortcodeController> shortcode_controller_;

    std::unique_ptr<tk::PopupSurfaceHandle> gif_popup_;
    views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<views::GifController> gif_controller_;
};

} // namespace tesseract
