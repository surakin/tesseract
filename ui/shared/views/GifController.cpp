#include "views/GifController.h"

#include <tesseract/client.h>

#include <atomic>
#include <utility>

namespace tesseract::views
{

GifController::GifController(tk::NativeTextArea* text_area, GifPopup* popup,
                            Hooks hooks)
    : text_area_(text_area), popup_(popup), hooks_(std::move(hooks))
{
    if (popup_)
    {
        popup_->on_accepted = [this](const tesseract::GifResult& g)
        { accept(g); };
        popup_->on_dismissed = [this] { hide(); };
    }
}

GifController::~GifController()
{
    *alive_ = false;
}

bool GifController::on_text_changed(const std::string& text)
{
    auto query = engine_.match(text);
    if (!query)
    {
        hide();
        return false;
    }

    // Debounce: only the most recently scheduled generation actually fires the
    // search, so rapid typing collapses to one request per ~kDebounceMs.
    pending_query_ = std::move(*query);
    const std::uint64_t gen = ++debounce_seq_;
    auto alive = alive_;
    if (hooks_.post_delayed)
    {
        hooks_.post_delayed(kDebounceMs,
                            [this, gen, alive]
                            {
                                if (!*alive || gen != debounce_seq_)
                                {
                                    return;
                                }
                                run_search(pending_query_);
                            });
    }
    return true;
}

void GifController::run_search(std::string query)
{
    auto* client = hooks_.client ? hooks_.client() : nullptr;
    if (!client)
    {
        return;
    }
    const std::string key = hooks_.api_key ? hooks_.api_key() : std::string{};
    const std::string ck =
        hooks_.client_key ? hooks_.client_key() : std::string{};
    // Process-global id so the shell can fan on_gif_results() to every
    // controller and only the issuing one matches (per-controller counters
    // would collide across composers).
    static std::atomic<std::uint64_t> g_seq{1};
    const std::uint64_t id = g_seq.fetch_add(1, std::memory_order_relaxed);
    request_seq_ = id;
    client->gif_search(id, query, key, ck, kLimit);
}

void GifController::on_results(std::uint64_t request_id,
                              std::vector<tesseract::GifResult> results)
{
    // Drop results for a superseded search.
    if (request_id != request_seq_ || !popup_)
    {
        return;
    }
    if (results.empty())
    {
        hide();
        return;
    }
    popup_->set_results(std::move(results));
    visible_ = true;
    if (hooks_.show)
    {
        hooks_.show();
    }
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
}

void GifController::on_search_failed(std::uint64_t request_id,
                                    const std::string& /*message*/)
{
    if (request_id == request_seq_)
    {
        hide();
    }
}

bool GifController::on_nav(tk::NavKey nk)
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    switch (nk)
    {
    case tk::NavKey::Down:
    case tk::NavKey::Tab:
        popup_->move_selection(+1);
        break;
    case tk::NavKey::Up:
    case tk::NavKey::ShiftTab:
        popup_->move_selection(-1);
        break;
    case tk::NavKey::Escape:
        hide();
        return true;
    }
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
    return true;
}

bool GifController::on_submit()
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    const tesseract::GifResult* sel = popup_->selected();
    if (!sel)
    {
        return false;
    }
    accept(*sel);
    return true;
}

void GifController::accept(const tesseract::GifResult& gif)
{
    auto* client = hooks_.client ? hooks_.client() : nullptr;
    const std::string room = hooks_.room_id ? hooks_.room_id() : std::string{};
    if (!client || room.empty())
    {
        return;
    }
    hide(); // dismiss the strip immediately; the upload runs in the background

    const tesseract::GifResult g = gif;
    auto alive = alive_;
    auto post_to_ui = hooks_.post_to_ui;
    auto clear = hooks_.clear_composer;

    if (clear)
    {
        clear(); // clear the "/gif …" text right away for responsiveness
    }

    if (!hooks_.run_async)
    {
        return;
    }
    hooks_.run_async(
        [client, room, g, alive, post_to_ui]
        {
            std::vector<std::uint8_t> mp4 = client->fetch_url_bytes(g.mp4_url);
            if (mp4.empty())
            {
                return;
            }
            client->send_gif_video(room, mp4, "video/mp4", "gif.mp4", g.mp4_w,
                                   g.mp4_h, g.duration_ms,
                                   /*thumb*/ {}, /*thumb_mime*/ "",
                                   /*thumb_w*/ 0, /*thumb_h*/ 0,
                                   /*reply*/ "", /*thread*/ "");
            (void)alive;
            (void)post_to_ui;
        });
}

void GifController::hide()
{
    if (!visible_)
    {
        return;
    }
    visible_ = false;
    if (popup_)
    {
        popup_->set_results({});
    }
    if (hooks_.hide)
    {
        hooks_.hide();
    }
}

} // namespace tesseract::views
