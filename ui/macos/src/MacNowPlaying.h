#pragma once

namespace tesseract
{
class AccountManager;
}

// MacNowPlaying — macOS analog of GtkMprisPlayer/QtMprisPlayer (see
// MediaPlaybackHub.h). Wraps MPNowPlayingInfoCenter/MPRemoteCommandCenter so
// voice/audio playback surfaces in Control Center and responds to its
// controls, without any D-Bus dependency.
//
// Single instance per process, exactly like MacOSTrayIcon — constructed only
// after claim_mpris_owner() succeeds (see MainWindowController.mm).
class MacNowPlaying final
{
public:
    explicit MacNowPlaying(tesseract::AccountManager& account_manager);
    ~MacNowPlaying();

    // Always true: MPNowPlayingInfoCenter/MPRemoteCommandCenter are
    // process-local singletons with no registration handshake to fail
    // (unlike D-Bus name-owning) — kept only so the call site shares the
    // same `if (!x->is_available()) { ... }` shape as MacOSTrayIcon /
    // MacSpotlightSearch.
    bool is_available() const
    {
        return true;
    }

private:
    void refresh_();

    tesseract::AccountManager& account_manager_;
};
