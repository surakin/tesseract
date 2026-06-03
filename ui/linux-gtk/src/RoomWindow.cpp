#include "RoomWindow.h"
#include "MainWindow.h"
#include "views/EmojiPicker.h"
#include "views/PopoutRoomWidget.h"
#include "views/StickerPicker.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>

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
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Save image");
        gtk_file_dialog_set_initial_name(dlg, suggested.c_str());
        struct Ctx { RoomWindow* self; std::string src; };
        auto* ctx = new Ctx{this, std::move(source_url)};
        gtk_file_dialog_save(dlg, GTK_WINDOW(window_), nullptr,
            +[](GObject* dialog_obj, GAsyncResult* res, gpointer p)
            {
                auto* c = static_cast<Ctx*>(p);
                GError* err = nullptr;
                GFile* gf = gtk_file_dialog_save_finish(
                    GTK_FILE_DIALOG(dialog_obj), res, &err);
                if (gf)
                {
                    char* cpath = g_file_get_path(gf);
                    std::string dest(cpath);
                    g_free(cpath);
                    g_object_unref(gf);
                    c->self->save_source_to_file_(std::move(c->src), dest);
                }
                if (err) g_error_free(err);
                delete c;
            }, ctx);
        g_object_unref(dlg);
    };
    vid_viewer_->on_save =
        [this](std::string source_json, std::string mime_type)
    {
        std::string suggested = "video";
        if (mime_type == "video/mp4")
            suggested = "video.mp4";
        else if (mime_type == "video/webm")
            suggested = "video.webm";
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Save video");
        gtk_file_dialog_set_initial_name(dlg, suggested.c_str());
        struct Ctx { RoomWindow* self; std::string src; };
        auto* ctx = new Ctx{this, std::move(source_json)};
        gtk_file_dialog_save(dlg, GTK_WINDOW(window_), nullptr,
            +[](GObject* dialog_obj, GAsyncResult* res, gpointer p)
            {
                auto* c = static_cast<Ctx*>(p);
                GError* err = nullptr;
                GFile* gf = gtk_file_dialog_save_finish(
                    GTK_FILE_DIALOG(dialog_obj), res, &err);
                if (gf)
                {
                    char* cpath = g_file_get_path(gf);
                    std::string dest(cpath);
                    g_free(cpath);
                    g_object_unref(gf);
                    c->self->save_source_to_file_(std::move(c->src), dest);
                }
                if (err) g_error_free(err);
                delete c;
            }, ctx);
        g_object_unref(dlg);
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
        graphene_point_t pt_in{static_cast<float>(sx), static_cast<float>(sy)};
        graphene_point_t pt_out{};
        if (!gtk_widget_compute_point(
                GTK_WIDGET(gtk_widget_get_native(w)), w, &pt_in, &pt_out))
            pt_out = {};
        GdkRectangle r{static_cast<int>(pt_out.x), static_cast<int>(pt_out.y), 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(copy_ctx_menu_), &r);
        gtk_popover_popup(GTK_POPOVER(copy_ctx_menu_));
    };

    // ── Compose text area overlay + @mention autocomplete ─────────────────
    room_text_area_ = surface_->host().make_text_area();
    room_text_area_->set_mention_colors(surface_->theme().palette.accent,
                                        surface_->theme().palette.text_on_accent);
    room_text_area_->set_placeholder(_("Message\xe2\x80\xa6"));
    room_text_area_->set_on_changed(
        [this](const std::string& s)
        {
            if (room_view_)
                room_view_->set_current_text(s);
            if (mention_controller_)
                mention_controller_->on_text_changed(
                    s, room_text_area_->cursor_byte_pos());
        });
    room_text_area_->set_on_submit(
        [this]
        {
            if (mention_controller_ && mention_controller_->on_submit())
                return;
            if (room_view_)
                room_view_->compose_bar()->trigger_send();
        });
    room_text_area_->set_on_height_changed(
        [this](float h)
        {
            if (room_view_)
                room_view_->set_text_area_natural_height(h);
            if (surface_)
                surface_->relayout();
        });
    room_text_area_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        { return mention_controller_ && mention_controller_->on_nav(nk); });
    room_text_area_->set_on_edit_last(
        [this] { return room_view_ && room_view_->edit_last_own(); });

    mention_popover_ = gtk_popover_new();
    gtk_widget_set_parent(mention_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(mention_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(mention_popover_), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(mention_popover_), TRUE);
    mention_popup_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    {
        auto w = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = w.get();
        mention_popup_surface_->set_root(std::move(w));
        gtk_popover_set_child(GTK_POPOVER(mention_popover_),
                              mention_popup_surface_->widget());
    }

    tesseract::views::MentionController::Hooks hooks;
    hooks.show = [this](tk::Rect cursor, int rows)
    { show_mention_popup_(cursor, rows); };
    hooks.hide = [this]
    {
        if (mention_popover_)
            gtk_popover_popdown(GTK_POPOVER(mention_popover_));
    };
    hooks.repaint = [this]
    {
        if (mention_popup_surface_)
            mention_popup_surface_->host().request_repaint();
    };
    hooks.room_id = [this] { return room_id_; };
    hooks.run_async = [this](std::function<void()> fn)
    { run_async_(std::move(fn)); };
    hooks.post_to_ui = [this](std::function<void()> fn)
    { post_to_ui_(std::move(fn)); };
    wire_mention_shell_hooks_(mention_popup_widget_, hooks);
    mention_controller_ = std::make_unique<tesseract::views::MentionController>(
        room_text_area_.get(), shell_client_(), mention_popup_widget_,
        std::move(hooks));

    surface_->set_on_layout(
        [this]
        {
            if (room_view_ && room_text_area_)
            {
                const tk::Rect ta = room_view_->compose_text_area_rect();
                room_text_area_->set_visible(!ta.empty());
                if (!ta.empty())
                    room_text_area_->set_rect(ta);
            }
        });

    // ── Platform popups the shared wire_room_view_ can't provide ──────────
    build_emoji_popover_();
    build_sticker_popover_();
    room_view_->on_emoji = [this](tk::Rect btn)
    {
        if (!emoji_popover_)
            return;
        if (gtk_widget_get_visible(emoji_popover_))
            gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        else
            popup_emoji_at_rect_(btn);
    };
    room_view_->on_sticker = [this](tk::Rect btn)
    {
        if (!sticker_popover_)
            return;
        if (gtk_widget_get_visible(sticker_popover_))
            gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        else
            popup_sticker_at_rect_(btn);
    };
    room_view_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor)
    {
        if (!emoji_popover_ || room_id_.empty())
            return;
        pending_reaction_event_id_ = event_id;
        popup_emoji_at_rect_(anchor);
    };
    room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        GtkWidget* w = surface_->widget();
        if (!topic_tooltip_popover_)
        {
            topic_tooltip_label_ = gtk_label_new(nullptr);
            gtk_label_set_wrap(GTK_LABEL(topic_tooltip_label_), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(topic_tooltip_label_), 60);
            topic_tooltip_popover_ = gtk_popover_new();
            gtk_widget_add_css_class(topic_tooltip_popover_, "tooltip");
            gtk_popover_set_child(GTK_POPOVER(topic_tooltip_popover_),
                                  topic_tooltip_label_);
            gtk_widget_set_parent(topic_tooltip_popover_, w);
            gtk_popover_set_autohide(GTK_POPOVER(topic_tooltip_popover_),
                                     FALSE);
            gtk_popover_set_has_arrow(GTK_POPOVER(topic_tooltip_popover_),
                                      FALSE);
        }
        gtk_label_set_text(GTK_LABEL(topic_tooltip_label_), text.c_str());
        GdkRectangle rect{
            static_cast<int>(anchor.x), static_cast<int>(anchor.y),
            static_cast<int>(anchor.w), static_cast<int>(anchor.h)};
        gtk_popover_set_pointing_to(GTK_POPOVER(topic_tooltip_popover_), &rect);
        gtk_popover_popup(GTK_POPOVER(topic_tooltip_popover_));
    };
    room_view_->on_hide_tooltip = [this]
    {
        if (topic_tooltip_popover_)
            gtk_popover_popdown(GTK_POPOVER(topic_tooltip_popover_));
    };
    room_view_->on_link_hovered = [this](const std::string& url)
    {
        gtk_widget_set_cursor_from_name(surface_->widget(),
                                        url.empty() ? "default" : "pointer");
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

    // Ctrl+K opens the quick switcher on the main window. A global-scope
    // shortcut controller fires even while the compose entry holds focus; the
    // switcher widget lives in the main window, so we route there and raise it.
    {
        GtkEventController* sc = gtk_shortcut_controller_new();
        gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(sc),
                                          GTK_SHORTCUT_SCOPE_GLOBAL);
        GtkShortcut* shortcut = gtk_shortcut_new(
            gtk_keyval_trigger_new(GDK_KEY_k, GDK_CONTROL_MASK),
            gtk_callback_action_new(on_quick_switch_shortcut_, this, nullptr));
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             shortcut);
        gtk_widget_add_controller(GTK_WIDGET(window_), sc);
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

