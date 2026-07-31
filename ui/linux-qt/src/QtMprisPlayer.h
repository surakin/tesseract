#pragma once
#include <memory>

namespace tesseract
{
class AccountManager;
}

// org.mpris.MediaPlayer2 + org.mpris.MediaPlayer2.Player, exported over
// QDBus at the MPRIS-mandated object path "/org/mpris/MediaPlayer2", owning
// the well-known bus name "org.mpris.MediaPlayer2.Tesseract" (a genuinely
// new name — like QtKRunnerPlugin's "org.tesseract.qt", Qt apps don't
// auto-register any D-Bus name). Mirrors the QDBusAbstractAdaptor idiom
// established by LinuxUpConnectorQt.cpp / QtKRunnerPlugin.cpp.
//
// Purely a thin D-Bus-to-tesseract::MediaPlaybackHub adapter (see
// ui/shared/app/MediaPlaybackHub.h) — all playback state and control
// dispatch lives there; this class only marshals Qt/D-Bus types in and out
// and emits PropertiesChanged when the Hub's state changes.
//
// One instance per process (multi-window/multi-account) — see
// AccountManager::claim_mpris_owner.
class QtMprisPlayer
{
public:
    explicit QtMprisPlayer(tesseract::AccountManager& account_manager);
    ~QtMprisPlayer();

    bool is_available() const
    {
        return available_;
    }

    // Opaque; defined in the .cpp (holds the QObject host + adaptors, both
    // Qt/D-Bus types this header stays free of).
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};
