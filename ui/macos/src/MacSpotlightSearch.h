#pragma once

#include <string>
#include <unordered_set>

namespace tesseract
{
class AccountManager;
}

// MacSpotlightSearch — macOS analog of GtkSearchProviderGtk (GNOME Shell
// search provider) / QtKRunnerPlugin (KRunner), see SearchBackend.h. There
// is no macOS mechanism for the OS to synchronously query us as the user
// types (unlike D-Bus); Spotlight instead works from a push-indexed
// CSSearchableIndex, so this class periodically re-pushes SearchBackend's
// current rooms/contacts rather than answering live queries.
//
// Single instance per process, exactly like MacOSTrayIcon/MacNowPlaying —
// constructed only after claim_search_provider_owner() succeeds (see
// MainWindowController.mm).
class MacSpotlightSearch final
{
public:
    explicit MacSpotlightSearch(tesseract::AccountManager& account_manager);
    ~MacSpotlightSearch();

    // Always true — see MacNowPlaying::is_available() for the same rationale
    // (CSSearchableIndex has no registration handshake to fail).
    bool is_available() const
    {
        return true;
    }

    // Re-pulls every room/contact from search_backend().query("") into
    // Spotlight, deleting anything indexed by a previous call that's no
    // longer present. Cheap enough to call after any room-list/roster
    // change; callers should still debounce rapid-fire changes (see
    // MainWindowController's -_scheduleSpotlightReindex).
    void reindex();

private:
    tesseract::AccountManager& account_manager_;
    std::unordered_set<std::string> indexed_ids_; // previous reindex()'s id set
};
