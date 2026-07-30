#pragma once

// RoomSwitchGateKeeper — the room-switch "display gate" state machine,
// extracted from MessageListView. On a room switch the message list is held
// invisible until the visible rows' content — height-affecting media
// (images, stickers, video thumbnails, URL-preview cards), sender/membership
// avatars, custom-emoji glyphs, and quoted-reply resolution — has loaded, OR
// a bounded timeout elapses — so the user never sees the list reflow or pop
// in as async content arrives. A "focused" mode jumps to a specific event on
// reveal instead of scrolling to the bottom.
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
#include <vector>

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
    // Fallback timeout so a slow / offline network can never hold the list
    // invisible forever. Blocks on URL previews, avatars, custom-emoji
    // glyphs, and quoted-reply resolution — all homeserver round-trips that
    // routinely exceed ~150ms, and all resolve concurrently (not
    // multiplicatively) since dep_satisfied checks are independent per row.
    // Keep the timeout generous so not-yet-loaded content is waited for
    // (avoiding the reveal-then-pop-in the gate exists to prevent) rather
    // than revealed early.
    static constexpr int kTimeoutMs = 600;

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
    // Pure-peek mxc -> tk::Image* lookup for sender/membership-target
    // avatars. Deliberately separate from image_provider_, which fetches on
    // a cache miss as an intentional side effect for media rows — reusing it
    // for an avatar mxc would wrongly kick off a full-resolution
    // ensure_media_image_ fetch instead of the correctly-sized
    // ensure_user_avatar_/ensure_room_avatar_ path.
    void set_avatar_provider(ImageProvider avatar)
    {
        avatar_provider_ = std::move(avatar);
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
    // Re-arm for a same-room timeline reset landing while the gate from the
    // ORIGINAL switch is still pending (e.g. subscribe_room's initial
    // snapshot followed by paginate_back_with_status's refill). No-op if no
    // gate is currently active — an ordinary backfill on an already-revealed
    // room must not start gating (that would freeze scroll-up pagination
    // behind an invisible list). Preserves focused/focus_event_id across the
    // re-arm, since the caller only calls set_focus_event() on a genuine
    // room_switch==true reset.
    void reset_within_switch();
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

    // Has every dependency of row `m` (media/preview/avatar/reply/emoji)
    // already resolved?
    bool dep_satisfied(const MessageRowData& m) const;

private:
    // Returns every not-yet-resolved dependency key for row `m` — zero, one,
    // or several (e.g. a reply row can simultaneously be waiting on its own
    // avatar AND the quoted message's resolution). Single source of truth
    // for both dep_satisfied() and evaluate()'s pending-set fill.
    std::vector<std::string> pending_keys_for(const MessageRowData& m) const;

    struct Gate
    {
        std::uint64_t epoch = 0;
        bool evaluated = false;                  // visible band scanned
        std::unordered_set<std::string> pending; // unmet dependency keys
        bool focused = false;                    // jump-to-event mode
        std::string focus_event_id;
    };
    std::optional<Gate> gate_;
    std::uint64_t epoch_ = 0;

    ImageProvider   image_provider_;
    ImageProvider   avatar_provider_;
    PreviewProvider preview_provider_;
    std::function<void(const std::string&)> scroll_to_event_;
    std::function<void()>                    scroll_to_bottom_;
    std::function<void(int, std::function<void()>)> post_delayed_;
    std::function<void()> request_repaint_;
    std::weak_ptr<bool> alive_;
};

} // namespace tesseract::views
