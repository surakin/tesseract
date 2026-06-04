#pragma once
#include "tk/host.h" // tk::NativeTextArea, tk::NavKey
#include "views/GifEngine.h"
#include "views/GifPopup.h"

#include <cstdint>
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

// Drives the `/gif <query>` inline result strip for a single composer. Owns the
// engine, debounce + request correlation, and the send dispatch; the shell
// supplies a borrowed NativeTextArea, a borrowed GifPopup (mounted in the
// shell's own surface), and surface/SDK plumbing hooks. Platform-agnostic so
// every composer (main window + each pop-out, on every shell) shares it.
//
// Usage from a shell:
//   - create a GifPopup, mount it in a surface, pass its pointer here
//   - text area on_changed:  call on_text_changed() first; if it returns true,
//                            skip the shortcode/mention/slash popups
//   - text area popup-nav:   route to on_nav()
//   - text area on_submit:   if on_submit() returns true, do NOT send the text
//   - on_gif_results / on_gif_search_failed callbacks: route to on_results() /
//                            on_search_failed()
class GifController
{
public:
    struct Hooks
    {
        // Position + show the (already-populated) popup above the composer.
        std::function<void()> show;
        std::function<void()> hide;
        std::function<void()> repaint;
        std::function<std::string()> room_id; // active room for the send
        std::function<tesseract::Client*()> client;
        // Worker / UI-thread plumbing (search + send run off the UI thread).
        std::function<void(std::function<void()>)> run_async;
        std::function<void(std::function<void()>)> post_to_ui;
        // Debounce timer: invoke the callback after `ms` on the UI thread.
        std::function<void(int ms, std::function<void()>)> post_delayed;
        // Provider credentials from app settings.
        std::function<std::string()> api_key;
        std::function<std::string()> client_key;
        // Clear the "/gif …" text from the composer after a successful send.
        std::function<void()> clear_composer;
    };

    GifController(tk::NativeTextArea* text_area, GifPopup* popup, Hooks hooks);
    ~GifController();

    // Returns true if the composer text is a `/gif <query>` command (a search
    // was scheduled / the strip is in play) — the caller should stop further
    // on_changed processing. Returns false (and hides the strip) otherwise.
    bool on_text_changed(const std::string& text);
    // Keyboard navigation while the strip is open; returns true if consumed.
    bool on_nav(tk::NavKey nk);
    // Enter pressed: send the selected GIF when the strip is open. Returns true
    // if a GIF was accepted (the caller must NOT send the composer text).
    bool on_submit();

    // SDK callback fan-in (routed by the shell). Stale request_ids are dropped.
    void on_results(std::uint64_t request_id,
                    std::vector<tesseract::GifResult> results);
    void on_search_failed(std::uint64_t request_id, const std::string& message);

    bool visible() const
    {
        return visible_;
    }
    void hide();

private:
    void run_search(std::string query);
    void accept(const tesseract::GifResult& gif);
    // Show a one-line message in the popup (empty key, no results, failed
    // search/send) so the strip never fails silently.
    void show_status(std::string message);

    tk::NativeTextArea* text_area_;
    GifPopup* popup_;
    Hooks hooks_;
    GifEngine engine_;

    std::string pending_query_;
    std::uint64_t request_seq_ = 0; // last-issued search id
    std::uint64_t debounce_seq_ = 0; // last-scheduled debounce generation
    bool visible_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    static constexpr int kDebounceMs = 300;
    static constexpr std::uint32_t kLimit = 24;
};

} // namespace tesseract::views
