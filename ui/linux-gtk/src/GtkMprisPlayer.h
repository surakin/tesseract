#pragma once
#include <memory>

namespace tesseract
{
class AccountManager;
}

// org.mpris.MediaPlayer2 + org.mpris.MediaPlayer2.Player, exported over GDBus
// at the MPRIS-mandated object path "/org/mpris/MediaPlayer2", owning the
// well-known bus name "org.mpris.MediaPlayer2.Tesseract" (a genuinely new
// name — unlike the search provider, MPRIS can't reuse GApplication's own
// "org.tesseract.gtk"). Mirrors GtkSniTrayIcon.cpp's GDBus export idiom.
//
// Purely a thin D-Bus-to-tesseract::MediaPlaybackHub adapter (see
// ui/shared/app/MediaPlaybackHub.h) — all playback state and control
// dispatch lives there; this class only marshals GVariant in and out and
// emits PropertiesChanged when the Hub's state changes.
//
// One instance per process (multi-window/multi-account, like the tray) —
// see AccountManager::claim_mpris_owner.
class GtkMprisPlayer final
{
public:
    explicit GtkMprisPlayer(tesseract::AccountManager& account_manager);
    ~GtkMprisPlayer();

    bool is_available() const
    {
        return available_;
    }

    // Opaque; defined in the .cpp. Public so the file-local GDBus vtable
    // callbacks (which receive it as user_data) can name the type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};
