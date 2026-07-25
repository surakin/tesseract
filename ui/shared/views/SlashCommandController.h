#pragma once
#include "tk/text_area.h" // tk::TextArea, tk::NavKey
#include "views/SlashCommandEngine.h"
#include "views/SlashCommandPopup.h"

#include <tesseract/bot_command.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

// Drives the `/command` autocomplete popup for a single composer. Owns the
// engine and selection state; the shell supplies a borrowed NativeTextArea, a
// borrowed SlashCommandPopup (mounted in the shell's own surface), and
// surface/SDK plumbing hooks. Platform-agnostic so every composer (main window
// + each pop-out, on every shell) shares one implementation — mirrors
// MentionController / GifController.
//
// Usage from a shell:
//   - create a SlashCommandPopup, mount it in a surface, pass its pointer here
//   - text area on_changed:  call on_text_changed() (see ComposePopups.h for the
//                            priority dispatch across all composer popups)
//   - text area popup-nav:   route to on_nav()
//   - text area on_submit:   if on_submit() returns true, do NOT send the text
class SlashCommandController
{
public:
    struct Hooks
    {
        // Position + show the popup surface anchored at `cursor` (surface-local),
        // sized for `rows` rows. The popup widget is already mounted by the shell.
        std::function<void(tk::Rect cursor, int rows)> show;
        std::function<void()> hide;
        std::function<void()> repaint;
        std::function<std::string()> room_id; // active room for the send
        std::function<tesseract::Client*()> client;
        // Clear the shell's shared composer mirror (RoomView::clear_compose_text)
        // after an arg-less command is dispatched. The controller already empties
        // the native text area; this keeps the view's cached text in sync.
        std::function<void()> clear_composer;
        // Open the selfie camera overlay. Called instead of dispatch_compose_send
        // when the user accepts /selfie. No-op if unset (e.g. popout windows).
        std::function<void()> on_selfie;
        // Fetch the device's current location and send it as an m.location
        // event. Called instead of dispatch_compose_send when the user accepts
        // /location. No-op if unset.
        std::function<void()> on_location;
        // MSC4391 bot commands currently known for the active room (already
        // filtered to joined senders — see Client::list_room_bot_commands).
        // Called on every keystroke while the popup could be shown; no-op
        // (empty list) if unset, so shells that haven't wired this yet just
        // see built-in commands, same as before this feature existed.
        std::function<std::vector<tesseract::CommandDescription>()> bot_commands;
    };

    SlashCommandController(tk::TextArea* text_area,
                           SlashCommandPopup* popup, Hooks hooks);
    ~SlashCommandController();

    // Returns true if a `/`-prefix was handled (popup shown) — the caller should
    // stop further on_changed processing.
    bool on_text_changed(const std::string& text, int cursor_byte_pos);
    // Keyboard navigation while the popup is open; returns true if consumed.
    bool on_nav(tk::NavKey nk);
    // Enter pressed: accept the selected command when visible. Returns true if a
    // command was accepted (the caller must NOT send the message).
    bool on_submit();

    bool visible() const
    {
        return visible_;
    }
    void hide();

    // True while collecting positional arguments for a previously-accepted
    // bot command (i.e. showing the hint row, not a suggestion list). The
    // shell's send handler checks this before falling through to the normal
    // dispatch_compose_send path — see ShellBase::dispatch_room_send_.
    bool collecting_bot_command_args() const
    {
        return active_bot_command_.has_value();
    }

private:
    void accept(const SlashCommandSuggestion& s);
    // Recompute and show the argument hint (or error, if `error` is set)
    // for the in-progress bot command, given the composer text after the
    // "/command_name " prefix.
    void update_arg_hint(const std::string& args_text,
                         const std::string& error = {});
    // Validate + send the in-progress bot command. Returns true (and clears
    // composer/state) on success; on failure, shows the error in the hint
    // row and leaves the composer text untouched so the user can fix it.
    bool submit_bot_command();

    tk::TextArea* text_area_;
    SlashCommandPopup* popup_;
    Hooks hooks_;
    SlashCommandEngine engine_;
    bool visible_ = false;
    // Set while collecting_bot_command_args() — the suggestion the user
    // accepted, carrying the full parameter list needed for hinting/
    // validation/send.
    std::optional<SlashCommandSuggestion> active_bot_command_;
};

} // namespace tesseract::views
