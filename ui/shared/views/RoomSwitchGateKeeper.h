#pragma once

// RoomSwitchGateKeeper — the room-switch "display gate" state machine,
// extracted from MessageListView. On a room switch the message list is held
// invisible until the visible rows' height-affecting media (images, stickers,
// video thumbnails, URL-preview cards) have loaded + measured, OR a ~400ms
// timeout elapses — so the user never sees the list reflow as async content
// arrives. A "focused" mode jumps to a specific event on reveal instead of
// scrolling to the bottom.
//
// MessageListView holds one of these by value. It still owns `messages_` and
// the ListView `visible_range()`, so the first-paint dependency scan is driven
// by MessageListView calling evaluate() with a callback that enumerates the
// visible band; the keeper applies the per-Kind "is this row's media loaded?"
// check (fed the row + the image/preview providers) to build the pending set.
//
// The per-Kind dep check and the pending-key derivation live here together so
// the gate logic stays in one place. Scroll-on-reveal, the timeout scheduler,
// and repaint requests are injected as std::function wiring so timing and
// behavior are preserved exactly.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace tk
{
class Image;
} // namespace tk

namespace tesseract::views
{

struct MessageRowData;
struct UrlPreviewData;

class RoomSwitchGateKeeper
{
public:
    // Default fallback timeout: a slow / offline network can never hold the
    // list invisible forever.
    static constexpr int kTimeoutMs = 400;

    // --- wiring (forwarded from MessageListView) ---
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc_or_url)>;
    using PreviewProvider =
        std::function<const UrlPreviewData*(const std::string& url)>;

    void set_providers(ImageProvider image, PreviewProvider preview)
    {
        image_provider_  = std::move(image);
        preview_provider_ = std::move(preview);
    }
    // Re-pin scroll on reveal: focus-mode jumps to the event, else scrolls
    // to the bottom. Heights are already final by the time these run.
    void set_scroll_callbacks(std::function<void(const std::string&)> to_event,
                              std::function<void()> to_bottom)
    {
        scroll_to_event_ = std::move(to_event);
        scroll_to_bottom_ = std::move(to_bottom);
    }
    // Schedule the timeout fallback: (delay_ms, callback).
    void set_post_delayed(std::function<void(int, std::function<void()>)> f)
    {
        post_delayed_ = std::move(f);
    }
    void set_repaint(std::function<void()> f) { request_repaint_ = std::move(f); }
    // Liveness guard for the deferred timeout closure (the view's alive_ flag).
    void set_alive(std::weak_ptr<bool> alive) { alive_ = std::move(alive); }

    // --- arming ---
    // Supersede any prior gate (rapid re-switch / same-room reset) and arm a
    // fresh gate with the timeout fallback. `epoch` neutralises an outstanding
    // timeout closure on a later re-switch. Returns the bumped epoch.
    std::uint64_t begin_room_switch();
    // Tear down any gate without revealing (room-switch=false or empty list).
    void clear() { gate_.reset(); }
    // Switch a not-yet-evaluated gate into jump-to-event mode.
    void set_focus_event(const std::string& focus_event_id);

    // --- per-paint evaluation ---
    // True while a gate exists (armed but not yet revealed). Input is swallowed
    // and the list is held invisible while this is true.
    bool active() const { return gate_.has_value(); }
    // True once the visible band has been scanned for deps.
    bool evaluated() const { return gate_ && gate_->evaluated; }
    // True while the pending dependency set still holds the list invisible.
    bool blocking() const { return gate_ && !gate_->pending.empty(); }

    // First-paint scan: fill the pending set from the visible band. `scan`
    // enumerates the currently-visible rows, invoking the supplied per-row
    // visitor for each. Idempotent guard via `evaluated`.
    void evaluate(const std::function<void(
                      const std::function<void(const MessageRowData&)>&)>& scan);

    // Drop a resolved key (image token or preview URL); request a repaint to
    // reveal once the pending set empties. No-op when no gate.
    void notify_loaded(const std::string& key);

    // If the gate's deps are resolved, clear it and re-pin scroll. Returns
    // whether a reveal happened. Call after evaluate() when !blocking().
    bool try_reveal();

    // Has every height-affecting dependency of row `m` already resolved?
    bool dep_satisfied(const MessageRowData& m) const;

private:
    struct Gate
    {
        std::uint64_t epoch = 0;
        bool evaluated = false;                  // visible band scanned
        std::unordered_set<std::string> pending; // unmet media/url keys
        bool focused = false;                    // jump-to-event mode
        std::string focus_event_id;
    };
    std::optional<Gate> gate_;
    std::uint64_t epoch_ = 0;

    ImageProvider   image_provider_;
    PreviewProvider preview_provider_;
    std::function<void(const std::string&)> scroll_to_event_;
    std::function<void()>                    scroll_to_bottom_;
    std::function<void(int, std::function<void()>)> post_delayed_;
    std::function<void()> request_repaint_;
    std::weak_ptr<bool> alive_;
};

} // namespace tesseract::views
