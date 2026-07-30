#include "LinuxNotifier.h"
#include <string>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "../../shared/linux_portal.h"
#include "tk/i18n.h"

LinuxNotifierGtk::LinuxNotifierGtk(
    std::function<void(std::string, std::string)> on_activate,
    std::function<void(std::string, std::string, std::string)> on_reply)
    : on_activate_(std::move(on_activate)), on_reply_(std::move(on_reply))
{
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus_)
    {
        return;
    }

    action_sub_ = g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "ActionInvoked", "/org/freedesktop/Notifications", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_action_invoked_cb, this, nullptr);

    closed_sub_ = g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "NotificationClosed", "/org/freedesktop/Notifications", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notification_closed_cb, this, nullptr);

    // KDE/GNOME de-facto extension (not in the base freedesktop.org spec):
    // fires just before ActionInvoked with a Wayland xdg_activation_v1
    // token, provided notify() sends a "desktop-entry" hint the daemon can
    // resolve to this app. Daemons that don't implement it simply never
    // fire this, so subscribing unconditionally is safe.
    activation_token_sub_ = g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "ActivationToken", "/org/freedesktop/Notifications", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_activation_token_cb, this, nullptr);

    // KDE Plasma-only extension: fired when the user submits text via the
    // inline-reply action (see ui/shared/linux_notification_reply.h). Other
    // daemons never emit this signal, so this subscription is simply dormant
    // there — no capability check needed before subscribing.
    replied_sub_ = g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "NotificationReplied", "/org/freedesktop/Notifications", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, on_notification_replied_cb, this, nullptr);

    // XDG Desktop Portal notification signal — provides an xdg_activation_v1
    // token on Wayland, enabling reliable window focus after notification click.
    portal_action_sub_ = g_dbus_connection_signal_subscribe(
        bus_, "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Notification", "ActionInvoked",
        "/org/freedesktop/portal/desktop", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_action_invoked_cb, this, nullptr);

    // Capability probes, done once here rather than lazily from notify() —
    // see the header comment on legacy_reply_supported_/portal_reply_supported_
    // for why a lazily-cached shared static is unsafe for this.
    if (GVariant* caps_result = g_dbus_connection_call_sync(
            bus_, "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
            "GetCapabilities", nullptr, G_VARIANT_TYPE("(as)"),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr))
    {
        GVariant* caps_v = nullptr;
        g_variant_get(caps_result, "(@as)", &caps_v);
        if (caps_v)
        {
            std::vector<std::string> caps;
            GVariantIter iter;
            g_variant_iter_init(&iter, caps_v);
            const char* s = nullptr;
            while (g_variant_iter_next(&iter, "&s", &s))
            {
                caps.emplace_back(s);
            }
            legacy_reply_supported_ =
                tesseract::linux_notify::supports_inline_reply(caps);
            g_variant_unref(caps_v);
        }
        g_variant_unref(caps_result);
    }

    if (GVariant* version_result = g_dbus_connection_call_sync(
            bus_, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop", "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", "org.freedesktop.portal.Notification",
                         "version"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
            nullptr))
    {
        GVariant* version_v = nullptr;
        g_variant_get(version_result, "(v)", &version_v);
        if (version_v)
        {
            if (g_variant_is_of_type(version_v, G_VARIANT_TYPE_UINT32))
            {
                portal_reply_supported_ =
                    g_variant_get_uint32(version_v) >=
                    tesseract::linux_notify::kPortalReplyMinVersion;
            }
            g_variant_unref(version_v);
        }
        g_variant_unref(version_result);
    }
}

LinuxNotifierGtk::~LinuxNotifierGtk()
{
    if (!bus_)
    {
        return;
    }
    if (action_sub_)
    {
        g_dbus_connection_signal_unsubscribe(bus_, action_sub_);
    }
    if (closed_sub_)
    {
        g_dbus_connection_signal_unsubscribe(bus_, closed_sub_);
    }
    if (replied_sub_)
    {
        g_dbus_connection_signal_unsubscribe(bus_, replied_sub_);
    }
    if (activation_token_sub_)
    {
        g_dbus_connection_signal_unsubscribe(bus_, activation_token_sub_);
    }
    if (portal_action_sub_)
    {
        g_dbus_connection_signal_unsubscribe(bus_, portal_action_sub_);
    }
    g_object_unref(bus_);
}