void RoomWindow::build_emoji_popover_()
{
    emoji_popover_ = gtk_popover_new();
    gtk_widget_set_parent(emoji_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(emoji_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(emoji_popover_), TRUE);

    emoji_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(shell_client_());
    emoji_picker_shared_->on_selected = [this](const std::string& glyph)
    {
        if (!pending_reaction_event_id_.empty())
        {
            std::string ev = std::move(pending_reaction_event_id_);
            pending_reaction_event_id_.clear();
            toggle_reaction_(ev, glyph, std::string{});
            if (emoji_popover_)
                gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
            return;
        }
        if (!room_text_area_)
            return;
        room_text_area_->insert_at_cursor(glyph);
        if (room_view_)
            room_view_->set_current_text(room_text_area_->text());
        room_text_area_->set_focused(true);
    };
    emoji_picker_shared_->on_emoticon_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (!pending_reaction_event_id_.empty())
        {
            std::string ev = std::move(pending_reaction_event_id_);
            pending_reaction_event_id_.clear();
            if (!img.url.empty())
                toggle_reaction_(ev, std::string{}, img.url);
            if (emoji_popover_)
                gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
            return;
        }
        if (!room_text_area_)
            return;
        room_text_area_->insert_at_cursor(":" + img.shortcode + ":");
        if (room_view_)
            room_view_->set_current_text(room_text_area_->text());
        room_text_area_->set_focused(true);
    };
    emoji_picker_shared_->set_image_provider(picker_image_provider_(false));
    emoji_picker_surface_->set_root(std::move(shared));

    emoji_picker_search_field_ =
        emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder(_("Search emoji"));
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (emoji_picker_shared_)
                emoji_picker_shared_->set_search_query(q);
            if (emoji_picker_surface_)
                emoji_picker_surface_->relayout();
        });
    emoji_picker_surface_->set_on_layout(
        [this]
        {
            if (emoji_picker_search_field_ && emoji_picker_shared_)
                emoji_picker_search_field_->set_rect(
                    emoji_picker_shared_->search_field_rect());
        });

    GtkWidget* surface_widget = emoji_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 320, 360);
    gtk_popover_set_child(GTK_POPOVER(emoji_popover_), surface_widget);
}

