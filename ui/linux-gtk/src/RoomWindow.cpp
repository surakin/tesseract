#include "RoomWindow.h"
#include "MainWindow.h"
#include "views/PopoutRoomWidget.h"

#include <libintl.h>
#include <string_view>
#define _(s) gettext(s)

namespace gtk4
{

RoomWindow::RoomWindow(MainWindow* parent_shell, const std::string& room_id)
    : tesseract::RoomWindowBase(parent_shell, room_id),
      parent_shell_(parent_shell)
{
    window_ = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(window_, 800, 600);
    gtk_window_set_title(window_, room_id.c_str());

    surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    gtk_window_set_child(window_, surface_->widget());

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>();
    room_view_  = room_widget->room_view();
    img_viewer_ = room_widget->image_viewer();
    vid_viewer_ = room_widget->video_viewer();
    surface_->set_root(std::move(room_widget));

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
    }

    // ── Image / video save dialogs ────────────────────────────────────────
    img_viewer_->on_save =
        [this](std::string source_url, std::string filename_hint)
    {
        std::string suggested = filename_hint.empty() ? "image" : filename_hint;
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
            "Save image", window_, GTK_FILE_CHOOSER_ACTION_SAVE,
            "Save", "Cancel");
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                          suggested.c_str());
        struct Ctx { RoomWindow* self; std::string src; };
        auto* ctx = new Ctx{this, std::move(source_url)};
        g_signal_connect(
            dlg, "response",
            G_CALLBACK(+[](GtkNativeDialog* d, gint resp, gpointer p)
            {
                auto* c = static_cast<Ctx*>(p);
                if (resp == GTK_RESPONSE_ACCEPT)
                {
                    GFile* gf = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                    char* cpath = g_file_get_path(gf);
                    std::string dest(cpath);
                    g_free(cpath);
                    g_object_unref(gf);
                    c->self->save_source_to_file_(std::move(c->src), dest);
                }
                delete c;
                gtk_native_dialog_destroy(d);
            }),
            ctx);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
    };
    vid_viewer_->on_save =
        [this](std::string source_json, std::string mime_type)
    {
        std::string suggested = "video";
        if (mime_type == "video/mp4")
            suggested = "video.mp4";
        else if (mime_type == "video/webm")
            suggested = "video.webm";
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
            "Save video", window_, GTK_FILE_CHOOSER_ACTION_SAVE,
            "Save", "Cancel");
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                          suggested.c_str());
        struct Ctx { RoomWindow* self; std::string src; };
        auto* ctx = new Ctx{this, std::move(source_json)};
        g_signal_connect(
            dlg, "response",
            G_CALLBACK(+[](GtkNativeDialog* d, gint resp, gpointer p)
            {
                auto* c = static_cast<Ctx*>(p);
                if (resp == GTK_RESPONSE_ACCEPT)
                {
                    GFile* gf = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                    char* cpath = g_file_get_path(gf);
                    std::string dest(cpath);
                    g_free(cpath);
                    g_object_unref(gf);
                    c->self->save_source_to_file_(std::move(c->src), dest);
                }
                delete c;
                gtk_native_dialog_destroy(d);
            }),
            ctx);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
    };

    // ── Surface-bound providers (need this shell's own surface_) ─────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }
    room_view_->set_post_delayed(
        [this](int ms, std::function<void()> fn)
        {
            if (surface_)
            {
                surface_->host().post_delayed(ms, std::move(fn));
            }
        });
    room_view_->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    room_view_->on_set_clipboard = [this](std::string_view t)
    {
        if (surface_)
            surface_->host().set_clipboard_text(t);
    };
    room_view_->message_list()->on_show_copy_menu = [this]()
    {
        if (!copy_ctx_menu_)
        {
            GMenu* menu = g_menu_new();
            g_menu_append(menu, _("Copy"), "copy-sel.copy");
            copy_ctx_menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
            gtk_popover_set_has_arrow(GTK_POPOVER(copy_ctx_menu_), FALSE);
            gtk_widget_set_parent(copy_ctx_menu_, surface_->widget());
            g_object_unref(menu);
            copy_ctx_actions_ = g_simple_action_group_new();
            GSimpleAction* act = g_simple_action_new("copy", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_copy_action_), this);
            g_action_map_add_action(G_ACTION_MAP(copy_ctx_actions_),
                                    G_ACTION(act));
            g_object_unref(act);
            gtk_widget_insert_action_group(surface_->widget(), "copy-sel",
                                           G_ACTION_GROUP(copy_ctx_actions_));
        }
        GtkWidget* w = surface_->widget();
        GdkDisplay* dpy = gtk_widget_get_display(w);
        GdkSeat* seat = gdk_display_get_default_seat(dpy);
        GdkDevice* ptr = gdk_seat_get_pointer(seat);
        GdkSurface* surf = gtk_native_get_surface(
            GTK_NATIVE(gtk_widget_get_native(w)));
        double sx = 0, sy = 0;
        if (surf)
            gdk_surface_get_device_position(surf, ptr, &sx, &sy, nullptr);
        double wx = 0, wy = 0;
        gtk_widget_translate_coordinates(
            GTK_WIDGET(gtk_widget_get_native(w)), w, sx, sy, &wx, &wy);
        GdkRectangle r{static_cast<int>(wx), static_cast<int>(wy), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(copy_ctx_menu_), &r);
        gtk_popover_popup(GTK_POPOVER(copy_ctx_menu_));
    };

    // "destroy" fires when the GtkWindow is destroyed (user clicks X or
    // gtk_window_destroy is called). At that point the GtkWidget tree is
    // already gone; schedule the C++ object deletion for next idle.
    g_signal_connect(window_, "destroy", G_CALLBACK(on_destroy_), this);

    // Escape key: close open viewer overlays regardless of focused widget.
    {
        GtkEventController* key_ctl = gtk_event_controller_key_new();
        g_signal_connect(key_ctl, "key-pressed",
                         G_CALLBACK(on_key_pressed_), this);
        gtk_widget_add_controller(GTK_WIDGET(window_), key_ctl);
    }

    gtk_window_present(window_);
    finish_init_();
}

