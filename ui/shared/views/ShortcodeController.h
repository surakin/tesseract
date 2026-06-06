#pragma once
#include "tk/host.h" // tk::NativeTextArea, tk::NavKey
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"

#include <tesseract/image_pack.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

// Drives `:shortcode:` emoji/emoticon autocomplete for a single composer: both
// the auto-expansion of a completed `:smile:` into its glyph and the suggestion
// popup for an open `:smi` prefix. Owns the engine, the active match, and the
// current suggestion list; the shell supplies a borrowed NativeTextArea, a
// borrowed ShortcodePopup (mounted in the shell's own surface), the live
// emoticon pack list, and surface-plumbing hooks. Platform-agnostic so every
// composer (main window + each pop-out, on every shell) shares one
// implementation — mirrors MentionController / GifController.
//
// Usage from a shell:
//   - create a ShortcodePopup, set its image_provider, mount it in a surface,
//     pass its pointer here
//   - text area on_changed:  call on_text_changed() (see ComposePopups.h for the
//                            priority dispatch across all composer popups)
//   - text area popup-nav:   route to on_nav()
//   - text area on_submit:   if on_submit() returns true, do NOT send the text
class ShortcodeController
{
public:
    struct Hooks
    {
        // Position + show the popup surface anchored at `cursor` (surface-local),
        // sized for `rows` rows. The popup widget is already mounted by the shell.
        std::function<void(tk::Rect cursor, int rows)> show;
        std::function<void()> hide;
        std::function<void()> repaint;
        // The live custom-emoticon pack list (shell-owned cache) used to rank
        // suggestions alongside the Unicode set.
        std::function<const std::vector<tesseract::ImagePackImage>&()> emoticons;
        // Prefetch a custom emoticon's image (mxc URL) into the shell's media
        // cache so the popup's image_provider can resolve it on a later repaint.
        // Optional: when null, custom emoticons render without a thumbnail.
        std::function<void(const std::string& url)> fetch_image;
    };

    ShortcodeController(tk::NativeTextArea* text_area, ShortcodePopup* popup,
                        Hooks hooks);
    ~ShortcodeController();

    // Returns true if a shortcode was handled — either a completed `:word:` was
    // auto-expanded, or an open `:prefix` showed the popup. The caller should
    // stop further on_changed processing.
    bool on_text_changed(const std::string& text, int cursor_byte_pos);
    // Keyboard navigation while the popup is open; returns true if consumed.
    bool on_nav(tk::NavKey nk);
    // Enter pressed: accept the selected suggestion when visible. Returns true
    // if a suggestion was accepted (the caller must NOT send the message).
    bool on_submit();

    bool visible() const
    {
        return visible_;
    }
    void hide();

private:
    void accept(const ShortcodeSuggestion& s);
    // Replace the active `:prefix` range with `r` and dismiss the popup.
    void replace_with(const std::string& r);

    tk::NativeTextArea* text_area_;
    ShortcodePopup* popup_;
    Hooks hooks_;
    ShortcodeEngine engine_;
    ShortcodeMatch active_match_{};
    std::vector<ShortcodeSuggestion> suggestions_;
    bool visible_ = false;

    static constexpr int kMinPrefix = 2; // need ":ab" before the popup opens
};

} // namespace tesseract::views