void RoomWindow::build_sticker_popover_()
{
    sticker_popover_ = gtk_popover_new();
    gtk_widget_set_parent(sticker_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(sticker_popover_), TRUE);

    sticker_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(shell_client_());
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (room_id_.empty())
            return;
        std::string body = img.body.empty() ? img.shortcode : img.body;
        if (auto* c = shell_client_())
            c->send_sticker(room_id_, body, img.url, img.info_json);
        if (sticker_popover_)
            gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
    };
    sticker_picker_shared_->set_image_provider(picker_image_provider_(true));
    sticker_picker_surface_->set_root(std::move(shared));

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder(_("Search stickers"));
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (sticker_picker_shared_)
                sticker_picker_shared_->set_search_query(q);
            if (sticker_picker_surface_)
                sticker_picker_surface_->relayout();
        });
    sticker_picker_surface_->set_on_layout(
        [this]
        {
            if (sticker_picker_search_field_ && sticker_picker_shared_)
                sticker_picker_search_field_->set_rect(
                    sticker_picker_shared_->search_field_rect());
        });

    GtkWidget* surface_widget = sticker_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 360, 420);
    gtk_popover_set_child(GTK_POPOVER(sticker_popover_), surface_widget);
}

void RoomWindow::popup_emoji_at_rect_(tk::Rect anchor)
{
    if (!emoji_popover_)
        return;
    GdkRectangle r{
        static_cast<int>(anchor.x), static_cast<int>(anchor.y),
        static_cast<int>(anchor.w), static_cast<int>(anchor.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
        emoji_picker_shared_->set_search_query("");
    }
    if (emoji_picker_search_field_)
        emoji_picker_search_field_->set_text("");
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_)
        emoji_picker_surface_->relayout();
}

void RoomWindow::popup_sticker_at_rect_(tk::Rect anchor)
{
    if (!sticker_popover_)
        return;
    GdkRectangle r{
        static_cast<int>(anchor.x), static_cast<int>(anchor.y),
        static_cast<int>(anchor.w), static_cast<int>(anchor.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
        sticker_picker_shared_->set_search_query("");
    }
    if (sticker_picker_search_field_)
        sticker_picker_search_field_->set_text("");
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_)
        sticker_picker_surface_->relayout();
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

// static
gboolean RoomWindow::on_quick_switch_shortcut_(GtkWidget*, GVariant*,
                                               gpointer self)
{
    auto* w = static_cast<RoomWindow*>(self);
    if (w->parent_shell_)
        w->parent_shell_->request_quick_switch_from_popout();
    return TRUE;
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
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
    }
    if (room_text_area_)
    {
        room_text_area_->set_mention_colors(t.palette.accent,
                                            t.palette.text_on_accent);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(t);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(t);
    }
}

void RoomWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popover_)
    {
        return;
    }
    int w = int(tesseract::views::MentionPopup::kWidth);
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);
    gtk_widget_set_size_request(mention_popup_surface_->widget(), w, h);
    GdkRectangle r{int(cursor_local.x), int(cursor_local.y),
                   int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(mention_popover_), &r);
    gtk_popover_popup(GTK_POPOVER(mention_popover_));
}

void RoomWindow::surface_repaint_()
{
    if (surface_)
    {
        gtk_widget_queue_draw(surface_->widget());
    }
}

void RoomWindow::repaint_anim_frame()
{
    surface_repaint_();
    if (emoji_popover_ && gtk_widget_get_visible(emoji_popover_))
    {
        if (emoji_picker_shared_)
            emoji_picker_shared_->invalidate_image_cache();
        if (emoji_picker_surface_)
            emoji_picker_surface_->relayout();
    }
    if (sticker_popover_ && gtk_widget_get_visible(sticker_popover_))
    {
        if (sticker_picker_shared_)
            sticker_picker_shared_->invalidate_image_cache();
        if (sticker_picker_surface_)
            sticker_picker_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace gtk4
