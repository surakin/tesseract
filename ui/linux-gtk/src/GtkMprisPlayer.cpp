#include "GtkMprisPlayer.h"

#include "app/AccountManager.h"
#include "app/MediaPlaybackHub.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstdint>
#include <string>

namespace
{

// Only the members actually implemented — no LoopStatus/Shuffle/playlist
// (Next/Previous stay unimplemented; CanGoNext/CanGoPrevious report false),
// matching the "rooms/messages aren't a linear playlist" framing.
const char* kMprisXml = R"XML(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
    <method name="Raise"/>
    <method name="Quit"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Rate" type="d" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="read"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
    <method name="Play"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Seek"><arg name="Offset" type="x" direction="in"/></method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/>
      <arg name="Position" type="x" direction="in"/>
    </method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
  </interface>
</node>)XML";

// MPRIS trackids must be valid D-Bus object paths ([A-Za-z0-9_/] only) —
// Matrix event ids ("$abc:server.org") are not, so sanitize.
std::string sanitize_track_id(const std::string& event_id)
{
    std::string out = "/org/tesseract/gtk/Track";
    for (char c : event_id)
    {
        out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return out;
}

GVariant* build_metadata(const tesseract::NowPlaying& np)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (np.kind == tesseract::NowPlaying::Kind::None)
    {
        return g_variant_builder_end(&b); // empty dict — nothing loaded
    }
    const std::string track_id = sanitize_track_id(np.event_id);
    g_variant_builder_add(&b, "{sv}", "mpris:trackid",
                          g_variant_new_object_path(track_id.c_str()));
    g_variant_builder_add(
        &b, "{sv}", "mpris:length",
        g_variant_new_int64(static_cast<gint64>(np.duration_ms) * 1000));
    g_variant_builder_add(&b, "{sv}", "xesam:title",
                          g_variant_new_string(np.title.c_str()));
    if (!np.artist.empty())
    {
        const char* artists[] = {np.artist.c_str(), nullptr};
        g_variant_builder_add(&b, "{sv}", "xesam:artist",
                              g_variant_new_strv(artists, -1));
    }
    return g_variant_builder_end(&b);
}

} // namespace

struct GtkMprisPlayer::Impl
{
    tesseract::AccountManager* account_manager = nullptr;

    GDBusConnection* conn = nullptr;
    guint root_reg_id = 0;
    guint player_reg_id = 0;
    guint own_name_id = 0;

    ~Impl()
    {
        if (conn && root_reg_id)
            g_dbus_connection_unregister_object(conn, root_reg_id);
        if (conn && player_reg_id)
            g_dbus_connection_unregister_object(conn, player_reg_id);
        if (own_name_id)
            g_bus_unown_name(own_name_id);
        if (conn)
            g_object_unref(conn);
    }

    void emit_properties_changed()
    {
        if (!conn)
            return;
        auto& hub = account_manager->media_playback_hub();
        const auto& np = hub.current();
        const bool loaded = np.kind != tesseract::NowPlaying::Kind::None;
        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(
            &changed, "{sv}", "PlaybackStatus",
            g_variant_new_string(np.kind == tesseract::NowPlaying::Kind::None
                                      ? "Stopped"
                                  : np.is_playing ? "Playing"
                                                  : "Paused"));
        g_variant_builder_add(&changed, "{sv}", "Metadata",
                              build_metadata(np));
        // CanPlay/CanPause/CanSeek flip false->true (and back) alongside
        // PlaybackStatus/Metadata — clients like Plasma's media widget cache
        // the initial GetAll snapshot and only re-read on PropertiesChanged,
        // so omitting these left the controls permanently disabled after the
        // very first (nothing-loaded) snapshot.
        g_variant_builder_add(&changed, "{sv}", "CanPlay",
                              g_variant_new_boolean(loaded));
        g_variant_builder_add(&changed, "{sv}", "CanPause",
                              g_variant_new_boolean(loaded));
        g_variant_builder_add(&changed, "{sv}", "CanSeek",
                              g_variant_new_boolean(loaded));
        GVariantBuilder invalidated;
        g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
        g_dbus_connection_emit_signal(
            conn, nullptr, "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player",
                         &changed, &invalidated),
            nullptr);
    }
};

