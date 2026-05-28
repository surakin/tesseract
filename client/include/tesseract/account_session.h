#pragma once

#include "tesseract/client.h"
#include "tesseract/event_handler.h"
#include "tesseract/notifier.h"
#include "tesseract/up_connector.h"

#include <memory>
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

    /// True once `client->start_sync(bridge.get())` has been called for this
    /// session — guards against double-starts and lets the destructor know to
    /// call `stop_sync` for clean shutdown.
    bool sync_started = false;

    /// UI state: true after the user dismisses the recovery banner for this
    /// account. Persists across account switches within the same app session.
    bool recovery_banner_dismissed = false;

    /// UI state: true when the user explicitly clicked "Use recovery key" for
    /// this account and the RecoveryBanner is awaiting key entry.
    bool recovery_key_chosen = false;

    /// UI state: true when the last on_verification_state_changed callback
    /// reported is_verified=false for this account. Saved by EventHandlerBase
    /// on every state change (including the initial snapshot) so that
    /// switch_active_account can restore the correct banner state.
    bool unverified = false;

    /// UI state: true after the user dismisses the verification banner for this
    /// account. Persists across account switches within the same app session.
    bool verification_banner_dismissed = false;
};

} // namespace tesseract
