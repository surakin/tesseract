#include "RoomWindow.h"
#include "MainWindow.h"
#include "views/ComposePopups.h"
#include "views/EmojiPicker.h"
#include "views/PopoutRoomWidget.h"
#include "views/StickerPicker.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>
#include <tesseract/settings.h>

#include <string_view>

#include "gettext_shorthand.h"

namespace gtk4
{

RoomWindow::RoomWindow(MainWindow* parent_shell, const std::string& room_id)
    : tesseract::RoomWindowBase(parent_shell, room_id),
      parent_shell_(parent_shell)
{
    window_ = GTK_WINDOW(gtk_window_new());
    // Associate with the GtkApplication so this is a proper application window.
    // Without it, a bare gtk_window_new() pop-out mis-routes popover keyboard
    // grabs — the composer stops receiving keys while any popup is open even
    // though it keeps focus (cursor still blinks). The main window works
    // because it is a gtk_application_window_new().
    if (parent_shell_ && parent_shell_->application())
    {
        gtk_window_set_application(window_, parent_shell_->application());
    }
    gtk_window_set_title(window_, room_id.c_str());

    // Apply saved size (GTK4/Wayland: position is compositor-managed).
    {
        const auto saved = get_saved_popout_geometry_(800, 600);
        gtk_window_set_default_size(window_,
                                    saved.valid ? saved.w : 800,
                                    saved.valid ? saved.h : 600);
    }

    // Save popout window size to Settings whenever the user resizes it.
    g_signal_connect(
        window_, "notify::default-width",
        G_CALLBACK(+[](GObject* /*obj*/, GParamSpec* /*ps*/, gpointer data)
                   {
                       auto* self = static_cast<RoomWindow*>(data);
                       int w = 0;
                       int h = 0;
                       gtk_window_get_default_size(self->window_, &w, &h);
                       self->save_popout_geometry_(0, 0, w, h);
                   }),
        this);
    g_signal_connect(
        window_, "notify::default-height",
        G_CALLBACK(+[](GObject* /*obj*/, GParamSpec* /*ps*/, gpointer data)
                   {
                       auto* self = static_cast<RoomWindow*>(data);
                       int w = 0;
                       int h = 0;
                       gtk_window_get_default_size(self->window_, &w, &h);
                       self->save_popout_geometry_(0, 0, w, h);
                   }),
        this);

    surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    gtk_window_set_child(window_, surface_->widget());

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>(
        &surface_->host());
    room_view_             = room_widget->room_view();
    img_viewer_            = room_widget->image_viewer();
    vid_viewer_            = room_widget->video_viewer();
    forward_picker_widget_ = room_widget->forward_picker();
    room_media_view_widget_ = room_widget->room_media_view();
    confirm_dialog_widget_ = room_widget->confirm_dialog();
    room_widget->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    surface_->set_root(std::move(room_widget));

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
    }

