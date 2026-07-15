#include "views/GifController.h"

#include <tesseract/client.h>

#include <atomic>
#include <utility>

namespace tesseract::views
{

GifController::GifController(tk::TextArea* text_area, GifPopup* popup,
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

void GifController::show_status(std::string message)
{
    if (!popup_)
    {
        return;
    }
    // A status message supersedes any in-flight search's results.
    request_seq_ = 0;
    popup_->set_status(std::move(message));
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

void GifController::run_search(std::string query)
{
    auto* client = hooks_.client ? hooks_.client() : nullptr;
    if (!client)
    {
        return;
    }
    const std::string key = hooks_.api_key ? hooks_.api_key() : std::string{};
    // No credential → the request would 404; surface it instead of failing
    // silently (this is exactly what made the empty-key case invisible).
    if (key.empty())
    {
        show_status("No GIF API key configured");
        return;
    }
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
        show_status("No GIFs found");
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
        show_status("GIF search failed");
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
    case tk::NavKey::Right:
    case tk::NavKey::Down:
    case tk::NavKey::Tab:
        popup_->move_selection(+1);
        break;
    case tk::NavKey::Left:
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

    // Copy the result out of the popup's storage BEFORE hide(): `gif` is a
    // reference into popup_->results_ (from selected() / the click path), and
    // hide() calls set_results({}) which frees that vector — copying afterward
    // would read freed memory and yield an all-empty GifResult.
    const tesseract::GifResult g = gif;

    // Use the user's typed search query as the event body/filename (what other
    // clients display) instead of a generic "gif.webp". Captured before hide()
    // since pending_query_ is unaffected, but copy for the async closure.
    const std::string body =
        pending_query_.empty() ? std::string("gif") : pending_query_;

    hide(); // dismiss the strip immediately; the upload runs in the background
    auto alive = alive_;
    auto post_to_ui = hooks_.post_to_ui;
    auto clear = hooks_.clear_composer;

    if (clear)
    {
        clear(); // clear the "/gif …" text right away for responsiveness
    }

    if (client)
    {
        client->send_gif_from_urls_async(
            0, room,
            g.image_url, g.image_mime, body,
            g.image_w, g.image_h,
            g.preview_url,
            g.preview_w, g.preview_h,
            /*reply*/ "", /*thread*/ "");
    }
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