bool LinuxNotifierGtk::use_portal() const
{
    // Only Flatpak actually requires the portal — direct D-Bus calls to the
    // notification daemon are blocked by the sandbox's D-Bus proxy there.
    // Wayland used to route here too, for the portal's activation-token
    // support, but the legacy interface's own ActivationToken extension
    // (see the constructor) covers that on both KDE and GNOME without the
    // portal's downsides — notably being stuck on Notification interface
    // v1 on KDE as of this writing, with no inline-reply support at all on
    // that path (see ActivationToken's history/reasoning).
    return g_getenv("FLATPAK_ID") != nullptr;
}

void LinuxNotifierGtk::notify(const tesseract::Notification& n)
{
    if (!bus_)
    {
        return;
    }

    // freedesktop notifications have a single image slot: prefer the
    // message image / sticker (already privacy-gated upstream), fall back
    // to the room avatar.
    const std::vector<uint8_t>& pic =
        !n.image_bytes.empty() ? n.image_bytes : n.avatar_bytes;

    // Decode pic bytes to a GdkPixbuf (kept alive through the D-Bus call so
    // that the pixel pointer in the image-data variant stays valid).
    GdkPixbufLoader* loader = nullptr;
    GdkPixbuf* rgba = nullptr;
    if (!pic.empty())
    {
        loader = gdk_pixbuf_loader_new();
        gdk_pixbuf_loader_write(loader,
                                reinterpret_cast<const guchar*>(pic.data()),
                                static_cast<gsize>(pic.size()), nullptr);
        gdk_pixbuf_loader_close(loader, nullptr);
        GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pb)
        {
            GdkPixbuf* scaled =
                gdk_pixbuf_scale_simple(pb, 64, 64, GDK_INTERP_BILINEAR);
            // scale_simple returns NULL on allocation failure / bad dims.
            if (scaled)
            {
                if (gdk_pixbuf_get_has_alpha(scaled))
                {
                    rgba = scaled;
                }
                else
                {
                    rgba = gdk_pixbuf_add_alpha(scaled, FALSE, 0, 0, 0);
                    g_object_unref(scaled);
                }
            }
        }
    }

    if (use_portal())
    {
        const std::string pid = tesseract::linux_portal::sanitize_notification_id(n.room_id);
        // Record mapping so on_portal_action_invoked_cb can look up the
        // room. Each notification for the same room reuses the same id
        // (replacing the prior one), so this just tracks the latest
        // event_id per room.
        correlation_.record_portal(pid, n.room_id, n.event_id);
        GVariantBuilder notif_b;
        g_variant_builder_init(&notif_b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&notif_b, "{sv}", "title",
                              g_variant_new_string(n.sender.c_str()));
        g_variant_builder_add(&notif_b, "{sv}", "body",
                              g_variant_new_string(n.body.c_str()));
        g_variant_builder_add(&notif_b, "{sv}", "default-action",
                              g_variant_new_string("default"));
        if (portal_reply_supported_)
        {
            // im.reply-with-text (portal interface v2+): a "category" of
            // im.received plus a button with this purpose gets the
            // compositor to render its own inline-reply widget. The
            // button's label IS genuinely shown (unlike the legacy KDE
            // extension below), so it's localized.
            g_variant_builder_add(
                &notif_b, "{sv}", "category",
                g_variant_new_string(tesseract::linux_notify::kPortalImCategory));
            const std::string reply_label = tk::tr("Reply");
            GVariantBuilder button_b;
            g_variant_builder_init(&button_b, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&button_b, "{sv}", "label",
                                  g_variant_new_string(reply_label.c_str()));
            g_variant_builder_add(
                &button_b, "{sv}", "action",
                g_variant_new_string(tesseract::linux_notify::kPortalReplyAction));
            g_variant_builder_add(
                &button_b, "{sv}", "purpose",
                g_variant_new_string(tesseract::linux_notify::kPortalReplyPurpose));
            GVariantBuilder buttons_b;
            g_variant_builder_init(&buttons_b, G_VARIANT_TYPE("aa{sv}"));
            g_variant_builder_add(&buttons_b, "a{sv}", &button_b);
            g_variant_builder_add(&notif_b, "{sv}", "buttons",
                                  g_variant_new("aa{sv}", &buttons_b));
        }
        if (!pic.empty())
        {
            // Pass raw encoded bytes as a bytes-icon GIcon — the portal daemon
            // handles decode. g_bytes_new copies so the GVariant owns the data.
            GBytes* gb = g_bytes_new(pic.data(), pic.size());
            GVariant* icv =
                g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), gb, TRUE);
            g_bytes_unref(gb);
            g_variant_builder_add(&notif_b, "{sv}", "icon",
                                  g_variant_new("(sv)", "bytes-icon", icv));
        }
        g_dbus_connection_call(
            bus_, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Notification", "AddNotification",
            g_variant_new("(sa{sv})", pid.c_str(), &notif_b), nullptr,
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        if (rgba)
        {
            g_object_unref(rgba);
        }
        if (loader)
        {
            g_object_unref(loader);
        }
        return;
    }

    GVariantBuilder actions_b;
    g_variant_builder_init(&actions_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions_b, "s", "default");
    g_variant_builder_add(&actions_b, "s", "Open");
    if (legacy_reply_supported_)
    {
        // The label is unused by KDE for this special action id — it renders
        // its own hardcoded reply affordance instead. Not localized: this is
        // a protocol-level literal, must match KDE's hardcoded string.
        g_variant_builder_add(&actions_b, "s",
                              tesseract::linux_notify::kInlineReplyAction);
        g_variant_builder_add(&actions_b, "s", "Reply");
    }

    GVariantBuilder hints_b;
    g_variant_builder_init(&hints_b, G_VARIANT_TYPE("a{sv}"));
    // Required for the daemon's ActivationToken signal to know which app to
    // mint a Wayland xdg_activation_v1 token for (see the ActivationToken
    // subscription in the constructor) — mirrors KDE's own
    // knotifications/src/notifybypopup.cpp. Matches the basename the GTK
    // package installs (packaging/debian/rules installs it to the
    // tesseract-matrix-gtk package; see LinuxAutostartGtk.cpp's identical
    // "tesseract-matrix-gtk" literal).
    g_variant_builder_add(&hints_b, "{sv}", "desktop-entry",
                          g_variant_new_string("tesseract-matrix-gtk"));
    if (legacy_reply_supported_)
    {
        // Plain text, not markup — these render as a widget placeholder /
        // button label in Plasma's own reply UI, not Pango-parsed body text,
        // so (unlike sender/body) they must not go through
        // g_markup_escape_text.
        const std::string placeholder = tk::tr("Reply\xe2\x80\xa6");
        const std::string submit_label = tk::tr("Send");
        g_variant_builder_add(&hints_b, "{sv}",
                              tesseract::linux_notify::kHintPlaceholder,
                              g_variant_new_string(placeholder.c_str()));
        g_variant_builder_add(&hints_b, "{sv}",
                              tesseract::linux_notify::kHintSubmitLabel,
                              g_variant_new_string(submit_label.c_str()));
    }
    if (rgba)
    {
        // image-data hint: (iiibiiay) — width, height, rowstride, has_alpha,
        // bits_per_sample, channels, pixel_data.  The pixel pointer is owned by
        // rgba which outlives the synchronous g_dbus_connection_call_sync below.
        const int w = gdk_pixbuf_get_width(rgba);
        const int h = gdk_pixbuf_get_height(rgba);
        const int rs = gdk_pixbuf_get_rowstride(rgba);
        const int ch = gdk_pixbuf_get_n_channels(rgba);
        const gboolean ha = gdk_pixbuf_get_has_alpha(rgba);
        const guchar* px = gdk_pixbuf_get_pixels(rgba);
        GVariant* data_v = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, px, static_cast<gsize>(rs) * h, 1);
        g_variant_builder_add(
            &hints_b, "{sv}", "image-data",
            g_variant_new("(iiibii@ay)", w, h, rs, ha, 8, ch, data_v));
    }

    // Several daemons (dunst, mako, GNOME Shell) render Pango markup in the
    // legacy Notify body; escape server-supplied text so it can't inject
    // markup. g_variant_new("s", ...) copies, so free right after.
    gchar* esc_sender = g_markup_escape_text(n.sender.c_str(), -1);
    gchar* esc_body = g_markup_escape_text(n.body.c_str(), -1);
    // Always pass replaces_id=0 so every notification generates a fresh
    // popup. Using replaces causes the daemon to update the existing toast
    // in place without re-triggering the animation or sound, making
    // subsequent messages from the same room invisible to the user.
    GVariant* params =
        g_variant_new("(susssasa{sv}i)", "Tesseract", 0u, "tesseract",
                      esc_sender, esc_body, &actions_b, &hints_b, 5000);
    g_free(esc_sender);
    g_free(esc_body);

    GVariant* result = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify", params,
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    // Safe to release avatar resources now that the sync call has serialised params.
    if (rgba)
    {
        g_object_unref(rgba);
    }
    if (loader)
    {
        g_object_unref(loader);
    }

    if (result)
    {
        uint32_t id = 0;
        g_variant_get(result, "(u)", &id);
        g_variant_unref(result);
        correlation_.record(id, n.room_id, n.event_id);
    }
}

