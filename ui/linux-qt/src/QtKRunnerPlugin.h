#pragma once
#include <memory>

namespace tesseract
{
class AccountManager;
}

// org.kde.krunner1 plugin, exported over QDBus on this app's own explicitly
// owned bus name ("org.tesseract.qt" — unlike the GTK4 shell's GApplication,
// a Qt app doesn't auto-register any D-Bus name, so this owns one itself the
// same way LinuxUpConnectorQt's UpSharedBusQt owns "im.gnomos.Tesseract").
// Purely a thin D-Bus-to-SearchBackend adapter: all query/ranking/activation
// logic lives in tesseract::SearchBackend (ui/shared/app/SearchBackend.h).
//
// One instance per process (multi-window/multi-account, like the tray) —
// see AccountManager::claim_search_provider_owner (shared with the GTK4
// search provider's ownership guard; only one shell type is ever built into
// a given process).
class QtKRunnerPlugin
{
public:
    explicit QtKRunnerPlugin(tesseract::AccountManager& account_manager);
    ~QtKRunnerPlugin();

    bool is_available() const
    {
        return available_;
    }

    // Opaque; defined in the .cpp (holds the QObject host + adaptor, both
    // Qt/D-Bus types this header stays free of).
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};
