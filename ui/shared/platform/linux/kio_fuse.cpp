#include "kio_fuse.h"

#include <array>
#include <cctype>

#include <gio/gio.h>

namespace tesseract::kio_fuse
{

namespace
{

// Schemes that name a remote file system. Deliberately a whitelist: a link
// dragged out of a browser (http/https) must not become an attachment.
//
// Known tradeoff: a text/uri-list drag looks the same whether it comes from
// Dolphin or from a web page, so dropping a page-supplied smb:// or sftp://
// URL on the composer makes kio-fuse connect to that host (SMB can expose an
// NTLM hash). It takes a deliberate drop and is accepted rather than gated
// behind a confirmation dialog.
constexpr std::array<std::string_view, 16> kRemoteSchemes = {
    "sftp", "fish", "smb", "ftp",    "ftps", "webdav",     "webdavs", "dav",
    "davs", "nfs",  "mtp", "gdrive", "afc",  "kdeconnect", "gphoto2", "camera",
};

constexpr int kMountTimeoutMs = 60 * 1000; // covers the remote connect

} // namespace

bool is_remote_uri(std::string_view uri)
{
    const auto colon = uri.find("://");
    if (colon == std::string_view::npos || colon == 0)
    {
        return false;
    }
    std::string scheme;
    for (const char c : uri.substr(0, colon))
    {
        scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto s : kRemoteSchemes)
    {
        if (scheme == s)
        {
            return true;
        }
    }
    return false;
}

Result mount_url(const std::string& uri)
{
    Result out;
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus)
    {
        out.status = Status::Unavailable;
        g_clear_error(&err);
        return out;
    }

    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.kde.KIOFuse", "/org/kde/KIOFuse", "org.kde.KIOFuse.VFS", "mountUrl",
        g_variant_new("(s)", uri.c_str()), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
        kMountTimeoutMs, nullptr, &err);
    if (!reply)
    {
        const bool missing =
            err && (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                    g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
                    g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND));
        out.status = missing ? Status::Unavailable : Status::Failed;
        g_clear_error(&err);
        g_object_unref(bus);
        return out;
    }

    const gchar* path = nullptr;
    g_variant_get(reply, "(&s)", &path);
    if (path && *path)
    {
        out.status = Status::Ok;
        out.path = path;
    }
    g_variant_unref(reply);
    g_object_unref(bus);
    return out;
}

} // namespace tesseract::kio_fuse