void LinuxNotifierGtk::on_action_invoked_cb(GDBusConnection*, const char*,
                                            const char*, const char*,
                                            const char*, GVariant* parameters,
                                            gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0;
    const char* action = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &action);
    if (auto found = self->correlation_.find(id))
    {
        // ActivationToken (if the daemon supports it) always arrives before
        // ActionInvoked for the same id — take_token() returns empty if the
        // daemon never sent one.
        self->on_activate_(found->room_id, self->correlation_.take_token(id));
    }
}

void LinuxNotifierGtk::on_notification_closed_cb(GDBusConnection*, const char*,
                                                 const char*, const char*,
                                                 const char*,
                                                 GVariant* parameters,
                                                 gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0, reason = 0;
    g_variant_get(parameters, "(uu)", &id, &reason);
    self->correlation_.forget(id);
}

void LinuxNotifierGtk::on_activation_token_cb(GDBusConnection*, const char*,
                                              const char*, const char*,
                                              const char*, GVariant* parameters,
                                              gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0;
    const char* token = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &token);
    self->correlation_.stash_token(id, token ? token : "");
}

void LinuxNotifierGtk::on_notification_replied_cb(GDBusConnection*, const char*,
                                                  const char*, const char*,
                                                  const char*,
                                                  GVariant* parameters,
                                                  gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0;
    const char* text = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &text);
    auto found = self->correlation_.find(id);
    if (!found)
    {
        return; // stale id — daemon already closed/expired this toast
    }
    self->on_reply_(found->room_id, found->event_id, text ? text : "");
}

