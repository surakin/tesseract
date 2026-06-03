#pragma once
#include "tk/host.h" // tk::NativeTextArea, tk::Rect
#include "views/MentionEngine.h"
#include "views/MentionPopup.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

// Drives `@mention` autocomplete for a single composer. Owns the engine,
// candidate state, and the async member fetch; the shell supplies a borrowed
// NativeTextArea, a borrowed MentionPopup (mounted in the shell's own surface),
// and surface-plumbing hooks. Stays platform-agnostic so every composer (main
// window + each pop-out, on every shell) shares one implementation.
//
// Usage from a shell:
//   - create a MentionPopup, mount it in a surface, pass its pointer here
//   - text area on_changed:  if not handled by shortcode, call on_text_changed()
//   - text area popup-nav:   route to on_nav()
//   - text area on_submit:   if on_submit() returns true, do NOT send
class MentionController
{
public:
    struct Hooks
    {
        // Position + show the popup surface anchored at `cursor` (surface-local),
        // sized for `rows` rows. The popup widget is already mounted by the shell.
        std::function<void(tk::Rect cursor, int rows)> show;
        std::function<void()> hide;
        std::function<void()> repaint;
        std::function<std::string()> room_id; // active room (member cache key)
        // Live client getter. Optional: when set, it is queried on every fetch
        // so the controller always sees the current client even if it was
        // constructed before login (the Win32 main window builds its controller
        // in on_create, before client_ is assigned). Callers that construct the
        // controller after login can leave this null and rely on the ctor
        // snapshot instead.
        std::function<tesseract::Client*()> client;
        // Prefetch a candidate's avatar (mxc URL) into the shell's avatar cache
        // so the popup's image provider can resolve it on a later repaint.
        // Optional: when null, candidates render initials discs only. The shell
        // is also responsible for setting the popup's image_provider (it reads
        // the shell-owned cache) and for repainting the popup surface when the
        // avatar decodes.
        std::function<void(const std::string& mxc)> fetch_avatar;
        // ShellBase worker / UI-thread plumbing (members fetch must be off-thread).
        std::function<void(std::function<void()>)> run_async;
        std::function<void(std::function<void()>)> post_to_ui;
    };

    MentionController(tk::NativeTextArea* text_area, tesseract::Client* client,
                      MentionPopup* popup, Hooks hooks);
    ~MentionController();

    // Returns true if an `@`-prefix was handled (popup shown or member fetch
    // kicked off) — the caller should stop further on_changed processing.
    bool on_text_changed(const std::string& text, int cursor_byte_pos);
    // Keyboard navigation while the popup is open; returns true if consumed.
    bool on_nav(tk::NativeTextArea::NavKey nk);
    // Enter pressed: accept the selected candidate when visible. Returns true
    // if a candidate was accepted (the caller must NOT send the message).
    bool on_submit();

    bool visible() const
    {
        return visible_;
    }
    void hide();

private:
    void accept(const MentionCandidate& c);

    tk::NativeTextArea* text_area_;
    tesseract::Client* client_;
    MentionPopup* popup_;
    Hooks hooks_;
    MentionEngine engine_;
    MentionMatch active_match_{};
    std::vector<MentionCandidate> candidates_;
    std::vector<tesseract::RoomMember> cached_members_;
    std::string cached_members_room_;
    std::string fetching_room_;
    bool visible_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace tesseract::views
