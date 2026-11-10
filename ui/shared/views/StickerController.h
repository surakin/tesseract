#pragma once
#include "tk/text_area.h" // tk::NavKey
#include "tk/weak_self.h"
#include "views/StickerEngine.h"
#include "views/StickerPopup.h"

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

// Drives the `/sticker <query>` inline result strip for a single composer.
// Search is local and synchronous (the shell's cached sticker packs), so there
// is no debounce or request correlation. The shell supplies a borrowed
// StickerPopup (mounted in its own surface) and the search/send hooks.
//
// Usage mirrors GifController: route text changes, popup-nav and submit here
// before the lower-priority popups. While the composer holds a `/sticker …`
// command, on_submit() always consumes Enter so the command is never sent as
// plain text.
class StickerController : public tk::EnableWeakSelf<StickerController>
{
public:
    struct Hooks
    {
        std::function<void()> show;
        std::function<void()> hide;
        std::function<void()> repaint;
        // Called instead of `repaint` when only the highlighted cell changed.
        std::function<void()> repaint_selection;
        // Stickers matching `query` for the active room, best matches first.
        std::function<std::vector<tesseract::ImagePackImage>(const std::string& query)> search;
        // Send the chosen sticker to the active room / thread.
        std::function<void(const tesseract::ImagePackImage&)> send;
        // Clear the "/sticker …" text from the composer after a send.
        std::function<void()> clear_composer;
    };

    StickerController(StickerPopup* popup, Hooks hooks);
    ~StickerController();

    // True if the composer text is a `/sticker …` command (the caller should
    // stop further on_changed processing). False (and hides the strip) otherwise.
    bool on_text_changed(const std::string& text);
    // Keyboard navigation while the strip is open; true if consumed.
    bool on_nav(tk::NavKey nk);
    // Enter: send the selected sticker. Returns true whenever the composer holds
    // a `/sticker …` command, so the text itself is never sent.
    bool on_submit();

    bool visible() const
    {
        return visible_;
    }
    void hide();

private:
    void repaint_selection_();
    void accept(const tesseract::ImagePackImage& sticker);
    void show_popup_();

    StickerPopup* popup_;
    Hooks hooks_;
    StickerEngine engine_;
    bool matched_ = false; // composer text is currently a /sticker command
    bool visible_ = false;
};

} // namespace tesseract::views