    // Inline autoplay video/GIF in the timeline (separate from the lightbox
    // player above — MessageListView falls back to a static thumbnail unless
    // both of these are set).
    room_view_->set_video_player_factory(
        [this]() { return surface_->host().make_video_player(); });
    room_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
        {
            fetch_source_bytes_(src, std::move(on_ready));
        });

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
    room_view_->on_file_clicked =
        [this](const tesseract::views::MessageListView::FileHit& hit)
    {
        std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
        GtkFileDialog* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Save file");
        gtk_file_dialog_set_initial_name(dlg, suggested.c_str());
        struct Ctx { RoomWindow* self; std::string src; };
        auto* ctx = new Ctx{this, hit.source ? hit.source->fetch_token() : std::string{}};
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

    // Drag-and-drop file ingest is now tree-dispatched automatically (see
    // Host::ingest_native_file_drop -> Host::dispatch_file_drop);
    // RoomView::on_file_drop routes into this window's compose bar via the
    // provider fields wired in wire_room_view_. Pop-outs never open their
    // RoomSettingsView, so it's always invisible and never claims a drop
    // ahead of the compose bar.
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            shell_show_status_message_(std::move(reason));
        });

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

    // ── Compose text area (self-owned) + @mention autocomplete ────────────
    room_text_area_ = room_view_->compose_bar()->text_area();
    room_text_area_->set_on_changed(
        [this](const std::string& s)
        {
            if (room_view_)
                room_view_->set_current_text(s);
            // Drive all composer popups through the shared priority dispatch
            // (gif > slash > shortcode > mention).
            tesseract::views::dispatch_compose_text_changed(
                s, room_text_area_->cursor_byte_pos(), gif_controller_.get(),
                slash_controller_.get(), shortcode_controller_.get(),
                mention_controller_.get());
        });
    room_text_area_->set_on_submit(
        [this]
        {
            if (tesseract::views::dispatch_compose_submit(
                    gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get()))
                return;
            if (room_view_)
                room_view_->compose_bar()->trigger_send();
        });
    // Auto-grow (set_on_height_changed) is wired internally by ComposeBar's
    // own constructor now — see ComposeBar::ComposeBar()'s text_area_ setup.
    room_text_area_->push_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            return tesseract::views::dispatch_compose_nav(
                nk, gif_controller_.get(), slash_controller_.get(),
                shortcode_controller_.get(), mention_controller_.get());
        });
    room_text_area_->set_on_edit_last(
        [this] { return room_view_ && room_view_->edit_last_own(); });

    mention_popover_ = gtk_popover_new();
    gtk_widget_set_parent(mention_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(mention_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(mention_popover_), FALSE);
    // Non-autohide so the composer keeps the keyboard (see slash popup below).
    gtk_popover_set_autohide(GTK_POPOVER(mention_popover_), FALSE);
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
        room_text_area_, shell_client_(), mention_popup_widget_,
        std::move(hooks));

    // ── /command autocomplete popup ───────────────────────────────────────
    slash_popover_ = gtk_popover_new();
    gtk_widget_set_parent(slash_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(slash_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(slash_popover_), FALSE);
    // autohide=FALSE: an autohide popover is modal and (on Wayland) takes an
    // xdg_popup keyboard grab, so the composer stops receiving keys while it is
    // open even though it keeps GTK focus. Non-autohide keeps the keyboard with
    // the composer; the controller pops it down on text change / Escape / accept
    // and nav is forwarded from the text area.
    gtk_popover_set_autohide(GTK_POPOVER(slash_popover_), FALSE);
    slash_popup_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    {
        auto w = std::make_unique<tesseract::views::SlashCommandPopup>();
        slash_popup_widget_ = w.get();
        slash_popup_surface_->set_root(std::move(w));
        gtk_popover_set_child(GTK_POPOVER(slash_popover_),
                              slash_popup_surface_->widget());
    }
    {
        tesseract::views::SlashCommandController::Hooks sh;
        sh.show = [this](tk::Rect cursor, int rows)
        { show_slash_popup_(cursor, rows); };
        sh.hide = [this]
        {
            if (slash_popover_)
                gtk_popover_popdown(GTK_POPOVER(slash_popover_));
        };
        sh.repaint = [this]
        {
            if (slash_popup_surface_)
                slash_popup_surface_->host().request_repaint();
        };
        sh.room_id = [this] { return room_id_; };
        sh.client = [this] { return shell_client_(); };
        sh.clear_composer = [this]
        {
            if (room_view_)
                room_view_->clear_compose_text();
        };
        slash_controller_ =
            std::make_unique<tesseract::views::SlashCommandController>(
                room_text_area_, slash_popup_widget_, std::move(sh));
    }

    // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────────
    shortcode_popover_ = gtk_popover_new();
    gtk_widget_set_parent(shortcode_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(shortcode_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(shortcode_popover_), FALSE);
    // Non-autohide so the composer keeps the keyboard (see slash popup above).
    gtk_popover_set_autohide(GTK_POPOVER(shortcode_popover_), FALSE);
    shortcode_popup_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    {
        auto w = std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = w.get();
        shortcode_popup_widget_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            { return shell_image_(url); });
        shortcode_popup_surface_->set_root(std::move(w));
        gtk_popover_set_child(GTK_POPOVER(shortcode_popover_),
                              shortcode_popup_surface_->widget());
    }
    {
        tesseract::views::ShortcodeController::Hooks sh;
        sh.show = [this](tk::Rect cursor, int rows)
        { show_shortcode_popup_(cursor, rows); };
        sh.hide = [this]
        {
            if (shortcode_popover_)
                gtk_popover_popdown(GTK_POPOVER(shortcode_popover_));
        };
        sh.repaint = [this]
        {
            if (shortcode_popup_surface_)
                shortcode_popup_surface_->host().request_repaint();
        };
        sh.emoticons = [this]() { return shell_emoticons_(); };
        sh.fetch_image = [this](const std::string& url)
        { shell_ensure_media_image_(url, 28, 28); };
        sh.resolve_image = [this](const std::string& url) -> const tk::Image*
        { return shell_image_(url); };
        shortcode_controller_ =
            std::make_unique<tesseract::views::ShortcodeController>(
                room_text_area_, shortcode_popup_widget_, std::move(sh));
    }

    // ── /gif inline result strip ──────────────────────────────────────────
    gif_popover_ = gtk_popover_new();
    gtk_widget_set_parent(gif_popover_, surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(gif_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(gif_popover_), FALSE);
    // Non-autohide so the composer keeps focus (nav is forwarded from the text
    // area); the controller pops it down on text change / send.
    gtk_popover_set_autohide(GTK_POPOVER(gif_popover_), FALSE);
    gif_popup_surface_ = std::make_unique<tk::gtk4::Surface>(surface_->theme());
    {
        auto w = std::make_unique<tesseract::views::GifPopup>();
        gif_popup_widget_ = w.get();
        // Strip cells render via the shell's shared two-stage provider. The
        // repaint refreshes THIS pop-out's surface and is self-guarded by the
        // window's liveness token (the shell's in-flight fetch may outlive us).
        gif_popup_widget_->set_image_provider(
            [this](const tesseract::GifResult& result) -> const tk::Image*
            {
                auto alive = alive_;
                return shell_gif_strip_image_(
                    result,
                    [this, alive]
                    {
                        if (*alive && gif_popup_surface_)
                            gif_popup_surface_->host().request_repaint();
                    });
            });
        gif_popup_surface_->set_root(std::move(w));
        gtk_popover_set_child(GTK_POPOVER(gif_popover_),
                              gif_popup_surface_->widget());
    }
    {
        tesseract::views::GifController::Hooks gh;
        gh.show = [this] { show_gif_popup_(); };
        gh.hide = [this] { hide_gif_popup_(); };
        gh.repaint = [this]
        {
            if (gif_popup_surface_)
                gif_popup_surface_->host().request_repaint();
        };
        gh.room_id = [this] { return room_id_; };
        gh.client = [this] { return shell_client_(); };
        gh.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        gh.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        gh.post_delayed = [this](int ms, std::function<void()> fn)
        {
            if (surface_)
                surface_->host().post_delayed(ms, std::move(fn));
        };
        gh.api_key = []() -> std::string
        { return tesseract::Settings::instance().gif_api_key; };
        gh.client_key = []() -> std::string { return "tesseract"; };
        gh.clear_composer = [this]
        {
            if (room_text_area_)
                room_text_area_->set_text("");
            if (room_view_)
                room_view_->clear_compose_text();
        };
        gh.get_cached_gif_bytes =
            [this](const std::string& url) -> std::vector<std::uint8_t>
        { return shell_cached_gif_bytes_(url); };
        gif_controller_ = std::make_unique<tesseract::views::GifController>(
            room_text_area_, gif_popup_widget_, std::move(gh));
    }

    surface_->set_on_layout(
        [this]
        {
            // Native child controls always paint over canvas-drawn overlays,
            // so hide them while the confirm dialog covers the window —
            // otherwise the compose box/search fields would poke through on
            // top of the modal backdrop. room_text_area_ self-positions via
            // ComposeBar::arrange() otherwise (reached via the relayout this
            // set_on_layout callback runs after), so only a force-hide is
            // needed here — mirrors the search fields' own gating below.
            const bool confirm_open =
                confirm_dialog_widget_ && confirm_dialog_widget_->is_open();
            if (confirm_open && room_text_area_)
            {
                room_text_area_->set_visible(false);
            }
            if (confirm_open && room_view_)
            {
                // Search field self-positions via RoomSearchBar::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* bar = room_view_->room_search_bar())
                    if (auto* f = bar->search_field())
                        f->set_visible(false);
            }
            if (confirm_open && forward_picker_widget_)
            {
                // Search field self-positions via ForwardRoomPicker::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
            }
        });

    // Per-room "find in conversation" — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here (on_close is already
    // wired internally by RoomView's own constructor).
    if (room_view_)
    {
        if (auto* bar = room_view_->room_search_bar())
        {
            if (auto* rif = bar->search_field())
            {
                rif->push_popup_nav(
                    [this](tk::NavKey nk) -> bool
                    {
                        if (!room_view_ || !room_view_->room_search_open())
                            return false;
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(-1);
                            return true;
                        case tk::NavKey::Down:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(+1);
                            return true;
                        case tk::NavKey::Escape:
                            room_view_->close_room_search();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
        }
    }

    // Forward-message picker — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here.
    if (forward_picker_widget_)
    {
        if (auto* fpf = forward_picker_widget_->search_field())
        {
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    if (!forward_picker_widget_ || !forward_picker_widget_->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        forward_picker_widget_->move_selection(-1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        forward_picker_widget_->move_selection(+1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        forward_picker_widget_->close();
                        return true;
                    default:
                        return false;
                    }
                });
        }
    }

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
        // Keep the message row's action buttons visible while the reaction
        // picker is open (released on the popover's "closed" signal).
        if (room_view_ && room_view_->message_list())
            room_view_->message_list()->set_hover_locked(true);
        popup_emoji_at_rect_(anchor);
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

    // Fires on every popdown (selection or autohide outside click). Clear the
    // pending reaction target and release the hover lock taken in
    // on_add_reaction_requested so the message row's action buttons hide again.
    g_signal_connect(
        emoji_popover_, "closed",
        G_CALLBACK(+[](GtkPopover*, gpointer u)
                   {
                       auto* self = static_cast<RoomWindow*>(u);
                       self->pending_reaction_event_id_.clear();
                       if (self->room_view_ && self->room_view_->message_list())
                           self->room_view_->message_list()->set_hover_locked(
                               false);
                       // Nothing else claims focus once the popover goes
                       // away — return it to the compose box.
                       if (self->room_text_area_)
                           self->room_text_area_->set_focused(true);
                   }),
        this);

    emoji_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    auto shared = std::make_unique<tesseract::views::EmojiPicker>(
        &emoji_picker_surface_->host());
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_current_room_id(room_id_);
    emoji_picker_shared_->set_current_room_parent_spaces(shell_parent_spaces_for_room_());
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
        const tk::Image* image = picker_image_provider_(false)(img.url, img.url);
        int pos = room_text_area_->cursor_byte_pos();
        room_text_area_->insert_emoticon(pos, pos, img.shortcode, img.url, image);
        if (room_view_)
            room_view_->set_current_text(room_text_area_->text());
        room_text_area_->set_focused(true);
    };
    emoji_picker_shared_->set_image_provider(picker_image_provider_(false));
    emoji_picker_surface_->set_root(std::move(shared));

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

    // Fires on every popdown (selection or autohide outside click). Nothing
    // else claims focus once the popover goes away — return it to the
    // compose box. Mirrors the emoji popover's "closed" handler above.
    g_signal_connect(
        sticker_popover_, "closed",
        G_CALLBACK(+[](GtkPopover*, gpointer u)
                   {
                       auto* self = static_cast<RoomWindow*>(u);
                       if (self->room_text_area_)
                           self->room_text_area_->set_focused(true);
                   }),
        this);

    sticker_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(surface_->theme());
    auto shared = std::make_unique<tesseract::views::StickerPicker>(
        &sticker_picker_surface_->host());
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_current_room_id(room_id_);
    sticker_picker_shared_->set_current_room_parent_spaces(shell_parent_spaces_for_room_());
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
        if (auto* sf = emoji_picker_shared_->search_field())
            sf->set_text("");
    }
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
        if (auto* sf = sticker_picker_shared_->search_field())
            sf->set_text("");
    }
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
        if (w->room_view_ && w->room_view_->room_search_open())
        {
            w->room_view_->close_room_search();
            return TRUE;
        }
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
        surface_->root()->apply_theme(t);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
        mention_popup_surface_->root()->apply_theme(t);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(t);
        emoji_picker_surface_->root()->apply_theme(t);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(t);
        sticker_picker_surface_->root()->apply_theme(t);
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

void RoomWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    if (!slash_popover_)
    {
        return;
    }
    int w = int(tesseract::views::SlashCommandPopup::kWidth);
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    gtk_widget_set_size_request(slash_popup_surface_->widget(), w, h);
    GdkRectangle r{int(cursor_local.x), int(cursor_local.y),
                   int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(slash_popover_), &r);
    gtk_popover_popup(GTK_POPOVER(slash_popover_));
}

void RoomWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    if (!shortcode_popover_)
    {
        return;
    }
    int w = int(tesseract::views::ShortcodePopup::kWidth);
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    gtk_widget_set_size_request(shortcode_popup_surface_->widget(), w, h);
    GdkRectangle r{int(cursor_local.x), int(cursor_local.y),
                   int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(shortcode_popover_), &r);
    gtk_popover_popup(GTK_POPOVER(shortcode_popover_));
}

void RoomWindow::show_gif_popup_()
{
    if (!gif_popover_ || !gif_popup_widget_ || !room_text_area_ || !surface_ ||
        !gif_popup_surface_ || !room_view_)
    {
        return;
    }
    // Full-width strip floating just above the compose bar (like the main
    // window's). content_size() drives the height + the empty/status check.
    const tk::Rect cb = room_view_->compose_bar_rect();
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    const int w = int(cb.w);
    const int h = int(sz.h);
    gtk_widget_set_size_request(gif_popup_surface_->widget(), w, h);
    GdkRectangle rect{int(cb.x), int(cb.y), int(cb.w), int(cb.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(gif_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(gif_popover_));
}

void RoomWindow::hide_gif_popup_()
{
    if (gif_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(gif_popover_));
    }
}

void RoomWindow::on_gif_results(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void RoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                      const std::string& message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, message);
    }
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
    // Advance the /gif strip's animated cells (frames come from the shared anim
    // cache; the shell's tick fires this for every window).
    if (gif_popover_ && gtk_widget_get_visible(gif_popover_) &&
        gif_popup_surface_)
    {
        gtk_widget_queue_draw(gif_popup_surface_->widget());
    }
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace gtk4