namespace
{

void root_method(GDBusConnection*, const char*, const char*, const char*,
                 const char* method, GVariant*,
                 GDBusMethodInvocation* invocation, gpointer user_data)
{
    auto* impl = static_cast<GtkMprisPlayer::Impl*>(user_data);
    if (g_strcmp0(method, "Raise") == 0)
    {
        // No window to raise from a headless MPRIS identity alone — the
        // tray/search-provider already provide "bring Tesseract to front"
        // affordances; Raise is a no-op here (CanRaise reports false below).
    }
    else if (g_strcmp0(method, "Quit") == 0)
    {
        // Deliberately not wired to app quit — MPRIS clients calling Quit on
        // a chat client (rather than a dedicated media player) would be
        // surprising; CanQuit reports false below so well-behaved clients
        // won't offer it.
        (void)impl;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* root_get_property(GDBusConnection*, const char*, const char*,
                            const char*, const char* prop, GError**,
                            gpointer)
{
    if (g_strcmp0(prop, "CanQuit") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "CanRaise") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "HasTrackList") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "Identity") == 0)
        return g_variant_new_string("Tesseract");
    if (g_strcmp0(prop, "DesktopEntry") == 0)
        return g_variant_new_string("tesseract-matrix-gtk");
    if (g_strcmp0(prop, "SupportedUriSchemes") == 0)
        return g_variant_new_strv(nullptr, 0);
    if (g_strcmp0(prop, "SupportedMimeTypes") == 0)
        return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

void player_method(GDBusConnection*, const char*, const char*, const char*,
                   const char* method, GVariant* params,
                   GDBusMethodInvocation* invocation, gpointer user_data)
{
    auto* impl = static_cast<GtkMprisPlayer::Impl*>(user_data);
    auto& hub = impl->account_manager->media_playback_hub();
    if (g_strcmp0(method, "Play") == 0)
    {
        hub.play();
    }
    else if (g_strcmp0(method, "Pause") == 0)
    {
        hub.pause();
    }
    else if (g_strcmp0(method, "PlayPause") == 0)
    {
        hub.play_pause();
    }
    else if (g_strcmp0(method, "Stop") == 0)
    {
        hub.stop();
    }
    else if (g_strcmp0(method, "Seek") == 0)
    {
        gint64 offset_us = 0;
        g_variant_get(params, "(x)", &offset_us);
        hub.seek(static_cast<std::int64_t>(offset_us / 1000));
    }
    else if (g_strcmp0(method, "SetPosition") == 0)
    {
        // v1: only relative Seek is wired through MediaPlaybackHub; an
        // absolute SetPosition would need the current position round-tripped
        // from TimelineMediaController, which isn't exposed here. Treat as a
        // no-op rather than silently doing the wrong seek.
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* player_get_property(GDBusConnection*, const char*, const char*,
                             const char*, const char* prop, GError**,
                             gpointer user_data)
{
    auto* impl = static_cast<GtkMprisPlayer::Impl*>(user_data);
    const auto& np = impl->account_manager->media_playback_hub().current();
    const bool loaded = np.kind != tesseract::NowPlaying::Kind::None;

    if (g_strcmp0(prop, "PlaybackStatus") == 0)
        return g_variant_new_string(!loaded              ? "Stopped"
                                    : np.is_playing ? "Playing"
                                                    : "Paused");
    if (g_strcmp0(prop, "Rate") == 0)
        return g_variant_new_double(1.0);
    if (g_strcmp0(prop, "Metadata") == 0)
        return build_metadata(np);
    if (g_strcmp0(prop, "Volume") == 0)
        return g_variant_new_double(1.0);
    if (g_strcmp0(prop, "Position") == 0)
        return g_variant_new_int64(static_cast<gint64>(np.position_ms) * 1000);
    if (g_strcmp0(prop, "MinimumRate") == 0 ||
        g_strcmp0(prop, "MaximumRate") == 0)
        return g_variant_new_double(1.0);
    if (g_strcmp0(prop, "CanGoNext") == 0 || g_strcmp0(prop, "CanGoPrevious") == 0)
        return g_variant_new_boolean(FALSE); // no playlist concept
    if (g_strcmp0(prop, "CanPlay") == 0 || g_strcmp0(prop, "CanPause") == 0 ||
        g_strcmp0(prop, "CanSeek") == 0)
        return g_variant_new_boolean(loaded ? TRUE : FALSE);
    if (g_strcmp0(prop, "CanControl") == 0)
        return g_variant_new_boolean(TRUE);
    return nullptr;
}

} // namespace

GtkMprisPlayer::GtkMprisPlayer(tesseract::AccountManager& account_manager)
    : impl_(std::make_unique<Impl>())
{
    impl_->account_manager = &account_manager;

    GError* err = nullptr;
    impl_->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!impl_->conn)
    {
        g_clear_error(&err);
        return;
    }

    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kMprisXml, &err);
    if (!node)
    {
        g_clear_error(&err);
        return;
    }

    GDBusInterfaceInfo* root_iface =
        g_dbus_node_info_lookup_interface(node, "org.mpris.MediaPlayer2");
    GDBusInterfaceInfo* player_iface = g_dbus_node_info_lookup_interface(
        node, "org.mpris.MediaPlayer2.Player");

    static const GDBusInterfaceVTable kRootVtable = {root_method,
                                                     root_get_property,
                                                     nullptr, {nullptr}};
    static const GDBusInterfaceVTable kPlayerVtable = {player_method,
                                                       player_get_property,
                                                       nullptr, {nullptr}};

    if (root_iface)
    {
        impl_->root_reg_id = g_dbus_connection_register_object(
            impl_->conn, "/org/mpris/MediaPlayer2", root_iface, &kRootVtable,
            impl_.get(), nullptr, &err);
    }
    if (impl_->root_reg_id && player_iface)
    {
        impl_->player_reg_id = g_dbus_connection_register_object(
            impl_->conn, "/org/mpris/MediaPlayer2", player_iface,
            &kPlayerVtable, impl_.get(), nullptr, &err);
    }
    g_dbus_node_info_unref(node);
    if (!impl_->root_reg_id || !impl_->player_reg_id)
    {
        g_clear_error(&err);
        return;
    }

    impl_->own_name_id = g_bus_own_name_on_connection(
        impl_->conn, "org.mpris.MediaPlayer2.Tesseract",
        G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);

    account_manager.media_playback_hub().on_changed = [impl = impl_.get()]
    { impl->emit_properties_changed(); };

    available_ = true;
}

GtkMprisPlayer::~GtkMprisPlayer()
{
    if (impl_ && impl_->account_manager)
    {
        // Drop the Hub's callback into this (about-to-be-destroyed) Impl —
        // otherwise a later report()/report_stopped() from a surviving
        // window would call into freed memory.
        impl_->account_manager->media_playback_hub().on_changed = nullptr;
    }
}
