#pragma once
#include <memory>

namespace tesseract
{
class AccountManager;
}

// org.gnome.Shell.SearchProvider2, exported over GDBus on the GApplication's
// own already-owned bus name (see main.cpp: gtk_application_new("org.tesseract.gtk", ...)) —
// no separate g_bus_own_name call needed, unlike GtkSniTrayIcon's SNI name.
// Purely a thin D-Bus-to-SearchBackend adapter: all query/ranking/activation
// logic lives in tesseract::SearchBackend (ui/shared/app/SearchBackend.h);
// this class only marshals GVariant in and out.
//
// One instance per process (multi-window/multi-account, like the tray) —
// see AccountManager::claim_search_provider_owner.
class GtkSearchProviderGtk final
{
public:
    explicit GtkSearchProviderGtk(tesseract::AccountManager& account_manager);
    ~GtkSearchProviderGtk();

    bool is_available() const
    {
        return available_;
    }

    // Opaque; defined in the .cpp. Public so the file-local GDBus vtable
    // callback (which receives it as user_data) can name the type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool available_ = false;
};