RoomWindow::~RoomWindow()
{
    if (window_)
    {
        g_signal_handlers_disconnect_by_data(window_, this);
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
}

// static
void RoomWindow::on_copy_action_(GSimpleAction* /*action*/,
                                  GVariant* /*parameter*/,
                                  gpointer self)
{
    auto* w = static_cast<RoomWindow*>(self);
    if (w->room_view_)
        w->room_view_->message_list()->copy_selection();
}

// static
void RoomWindow::on_destroy_(GtkWidget* /*widget*/, gpointer self)
{
    auto* w = static_cast<RoomWindow*>(self);
    w->window_ = nullptr; // already destroyed; prevent double-destroy in dtor
    w->schedule_self_close_();
}

// static
gboolean RoomWindow::on_key_pressed_(GtkEventControllerKey*, guint keyval,
                                      guint, GdkModifierType state,
                                      gpointer self)
{
    auto* w = static_cast<RoomWindow*>(self);
    if (keyval == GDK_KEY_c && (state & GDK_CONTROL_MASK))
    {
        if (w->room_view_ && w->room_view_->message_list()->has_selection())
        {
            w->room_view_->message_list()->copy_selection();
            return TRUE;
        }
    }
    if (keyval == GDK_KEY_Escape)
    {
        if (w->vid_viewer_ && w->vid_viewer_->is_open())
        {
            w->vid_viewer_->close();
            w->vid_viewer_->set_visible(false);
            w->request_relayout();
            return TRUE;
        }
        if (w->img_viewer_ && w->img_viewer_->is_open())
        {
            w->img_viewer_->close();
            w->img_viewer_->set_visible(false);
            w->request_relayout();
            return TRUE;
        }
    }
    return FALSE;
}

void RoomWindow::bring_to_front()
{
    if (window_)
    {
        gtk_window_present(window_);
    }
}

void RoomWindow::close_window()
{
    if (window_)
    {
        gtk_window_destroy(window_);
    }
}

void RoomWindow::request_relayout()
{
    if (surface_)
    {
        surface_->relayout();
    }
}

void RoomWindow::update_window_title_(const std::string& name)
{
    if (window_)
    {
        gtk_window_set_title(window_, name.c_str());
    }
}

void RoomWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
    }
}

void RoomWindow::surface_repaint_()
{
    if (surface_)
    {
        gtk_widget_queue_draw(surface_->widget());
    }
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace gtk4