void LinuxNotifierGtk::on_portal_action_invoked_cb(GDBusConnection*,
                                                   const char*, const char*,
                                                   const char*, const char*,
                                                   GVariant* parameters,
                                                   gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    const char* notif_id = nullptr;
    const char* action = nullptr;
    GVariant* param_av = nullptr;
    // Portal ActionInvoked format: (s s av)
    // The av elements are, in order: optional target, platform-data a{sv}
    // (portal >= 1.16, contains "activation-token" on Wayland), optional response.
    g_variant_get(parameters, "(&s&s@av)", &notif_id, &action, &param_av);

    if (!notif_id)
    {
        if (param_av)
            g_variant_unref(param_av);
        return;
    }
    auto found = self->correlation_.find_portal(notif_id);
    if (!found)
    {
        if (param_av)
            g_variant_unref(param_av);
        return;
    }

    if (g_strcmp0(action, tesseract::linux_notify::kPortalReplyAction) == 0)
    {
        // im.reply-with-text: our action id isn't "app."-prefixed, so it's
        // non-exported — the daemon delivers the typed text as the third
        // element (index 2) of this signal's parameter array.
        std::string text;
        if (param_av && g_variant_n_children(param_av) >= 3)
        {
            GVariant* elem = g_variant_get_child_value(param_av, 2);
            GVariant* inner = g_variant_get_variant(elem);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING))
            {
                text = g_variant_get_string(inner, nullptr);
            }
            g_variant_unref(inner);
            g_variant_unref(elem);
        }
        if (param_av)
        {
            g_variant_unref(param_av);
        }
        if (!text.empty())
        {
            self->on_reply_(found->room_id, found->event_id, text);
        }
        return;
    }

    // Click-to-open: search the av for the a{sv} element that carries
    // "activation-token".
    std::string token;
    if (param_av)
    {
        GVariantIter iter;
        g_variant_iter_init(&iter, param_av);
        GVariant* elem;
        while ((elem = g_variant_iter_next_value(&iter)))
        {
            // Each element in av has type v; unwrap to get the inner value.
            GVariant* inner = g_variant_get_variant(elem);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_VARDICT))
            {
                GVariant* tv = g_variant_lookup_value(
                    inner, "activation-token", G_VARIANT_TYPE_STRING);
                if (tv)
                {
                    token = g_variant_get_string(tv, nullptr);
                    g_variant_unref(tv);
                }
            }
            g_variant_unref(inner);
            g_variant_unref(elem);
            if (!token.empty())
                break;
        }
        g_variant_unref(param_av);
    }

    self->on_activate_(found->room_id, std::move(token));
}
