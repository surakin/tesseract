#pragma once

#include "tesseract/client.h"
#include "tesseract/emoji.h"
#include "tesseract/event_handler.h"
#include "tesseract/notifier.h"
#include "tesseract/up_connector.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tesseract
{

/// Everything the host needs to drive a single Matrix account: the `Client`
/// (its own tokio runtime + matrix-sdk Client), the per-account
/// `IEventHandler` bridge (each shell concretes this with its own marshalling
/// type — `EventBridge` on Qt6, `EventHandler` on GTK4 / Win32, the
/// `EventBridgeImpl` on macOS), and cached identity bits the user-strip and
/// account-picker views read every frame.
///
/// `AccountSession` is a pure value type — no methods beyond the implicit
/// move/destructor. The host owns a `std::vector<std::unique_ptr<AccountSession>>`
/// and rebinds shared UI surfaces to the active entry through the
/// `MainWindow::switch_active_account` chokepoint.
struct AccountSession
{
    // bridge must be declared BEFORE client so that it is destroyed LAST
    // (C++ destroys fields in reverse declaration order). The tokio runtime
    // inside ClientFfi calls rt.drop() last, which blocks until all spawned
    // watcher tasks finish. Those tasks hold Arc<Mutex<SendHandler>> and may
    // invoke C++ callbacks — the IEventHandler must still be alive for the
    // entire duration of rt.drop(). Declaring client second guarantees that.
    std::unique_ptr<IEventHandler> bridge;
    std::unique_ptr<Client> client;
    // notifier is declared after client so it is destroyed first (before
    // client teardown), stopping incoming notifications before the SDK tears
    // down. bridge is still declared first so it outlives both.
    std::unique_ptr<INotifier> notifier;
    // up_connector registers with the UnifiedPush distributor via D-Bus.
    // Null on Win32 and macOS. Declared after notifier (destroyed before it)
    // so the D-Bus listener is torn down before notifications stop.
    std::unique_ptr<IUpConnector> up_connector;

    /// Canonical Matrix ID, e.g. `@alice:example.org`. Used as the key in
    /// `accounts.json` and as the parent of `SessionStore::account_dir`.
    std::string user_id;

    /// Display name resolved at restore/login time and refreshed when the
    /// server-side profile changes. Empty when the account has none set.
    std::string display_name;

    /// `mxc://…` URI of the account's avatar, or empty when unset. The shell's
    /// avatar cache resolves this to a `tk::Image*` on demand.
    std::string avatar_url;

    /// Room ID the user last had focused for this account, restored from the
    /// `im.gnomos.tesseract` account-data event on session restore.
    std::string last_room;

    /// All open tab room IDs in visual order at last save, restored from the
    /// `im.gnomos.tesseract` account-data event. Includes last_room.
    std::vector<std::string> open_rooms;

    /// Most-recently-visited room IDs (front = most recent), restored from
    /// the `im.gnomos.tesseract` account-data event. Holds this account's MRU
    /// while another account is active; ShellBase::recent_room_ids_ is the
    /// live copy for the active account.
    std::vector<std::string> recent_rooms;

    /// Room-directory servers the user has browsed, most recent first (at
    /// most `kMaxDirectoryServers`), restored from the `im.gnomos.tesseract`
    /// account-data event. The room browser's server picker lists these.
    std::vector<std::string> directory_servers;

    /// Space room IDs pinned to the space picker sidebar (tab order),
    /// restored from the `im.gnomos.tesseract` account-data event. Pinned
    /// spaces are hidden from the room list.
    std::vector<std::string> pinned_spaces;

    /// Room IDs the user has locally marked "not actually bridged", restored
    /// from the `im.gnomos.tesseract` account-data event. Overrides
    /// `RoomInfo::is_bridged` for that room — see
    /// `ShellBase::room_effectively_bridged_()`.
    std::vector<std::string> bridge_not_bridged_overrides;

    /// Default emoji skin tone, restored from (and saved to) the
    /// `im.gnomos.tesseract` account-data event so it follows the account.
    emoji::SkinTone emoji_skin_tone = emoji::SkinTone::None;
    /// Last known raw content of the `im.gnomos.tesseract` event (restored,
    /// synced, or our own latest save). Saves overlay onto it so keys this
    /// build doesn't write survive, without a blocking store read.
    std::string prefs_json = "{}";

    /// When the tone was last changed locally (default = not recently). For
    /// a short window afterwards a differing synced value may be a stale
    /// echo of an earlier save, so it is parked in `emoji_skin_tone_deferred`
    /// instead of applied; our own echo clears it, otherwise it is adopted
    /// once the window has passed (another device wrote after us, or our
    /// save never landed). See apply_synced_emoji_skin_tone() below.
    std::chrono::steady_clock::time_point emoji_skin_tone_set_at{};
    std::optional<emoji::SkinTone> emoji_skin_tone_deferred;

    /// True once `client->start_sync(bridge.get())` has been called for this
    /// session — guards against double-starts and lets the destructor know to
    /// call `stop_sync` for clean shutdown.
    bool sync_started = false;

    /// UI state: true when the last on_verification_state_changed callback
    /// reported is_verified=false for this account. Saved by EventHandlerBase
    /// on every state change (including the initial snapshot) so that
    /// switch_active_account can restore the correct banner state.
    bool unverified = false;
};

/// How long after a local tone change a differing synced tone is treated as
/// a possible stale echo of an earlier save rather than applied.
inline constexpr std::chrono::seconds kSkinToneEchoWindow{15};

/// Local tone change: apply it and open the echo window.
inline void set_local_emoji_skin_tone(AccountSession& a, emoji::SkinTone tone,
                                      std::chrono::steady_clock::time_point now)
{
    a.emoji_skin_tone = tone;
    a.emoji_skin_tone_set_at = now;
    a.emoji_skin_tone_deferred.reset();
}

/// A tone arrived via sync (another device, or an echo of one of our saves).
/// Our own value closes the echo window; inside the window a differing value
/// is parked, outside it the value is applied.
inline void apply_synced_emoji_skin_tone(AccountSession& a, emoji::SkinTone synced,
                                         std::chrono::steady_clock::time_point now)
{
    const bool in_window = a.emoji_skin_tone_set_at != std::chrono::steady_clock::time_point{} &&
                           now - a.emoji_skin_tone_set_at < kSkinToneEchoWindow;
    if (synced == a.emoji_skin_tone)
    {
        a.emoji_skin_tone_deferred.reset();
        a.emoji_skin_tone_set_at = {};
    }
    else if (in_window)
    {
        a.emoji_skin_tone_deferred = synced;
    }
    else
    {
        a.emoji_skin_tone = synced;
        a.emoji_skin_tone_deferred.reset();
        a.emoji_skin_tone_set_at = {};
    }
}

/// Adopt a parked synced tone once the echo window has passed without our
/// own echo arriving (another device wrote after us, or our save failed).
inline void settle_emoji_skin_tone(AccountSession& a, std::chrono::steady_clock::time_point now)
{
    if (!a.emoji_skin_tone_deferred || now - a.emoji_skin_tone_set_at < kSkinToneEchoWindow)
    {
        return;
    }
    a.emoji_skin_tone = *a.emoji_skin_tone_deferred;
    a.emoji_skin_tone_deferred.reset();
    a.emoji_skin_tone_set_at = {};
}

} // namespace tesseract
