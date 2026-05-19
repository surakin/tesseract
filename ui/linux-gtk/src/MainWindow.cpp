#include "MainWindow.h"
#include "LoginView.h"
#include "SettingsWidget.h"
#include "LinuxScreenLockGtk.h"

#include "tk/canvas_cairo.h"
#include "tk/theme.h"
#include "views/text_util.h"

#include <cairo.h>
#include <thread>

#include <tesseract/emoji.h>
#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_set>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <libintl.h>
#define _(s) gettext(s)

namespace gtk4
{

// Single GNotification id used for the "window visible but unfocused"
// attention request (GTK4 has no urgency-hint API). Reusing one id means a
// newer message replaces the previous attention banner, and the window
// becoming active withdraws it — mirroring the one-shot urgency-hint
// semantics other backends get.
namespace
{
constexpr char kAttentionNotifId[] = "tesseract-attention";
}

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// g_idle_add helpers (for async workers that are NOT part of EventHandlerBase)
// ---------------------------------------------------------------------------

struct IdlePaginateResult
{
    MainWindow* window;
    std::string room_id;
    bool reached_start;
};

struct IdleSubscribeResult
{
    MainWindow* window;
    std::string room_id;
    bool reached_start;
};

struct IdleJumpResult
{
    MainWindow* window;
    std::string room_id;
    std::string event_id;
};

struct IdleJumpError
{
    MainWindow* window;
    std::string message;
};

struct JumpDlgCtx
{
    MainWindow* self;
    GtkWidget* calendar;
    GtkWidget* dialog;
};

// ---------------------------------------------------------------------------
// EventHandlerBase UI-thread hook implementations (GTK4)
// ---------------------------------------------------------------------------

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    push_timeline_reset(std::move(room_id), std::move(snapshot));
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    push_message_inserted(std::move(room_id), index, std::move(ev));
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    push_message_updated(std::move(room_id), index, std::move(ev));
}

void MainWindow::handle_message_removed_ui_(std::string room_id,
                                            std::size_t index)
{
    push_message_removed(std::move(room_id), index);
}

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                       std::string description,
                                       bool soft_logout)
{
    if (context == "sync_reconnect")
    {
        handle_reconnect(user_id);
    }
    else if (context == "sync_auth_error")
    {
        handle_auth_error(soft_logout);
    }
    else
    {
        push_error(std::move(description));
    }
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    push_backup_progress(std::move(progress));
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id, std::string room_name,
    std::string sender, std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }

    if (!notification_image_allowed_())
    {
        image_bytes.clear();
    }
    push_notification(user_id, room_id, room_name, sender, body, is_mention,
                      std::move(avatar_bytes), std::move(image_bytes));
}

void MainWindow::handle_voice_waveform_ready_ui_(
    std::string room_id, std::string event_id,
    std::vector<std::uint16_t> waveform)
{
    if (room_id != current_room_id_)
        return;
    if (auto* ml = room_view_->message_list())
        ml->update_voice_waveform(event_id, std::move(waveform));
}

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status();
}

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (room_view_)
    {
        room_view_->set_typing_text(text);
    }
}

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    if (is_verified)
    {
        main_app_->show_verif_banner(false);
        main_app_surface_->relayout();
        return;
    }
    if (verification_banner_dismissed_)
    {
        return;
    }
    if (!main_app_->verif_banner()->visible())
    {
        active_verification_flow_id_.clear();
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Prompt);
        if (recovery_shared_ && main_app_->recovery_banner()->visible())
        {
            auto rs = recovery_shared_->state();
            if (rs == tesseract::views::RecoveryBanner::State::Form ||
                rs == tesseract::views::RecoveryBanner::State::Failed)
            {
                main_app_->show_recovery_banner(false);
            }
            else
            {
                return;
            }
        }
        main_app_->show_verif_banner(true);
        main_app_surface_->relayout();
    }
}

void MainWindow::handle_verification_request_ui_(std::string flow_id,
                                                 std::string /*user_id*/,
                                                 std::string /*device_id*/,
                                                 bool incoming)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    active_verification_flow_id_ = flow_id;
    if (incoming)
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    }
    else
    {
        verif_shared_->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_)
        {
            client_->start_sas(flow_id);
        }
    }
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_emojis(emojis);
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(tesseract::views::VerificationBanner::State::Done);
    main_app_surface_->relayout();
    // Hide after 1.5 s. The payload carries a liveness weak_ptr so a
    // window destroyed within that window doesn't get called on freed `this`.
    struct DoneData
    {
        MainWindow* w;
        std::weak_ptr<bool> alive;
    };
    auto* dd = new DoneData{this, alive_};
    g_timeout_add(
        1500,
        [](gpointer data) -> gboolean
        {
            auto* d = static_cast<DoneData*>(data);
            if (!d->alive.expired())
            {
                auto* self = d->w;
                if (self->verif_shared_ && self->verif_shared_->on_done)
                {
                    self->verif_shared_->on_done();
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        },
        dd);
}

void MainWindow::handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                   std::string reason)
{
    if (!main_app_ || !verif_shared_)
    {
        return;
    }
    verif_shared_->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    verif_shared_->set_cancel_reason(std::move(reason));
    main_app_->show_verif_banner(true);
    main_app_surface_->relayout();
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow(GtkApplication* app) : app_(app)
{
    set_screen_lock_(std::make_unique<LinuxScreenLockGtk>());

    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Tesseract");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 768);

#ifdef TESSERACT_ICON_SEARCH_PATH
    gtk_icon_theme_add_search_path(
        gtk_icon_theme_get_for_display(gtk_widget_get_display(window_)),
        TESSERACT_ICON_SEARCH_PATH);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "tesseract");
#endif

    g_object_set_data(G_OBJECT(window_), "cpp_window", this);

    // ---- CSS ----
    theme_css_provider_ = gtk_css_provider_new();
    gtk_css_provider_load_from_string(theme_css_provider_, R"css(
        .sidebar {
            background-color: #F0F2F5;
        }
        .sidebar-separator {
            background-color: #D0D3D8;
            min-width: 1px;
        }
        .message-body {
            padding: 2px 0px;
        }
        .sender-name {
            font-weight: bold;
            font-size: 11px;
            color: #555555;
        }
        .timestamp {
            font-size: 9px;
            color: rgba(0,0,0,0.45);
        }
        .avatar-initial {
            background-color: #8E8E93;
            color: white;
            font-weight: bold;
            font-size: 15px;
            border-radius: 16px;
            min-width: 32px;
            min-height: 32px;
            padding: 0;
        }
        .unread-badge {
            background-color: #0084FF;
            color: white;
            border-radius: 10px;
            padding: 0px 6px;
            font-size: 10px;
            font-weight: bold;
        }
        .room-header {
            background-color: white;
            border-bottom: 1px solid #D0D3D8;
        }
        .room-header-name {
            font-weight: bold;
            font-size: 14px;
        }
        .room-header-topic {
            font-size: 11px;
            color: #65676B;
        }
    )css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(theme_css_provider_),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ---- Layout ----
    content_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack_),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_window_set_child(GTK_WINDOW(window_), content_stack_);

    login_view_ = std::make_unique<LoginView>();
    login_view_->set_on_success(
        [this]()
        {
            on_login_succeeded();
        });
    login_view_->set_on_cancel(
        [this]()
        {
            on_login_cancelled();
        });
    gtk_stack_add_named(GTK_STACK(content_stack_), login_view_->widget(),
                        "login");

    // Single surface hosting the full main-app widget tree.
    main_app_surface_ = std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
    {
        auto main_app_owner =
            std::make_unique<tesseract::views::MainAppWidget>();
        main_app_ = main_app_owner.get();
        room_list_view_ = main_app_->room_list_view();
        room_view_ = main_app_->room_view();
        recovery_shared_ = main_app_->recovery_banner();
        verif_shared_ = main_app_->verif_banner();
        img_viewer_ = main_app_->image_viewer();
        vid_viewer_ = main_app_->video_viewer();

        // Wire TabBar callbacks.
        main_app_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            tab_select_room(room_id);
        };
        main_app_->tab_bar()->on_tab_closed = [this](const std::string& room_id)
        {
            tab_close(room_id);
        };

        // Wire UserInfo callbacks (replaces native GTK user-strip gestures).
        main_app_->user_info()->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        main_app_->user_info()->on_primary = [this](tk::Point world)
        {
            open_account_picker(world.x, world.y);
        };
        main_app_->user_info()->on_secondary = [this](tk::Point world)
        {
            if (!user_popover_)
            {
                return;
            }
            GdkRectangle r = {static_cast<int>(world.x),
                              static_cast<int>(world.y), 1, 1};
            gtk_popover_set_pointing_to(GTK_POPOVER(user_popover_), &r);
            gtk_popover_popup(GTK_POPOVER(user_popover_));
        };

        // Space nav back button.
        main_app_->on_space_back = [this]
        {
            if (!space_stack_.empty())
            {
                space_stack_.pop_back();
            }
            refresh_room_list();
        };

        // Wire space nav and RoomListView avatar providers.
        main_app_->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        room_list_view_->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        room_list_view_->set_sticker_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                if (const auto* f = anim_cache_.current_frame(mxc))
                {
                    return f;
                }
                auto it = tk_images_.find(mxc);
                if (it != tk_images_.end())
                {
                    return it->second.get();
                }
                ensure_media_image_(mxc, 64, 64);
                return nullptr;
            });
        room_list_view_->on_room_selected = [this](const std::string& room_id)
        {
            tab_select_room(room_id);
        };
        room_list_view_->on_scroll = [this]
        {
            if (scroll_debounce_id_)
            {
                g_source_remove(scroll_debounce_id_);
                scroll_debounce_id_ = 0;
            }
            scroll_debounce_id_ = g_timeout_add(
                300,
                [](gpointer ud) -> gboolean
                {
                    auto* self = static_cast<MainWindow*>(ud);
                    self->scroll_debounce_id_ = 0;
                    if (!self->room_list_view_ || !self->client_)
                    {
                        return G_SOURCE_REMOVE;
                    }
                    auto ids = self->room_list_view_->visible_room_ids();
                    self->client_->stop_background_backfill();
                    self->client_->start_background_backfill(ids);
                    return G_SOURCE_REMOVE;
                },
                this);
        };
        room_list_view_->on_search_clear = [this]
        {
            if (search_debounce_id_)
            {
                g_source_remove(search_debounce_id_);
                search_debounce_id_ = 0;
            }
            search_pending_text_.clear();
            if (room_search_field_)
            {
                room_search_field_->set_text("");
            }
            room_list_view_->set_search_text("");
            refresh_room_list();
        };
        room_list_view_->on_join_room_requested = [this]
        {
            open_join_room_dialog();
        };

        // Wire RoomView callbacks.
        room_view_->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        room_view_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                if (const auto* f = anim_cache_.current_frame(mxc))
                {
                    return f;
                }
                auto it = tk_images_.find(mxc);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        room_view_->set_preview_provider(
            [this](const std::string& url)
                -> const tesseract::views::UrlPreviewData*
            {
                auto it = url_preview_data_.find(url);
                if (it == url_preview_data_.end())
                {
                    return nullptr;
                }
                if (!it->second.image_mxc.empty() &&
                    !tk_images_.count(it->second.image_mxc) &&
                    !anim_cache_.has(it->second.image_mxc))
                {
                    ensure_media_image_(it->second.image_mxc, 64, 64);
                }
                return &it->second;
            });
        if (auto player = main_app_surface_->host().make_audio_player())
        {
            room_view_->set_audio_player(std::move(player));
        }
        capture_ = main_app_surface_->host().make_audio_capture();
        {
            tk::gtk4::Surface* sfp = main_app_surface_.get();
            wire_voice_capture_(
                room_view_,
                [sfp]() { gtk_widget_queue_draw(sfp->widget()); },
                [this]() { return current_room_id_; },
                [this]() { room_view_->set_current_text({}); });
        }
        room_view_->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t>
            {
                return client_->fetch_source_bytes(source_json);
            });
        {
            tk::gtk4::Surface* sfp = main_app_surface_.get();
            room_view_->set_repaint_requester(
                [sfp]()
                {
                    if (sfp)
                    {
                        gtk_widget_queue_draw(sfp->widget());
                    }
                });
            room_view_->set_post_delayed(
                [sfp](int ms, std::function<void()> fn)
                {
                    if (sfp)
                    {
                        sfp->host().post_delayed(ms, std::move(fn));
                    }
                });
        }

        // Compose text area overlay.
        room_text_area_ = main_app_surface_->host().make_text_area();
        room_text_area_->set_placeholder(_("Message\xe2\x80\xa6"));
        room_text_area_->set_on_changed(
            [this](const std::string& s)
            {
                handle_compose_text_changed_(s);
                room_view_->set_current_text(s);

                // ── Shortcode detection ─────────────────────────────────────────
                int cursor = (int)s.size();

                auto complete = shortcode_engine_.find_complete(s, cursor);
                if (complete)
                {
                    auto hits = shortcode_engine_.lookup(complete->prefix,
                                                         cached_emoticons_, 1);
                    std::string r =
                        (!hits.empty() && !hits.front().glyph.empty())
                            ? hits.front().glyph
                            : ":" + complete->prefix + ":";
                    room_text_area_->replace_range(complete->start,
                                                   complete->end, r);
                    hide_shortcode_popup_();
                    return;
                }

                auto prefix_match = shortcode_engine_.find_prefix(s, cursor);
                if (prefix_match && prefix_match->prefix.size() >= 2)
                {
                    shortcode_current_suggestions_ = shortcode_engine_.lookup(
                        prefix_match->prefix, cached_emoticons_);
                    if (!shortcode_current_suggestions_.empty())
                    {
                        shortcode_active_match_ = *prefix_match;
                        for (const auto& sugg : shortcode_current_suggestions_)
                        {
                            if (!sugg.emoticon.url.empty())
                            {
                                ensure_media_image_(sugg.emoticon.url, 28, 28);
                            }
                        }
                        bool was_visible = shortcode_popup_visible_();
                        show_shortcode_popup_(shortcode_current_suggestions_,
                                              room_text_area_->cursor_rect());
                        if (!was_visible)
                        {
                            room_text_area_->set_on_popup_nav(
                                [this](tk::NativeTextArea::NavKey nk) -> bool
                                {
                                    if (!shortcode_popup_visible_())
                                    {
                                        return false;
                                    }
                                    int cur = shortcode_popup_widget_
                                                  ->selected_index();
                                    int n =
                                        shortcode_popup_widget_->visible_rows();
                                    if (n <= 0)
                                    {
                                        return true;
                                    }
                                    int next = cur;
                                    switch (nk)
                                    {
                                    case tk::NativeTextArea::NavKey::Up:
                                        next = std::max(0, cur - 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Down:
                                        next = std::min(n - 1, cur + 1);
                                        break;
                                    case tk::NativeTextArea::NavKey::Tab:
                                    {
                                        int sel = shortcode_popup_widget_
                                                      ->selected_index();
                                        if (sel >= 0 &&
                                            sel <
                                                (int)
                                                    shortcode_current_suggestions_
                                                        .size())
                                        {
                                            auto& s =
                                                shortcode_current_suggestions_
                                                    [sel];
                                            std::string r =
                                                s.glyph.empty()
                                                    ? ":" + s.shortcode + ":"
                                                    : s.glyph;
                                            room_text_area_->replace_range(
                                                shortcode_active_match_.start,
                                                shortcode_active_match_.end, r);
                                        }
                                        hide_shortcode_popup_();
                                        return true;
                                    }
                                    case tk::NativeTextArea::NavKey::ShiftTab:
                                        return false;
                                    case tk::NativeTextArea::NavKey::Escape:
                                        hide_shortcode_popup_();
                                        return true;
                                    }
                                    shortcode_popup_widget_->set_selected_index(
                                        next);
                                    shortcode_popup_surface_->host()
                                        .request_repaint();
                                    return true;
                                });
                        }
                        return;
                    }
                }
                hide_shortcode_popup_();
                // ── End shortcode detection ─────────────────────────────────────
            });
        room_text_area_->set_on_submit(
            [this]
            {
                if (shortcode_popup_visible_())
                {
                    int sel = shortcode_popup_widget_->selected_index();
                    if (sel >= 0 &&
                        sel < (int)shortcode_current_suggestions_.size())
                    {
                        auto& s = shortcode_current_suggestions_[sel];
                        std::string r =
                            s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
                        room_text_area_->replace_range(
                            shortcode_active_match_.start,
                            shortcode_active_match_.end, r);
                        hide_shortcode_popup_();
                        return;
                    }
                    hide_shortcode_popup_();
                }
                on_send_clicked();
            });
        room_text_area_->set_on_edit_last(
            [this]
            {
                return room_view_ && room_view_->edit_last_own();
            });
        room_text_area_->set_on_height_changed(
            [this](float h)
            {
                room_view_->set_text_area_natural_height(h);
                main_app_surface_->relayout();
            });
        room_text_area_->set_on_image_paste(
            [this](std::vector<std::uint8_t> bytes, std::string mime)
            {
                if (room_view_)
                {
                    room_view_->compose_bar()->set_pending_image(
                        std::move(bytes), std::move(mime));
                }
            });

        // File drop.
        auto on_file_drop = [this](std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename)
        {
            if (!room_view_)
            {
                return;
            }
            const auto limit = client_->media_upload_limit();
            if (limit > 0 && bytes.size() > limit)
            {
                if (status_bar_)
                {
                    std::string msg =
                        std::string(_("File exceeds server limit (")) +
                        tesseract::views::format_size(limit) + ")";
                    gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
                }
                return;
            }
            if (mime.rfind("image/", 0) == 0)
            {
                room_view_->compose_bar()->set_pending_image(
                    std::move(bytes), std::move(mime), std::move(filename));
            }
            else
            {
                room_view_->compose_bar()->set_pending_file(
                    std::move(bytes), std::move(mime), std::move(filename));
            }
        };
        main_app_surface_->set_on_file_drop(on_file_drop);

        room_view_->on_layout_changed = [this]
        {
            main_app_surface_->relayout();
        };

        room_view_->on_send = [this](const std::string& body)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string trimmed = tesseract::text::trim(body);
            if (trimmed.empty())
            {
                return;
            }
            auto res = client_->send_message(current_room_id_, trimmed);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
        };
        room_view_->on_send_reply =
            [this](const std::string& reply_event_id, const std::string& body)
        {
            if (body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res =
                client_->send_reply(current_room_id_, reply_event_id, body);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send reply failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_edit =
            [this](const std::string& event_id, const std::string& new_body)
        {
            if (new_body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_edit(current_room_id_, event_id, new_body);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg = std::string(_("Edit failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_send_image =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int /*src_w*/,
                   int /*src_h*/, std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            const bool compress =
                tesseract::Settings::instance().image_quality ==
                tesseract::Settings::ImageQuality::Compressed;
            auto enc = main_app_surface_->host().encode_for_send(
                bytes.data(), bytes.size(), compress);
            if (enc.bytes.empty())
            {
                return;
            }
            std::string out_name = filename;
            if (enc.mime == "image/jpeg")
            {
                auto dot = out_name.find_last_of('.');
                if (dot != std::string::npos)
                {
                    out_name = out_name.substr(0, dot);
                }
                out_name += ".jpg";
            }
            auto res = client_->send_image(
                current_room_id_, enc.bytes, enc.mime, out_name, caption,
                enc.width, enc.height, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
        };
        room_view_->on_send_file =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::string reply_event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_file(current_room_id_, bytes, mime,
                                          filename, caption, reply_event_id);
            if (res)
            {
                if (room_text_area_)
                {
                    room_text_area_->set_text("");
                }
                room_view_->clear_compose_text();
            }
            else if (status_bar_)
            {
                std::string msg =
                    std::string(_("Send file failed: ")) + res.message;
                gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
            }
        };
        room_view_->on_edit_cancelled = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_text("");
            }
            room_view_->clear_compose_text();
        };
        room_view_->on_reply_focus = [this]
        {
            if (room_text_area_)
            {
                room_text_area_->set_focused(true);
            }
        };
        room_view_->on_edit_prefill = [this](const std::string& body)
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(body);
                room_view_->set_current_text(body);
                room_text_area_->set_focused(true);
            }
        };
        room_view_->on_delete_requested = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->redact_event(current_room_id_, event_id);
        };
        room_view_->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->send_reaction(current_room_id_, event_id, key);
        };
        room_view_->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor)
        {
            if (!emoji_popover_ || current_room_id_.empty())
            {
                return;
            }
            pending_reaction_event_id_ = event_id;
            popup_emoji_at_rect(main_app_surface_->widget(), anchor);
        };
        room_view_->on_link_clicked = [](const std::string& url)
        {
            tesseract::Client::open_in_browser(url);
        };
        room_view_->on_link_hovered = [this](const std::string& url)
        {
            GtkWidget* w = main_app_surface_->widget();
            gtk_widget_set_cursor_from_name(w, url.empty() ? "default"
                                                           : "pointer");
        };
        room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
        {
            GtkWidget* w = main_app_surface_->widget();
            if (!topic_tooltip_popover_)
            {
                topic_tooltip_label_ = gtk_label_new(nullptr);
                gtk_label_set_wrap(GTK_LABEL(topic_tooltip_label_), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(topic_tooltip_label_),
                                              60);
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
            gtk_popover_set_pointing_to(GTK_POPOVER(topic_tooltip_popover_),
                                        &rect);
            gtk_popover_popup(GTK_POPOVER(topic_tooltip_popover_));
        };
        room_view_->on_hide_tooltip = [this]
        {
            if (topic_tooltip_popover_)
            {
                gtk_popover_popdown(GTK_POPOVER(topic_tooltip_popover_));
            }
        };
        room_view_->on_receipt_needed = [this](const std::string& eid)
        {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        room_view_->message_list()->on_tile_needed = [this](int z, int x, int y)
        {
            ensure_tile_async(z, x, y);
        };
        room_view_->on_near_top = [this]
        {
            if (current_room_id_.empty())
            {
                return;
            }
            request_more_history(current_room_id_);
        };
        room_view_->on_near_bottom = [this]
        {
            if (!current_room_id_.empty())
            {
                request_forward_history_(current_room_id_);
            }
        };
        room_view_->on_return_to_live = [this]
        {
            if (!current_room_id_.empty())
            {
                return_to_live_(current_room_id_);
            }
        };
        room_view_->on_scroll_to_original = [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string room = current_room_id_;
            begin_focused_subscription_(room, event_id);
            run_async_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        room_view_->on_jump_to_date_requested = [this]
        {
            open_jump_to_date_dialog();
        };
        room_view_->on_emoji = [this](tk::Rect btn)
        {
            if (!emoji_popover_)
            {
                return;
            }
            if (gtk_widget_get_visible(emoji_popover_))
            {
                gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
            }
            else
            {
                popup_emoji_at_rect(main_app_surface_->widget(), btn);
            }
        };
        room_view_->on_sticker = [this](tk::Rect btn)
        {
            if (!sticker_popover_)
            {
                return;
            }
            if (gtk_widget_get_visible(sticker_popover_))
            {
                gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
            }
            else
            {
                popup_sticker_at_rect(main_app_surface_->widget(), btn);
            }
        };

        // Image viewer.
        img_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                if (const auto* f = anim_cache_.current_frame(url))
                {
                    return f;
                }
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        img_viewer_->on_close = [this]
        {
            main_app_->show_image_viewer(false);
            main_app_surface_->relayout();
        };

        room_view_->on_image_clicked =
            [this](const tesseract::views::MessageListView::ImageHit& hit)
        {
            img_viewer_->open(hit.media_url, hit.body, hit.natural_w,
                              hit.natural_h);
            main_app_->show_image_viewer(true);
            main_app_surface_->relayout();
            gtk_widget_grab_focus(main_app_surface_->widget());
        };

        img_viewer_->set_repaint_requester(
            [this]
            {
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            });
        img_viewer_->on_save =
            [this](std::string source_url, std::string filename_hint)
        {
            std::string suggested = filename_hint.empty() ? "image" : filename_hint;
            GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
                "Save image",
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggested.c_str());
            struct ImgSaveCtx
            {
                MainWindow* self;
                std::string source_url;
            };
            auto* ctx = new ImgSaveCtx{this, std::move(source_url)};
            g_signal_connect(
                dlg, "response",
                G_CALLBACK(+[](GtkNativeDialog* d, gint response, gpointer p)
                {
                    auto* c = static_cast<ImgSaveCtx*>(p);
                    if (response == GTK_RESPONSE_ACCEPT)
                    {
                        GFile* gf =
                            gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string url = std::move(c->source_url);
                        c->self->run_async_(
                            [self = c->self, url = std::move(url), dest]()
                            {
                                auto bytes = self->client_->fetch_source_bytes(url);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
                    }
                    delete c;
                    gtk_native_dialog_destroy(d);
                }),
                ctx);
            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
        };

        // Video viewer.
        vid_viewer_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        vid_viewer_->set_video_player(
            main_app_surface_->host().make_video_player());
        vid_viewer_->set_repaint_requester(
            [this]
            {
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            });
        vid_viewer_->on_close = [this]
        {
            main_app_->show_video_viewer(false);
            main_app_surface_->relayout();
        };

        room_view_->on_video_clicked =
            [this](const tesseract::views::MessageListView::VideoHit& hit)
        {
            vid_viewer_->open(hit.source_json, hit.thumbnail_url, hit.mime_type,
                              hit.duration_ms, hit.natural_w, hit.natural_h,
                              hit.autoplay, hit.loop, hit.no_audio,
                              hit.hide_controls);
            main_app_->show_video_viewer(true);
            main_app_surface_->relayout();
            gtk_widget_grab_focus(main_app_surface_->widget());
            std::string src = hit.source_json;
            run_async_(
                [this, src = std::move(src)]() mutable
                {
                    auto bytes = client_->fetch_source_bytes(src);
                    struct Ctx
                    {
                        MainWindow* self;
                        std::vector<uint8_t> bytes;
                    };
                    auto* ctx = new Ctx{this, std::move(bytes)};
                    g_idle_add(
                        [](gpointer p) -> gboolean
                        {
                            auto* c = static_cast<Ctx*>(p);
                            if (c->self->vid_viewer_)
                            {
                                c->self->vid_viewer_->load_bytes(
                                    c->bytes.data(), c->bytes.size());
                            }
                            delete c;
                            return G_SOURCE_REMOVE;
                        },
                        ctx);
                });
        };

        vid_viewer_->on_save =
            [this](std::string source_json, std::string mime_type)
        {
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
                "Save video",
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                              ("video" + ext).c_str());
            struct VidSaveCtx
            {
                MainWindow* self;
                std::string source_json;
            };
            auto* ctx = new VidSaveCtx{this, std::move(source_json)};
            g_signal_connect(
                dlg, "response",
                G_CALLBACK(+[](GtkNativeDialog* d, gint response, gpointer p)
                {
                    auto* c = static_cast<VidSaveCtx*>(p);
                    if (response == GTK_RESPONSE_ACCEPT)
                    {
                        GFile* gf =
                            gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string src = std::move(c->source_json);
                        c->self->run_async_(
                            [self = c->self, src = std::move(src), dest]()
                            {
                                auto bytes = self->client_->fetch_source_bytes(src);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
                    }
                    delete c;
                    gtk_native_dialog_destroy(d);
                }),
                ctx);
            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
        };

        room_view_->on_file_clicked =
            [this](const tesseract::views::MessageListView::FileHit& hit)
        {
            std::string suggested =
                hit.file_name.empty() ? "download" : hit.file_name;
            GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
                "Save file",
                GTK_WINDOW(gtk_widget_get_root(main_app_surface_->widget())),
                GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                              suggested.c_str());
            struct FileSaveCtx
            {
                MainWindow* self;
                std::string media_url;
            };
            auto* ctx = new FileSaveCtx{this, hit.media_url};
            g_signal_connect(
                dlg, "response",
                G_CALLBACK(+[](GtkNativeDialog* d, gint response, gpointer p)
                {
                    auto* c = static_cast<FileSaveCtx*>(p);
                    if (response == GTK_RESPONSE_ACCEPT)
                    {
                        GFile* gf =
                            gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
                        char* cpath = g_file_get_path(gf);
                        std::string dest(cpath);
                        g_free(cpath);
                        g_object_unref(gf);
                        std::string url = std::move(c->media_url);
                        c->self->run_async_(
                            [self = c->self, url = std::move(url), dest]()
                            {
                                auto bytes = self->client_->fetch_media_bytes(url);
                                struct WriteCtx
                                {
                                    std::string dest;
                                    std::vector<uint8_t> bytes;
                                };
                                auto* wc = new WriteCtx{dest, std::move(bytes)};
                                g_idle_add(
                                    [](gpointer wp) -> gboolean
                                    {
                                        auto* w = static_cast<WriteCtx*>(wp);
                                        if (!w->bytes.empty())
                                        {
                                            std::ofstream f(w->dest,
                                                            std::ios::binary);
                                            f.write(
                                                reinterpret_cast<const char*>(
                                                    w->bytes.data()),
                                                static_cast<std::streamsize>(
                                                    w->bytes.size()));
                                        }
                                        delete w;
                                        return G_SOURCE_REMOVE;
                                    },
                                    wc);
                            });
                    }
                    delete c;
                    gtk_native_dialog_destroy(d);
                }),
                ctx);
            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dlg));
        };

        room_view_->set_video_player_factory(
            [this]()
            {
                return main_app_surface_->host().make_video_player();
            });
        room_view_->set_video_fetch_provider(
            [this](const std::string& src,
                   std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                run_async_(
                    [this, src, on_ready = std::move(on_ready)]() mutable
                    {
                        auto bytes = client_->fetch_source_bytes(src);
                        struct Ctx
                        {
                            std::function<void(std::vector<std::uint8_t>)> cb;
                            std::vector<std::uint8_t> bytes;
                        };
                        auto* ctx =
                            new Ctx{std::move(on_ready), std::move(bytes)};
                        g_idle_add(
                            [](gpointer p) -> gboolean
                            {
                                auto* c = static_cast<Ctx*>(p);
                                c->cb(std::move(c->bytes));
                                delete c;
                                return G_SOURCE_REMOVE;
                            },
                            ctx);
                    });
            });

        // Recovery banner callbacks.
        recovery_shared_->on_verify = [this](const std::string& /*key*/)
        {
            on_recovery_verify_clicked_(nullptr, this);
        };
        recovery_shared_->on_dismiss = [this]
        {
            on_recovery_dismiss_clicked_(nullptr, this);
        };

        // Verification banner callbacks.
        verif_shared_->on_verify = [this]
        {
            if (client_)
            {
                client_->request_self_verification();
            }
        };
        verif_shared_->on_accept = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_match = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                if (verif_shared_)
                {
                    verif_shared_->set_state(
                        tesseract::views::VerificationBanner::State::
                            Confirming);
                }
                main_app_surface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        verif_shared_->on_mismatch = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_cancel = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        verif_shared_->on_dismiss = [this]
        {
            verification_banner_dismissed_ = true;
            main_app_->show_verif_banner(false);
            main_app_surface_->relayout();
        };
        verif_shared_->on_done = [this]
        {
            main_app_->show_verif_banner(false);
            main_app_surface_->relayout();
        };
        verif_shared_->on_use_recovery_key = [this]
        {
            main_app_->show_verif_banner(false);
            main_app_surface_->relayout();
            maybe_show_recovery_banner();
        };

        // Room search field overlay.
        room_search_field_ = main_app_surface_->host().make_text_field();
        room_search_field_->set_placeholder("Search");
        room_search_field_->set_visible(false);
        room_search_field_->set_on_changed(
            [this](const std::string& q)
            {
                search_pending_text_ = q;
                if (search_debounce_id_)
                {
                    g_source_remove(search_debounce_id_);
                    search_debounce_id_ = 0;
                }
                search_debounce_id_ = g_timeout_add(
                    500,
                    [](gpointer ud) -> gboolean
                    {
                        auto* self = static_cast<MainWindow*>(ud);
                        self->search_debounce_id_ = 0;
                        if (self->room_list_view_)
                        {
                            self->room_list_view_->set_search_text(
                                self->search_pending_text_);
                        }
                        self->refresh_room_list();
                        return G_SOURCE_REMOVE;
                    },
                    this);
            });

        // Recovery key field overlay.
        recovery_key_field_ = main_app_surface_->host().make_text_field();
        recovery_key_field_->set_placeholder(_("Recovery key or passphrase"));
        recovery_key_field_->set_password(true);
        recovery_key_field_->set_on_changed(
            [this](const std::string& k)
            {
                if (recovery_shared_)
                {
                    recovery_shared_->set_current_key(k);
                }
            });
        recovery_key_field_->set_on_submit(
            [this]
            {
                on_recovery_verify_clicked_(nullptr, this);
            });

        // Unified layout callback — positions all 3 native overlays.
        main_app_surface_->set_on_layout(
            [this]
            {
                if (!main_app_)
                {
                    return;
                }

                bool search_visible = main_app_->room_search_field_visible();
                if (room_search_field_)
                {
                    room_search_field_->set_visible(search_visible);
                    if (search_visible)
                    {
                        room_search_field_->set_rect(
                            main_app_->room_search_field_rect());
                    }
                }

                if (room_text_area_)
                {
                    const tk::Rect ta = main_app_->compose_text_area_rect();
                    room_text_area_->set_visible(!ta.empty());
                    if (!ta.empty())
                        room_text_area_->set_rect(ta);
                }

                if (recovery_key_field_)
                {
                    bool rec_visible = main_app_->recovery_key_field_visible();
                    recovery_key_field_->set_visible(rec_visible);
                    if (rec_visible)
                    {
                        recovery_key_field_->set_rect(
                            main_app_->recovery_key_field_rect());
                    }
                }
            });

        main_app_surface_->set_root(std::move(main_app_owner));
    }

    // User context menu (right-click on user strip) — parented to main_app_surface_
    // so it can float anywhere in the window. Position is set via pointing_to in
    // UserInfo::on_secondary.
    {
        GMenu* top = g_menu_new();

        GMenu* main_section = g_menu_new();
        g_menu_append(main_section, _("Settings\xe2\x80\xa6"), "user.settings");
        g_menu_append(main_section, _("Add Account\xe2\x80\xa6"),
                      "user.add_account");
        g_menu_append(main_section, _("Log Out"), "user.logout");
        g_menu_append_section(top, nullptr, G_MENU_MODEL(main_section));
        g_object_unref(main_section);

        GMenu* quit_section = g_menu_new();
        g_menu_append(quit_section, _("Quit"), "user.quit");
        g_menu_append_section(top, nullptr, G_MENU_MODEL(quit_section));
        g_object_unref(quit_section);

        user_popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(top));
        gtk_widget_set_parent(user_popover_, main_app_surface_->widget());
        gtk_popover_set_has_arrow(GTK_POPOVER(user_popover_), FALSE);
        g_object_unref(top);

        GSimpleActionGroup* group = g_simple_action_group_new();
        {
            GSimpleAction* act = g_simple_action_new("settings", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_settings_activate_),
                             this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("add_account", nullptr);
            g_signal_connect(act, "activate",
                             G_CALLBACK(on_add_account_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("logout", nullptr);
            g_signal_connect(act, "activate", G_CALLBACK(on_logout_activate_),
                             this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        {
            GSimpleAction* act = g_simple_action_new("quit", nullptr);
            g_signal_connect(act, "activate",
                             G_CALLBACK(on_quit_user_activate_), this);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);
        }
        gtk_widget_insert_action_group(main_app_surface_->widget(), "user",
                                       G_ACTION_GROUP(group));
        g_object_unref(group);
    }

    // Right-click on the chat area: hit-test sticker rects.
    {
        GtkGesture* gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed", G_CALLBACK(on_msg_right_click_),
                         this);
        gtk_widget_add_controller(main_app_surface_->widget(),
                                  GTK_EVENT_CONTROLLER(gesture));
    }

    GtkWidget* main_widget = main_app_surface_->widget();
    gtk_widget_set_hexpand(main_widget, TRUE);
    gtk_widget_set_vexpand(main_widget, TRUE);
    gtk_stack_add_named(GTK_STACK(content_stack_), main_widget, "main");

    // Settings page — populated on each open via open_settings_().
    {
        settings_widget_ = std::make_unique<gtk4::SettingsWidget>();
        GtkWidget* w = settings_widget_->widget();
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_vexpand(w, TRUE);
        gtk_stack_add_named(GTK_STACK(content_stack_), w, "settings");

        settings_widget_->on_close = [this]
        {
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
        };
        settings_widget_->on_theme_changed =
            [this](tesseract::Settings::ThemePreference pref)
        {
            set_theme_preference_(pref);
        };
        settings_widget_->on_notifications_changed = [this](bool enabled)
        {
            tesseract::Settings::instance().notifications_enabled = enabled;
            tesseract::Settings::instance().save_to_disk(
                tesseract::config_dir());
        };
    }

    // Escape key: close viewer overlays. Attached to the window so it fires
    // regardless of which widget holds focus.
    {
        GtkEventController* key_ctl = gtk_event_controller_key_new();
        g_signal_connect(key_ctl, "key-pressed",
                         G_CALLBACK(on_window_key_pressed_), this);
        gtk_widget_add_controller(window_, key_ctl);
    }

    // Status bar floats below the main stack (outside the stack so it is
    // always visible regardless of which page is shown).
    status_bar_ = gtk_label_new(_("Not logged in"));
    gtk_widget_set_halign(status_bar_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_bar_, 4);
    gtk_widget_set_margin_bottom(status_bar_, 2);
    {
        // Wrap content_stack_ + status_bar_ in an outer vbox so the status
        // bar stays below the stack on all pages.
        GtkWidget* outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        // Reparent: the constructor already set content_stack_ as child of
        // window_; swap it out for the outer vbox.
        g_object_ref(content_stack_);
        gtk_window_set_child(GTK_WINDOW(window_), nullptr);
        gtk_box_append(GTK_BOX(outer_vbox), content_stack_);
        g_object_unref(content_stack_);
        gtk_box_append(GTK_BOX(outer_vbox), status_bar_);
        gtk_window_set_child(GTK_WINDOW(window_), outer_vbox);
    }

    gtk_widget_set_visible(window_, TRUE);

    // Notifiers are created per-account in do_login / on_login_succeeded.

    g_signal_connect(window_, "close-request",
                     G_CALLBACK(&MainWindow::on_window_close_request_), this);

    // GTK4 has no gtk_window_set_urgency_hint (removed, not deprecated), so
    // the "visible but unfocused" attention request is delivered as a
    // GNotification instead (see handle_notification). Withdraw it when the
    // user brings the window to the front, mirroring the urgency-hint clear
    // other backends do.
    g_signal_connect(window_, "notify::is-active",
                     G_CALLBACK(+[](GtkWindow* w, GParamSpec*, gpointer data)
                                {
                                    auto* self = static_cast<MainWindow*>(data);
                                    if (gtk_window_is_active(w) && self->app_)
                                    {
                                        g_application_withdraw_notification(
                                            G_APPLICATION(self->app_),
                                            kAttentionNotifId);
                                    }
                                }),
                     this);

    // Load saved theme preference and apply it.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());
    apply_current_theme_();

    // Re-apply when the OS dark-mode setting changes (System mode only).
    g_signal_connect(
        gtk_settings_get_default(), "notify::gtk-application-prefer-dark-theme",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data)
                   {
                       auto* self = static_cast<MainWindow*>(data);
                       if (tesseract::Settings::instance().theme_pref ==
                           tesseract::Settings::ThemePreference::System)
                       {
                           self->apply_current_theme_();
                       }
                   }),
        this);

    g_idle_add(
        [](gpointer data) -> gboolean
        {
            static_cast<MainWindow*>(data)->do_login();
            return G_SOURCE_REMOVE;
        },
        this);
}

void MainWindow::start_tray_if_needed_()
{
    if (tray_)
    {
        return;
    }
    tray_ = std::make_unique<LinuxGtkTrayIcon>(
        [this]
        {
            gtk_window_present(GTK_WINDOW(window_));
        },
        [this]
        {
            // Real quit: drop the tray so close-request falls through to
            // the default (window destroyed → app holds nothing → quits).
            tray_.reset();
            g_application_quit(G_APPLICATION(app_));
        });
    if (tray_->is_available())
    {
        // Keep the GApplication alive when the window is hidden.
        g_application_hold(G_APPLICATION(app_));
    }
    else
    {
        tray_.reset();
    }
}

gboolean MainWindow::on_window_close_request_(GtkWindow* /*window*/,
                                              gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->tray_ && self->tray_->is_available())
    {
        gtk_widget_set_visible(self->window_, FALSE);
        return TRUE; // stop default destruction
    }
    return FALSE;
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
    return prefer_dark ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
}

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    if (main_app_surface_)
    {
        main_app_surface_->set_theme(t);
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->set_theme(t);
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->set_theme(t);
    }
    if (join_room_surface_)
    {
        join_room_surface_->set_theme(t);
    }
    if (account_picker_surface_)
    {
        account_picker_surface_->set_theme(t);
    }
    if (settings_widget_)
    {
        settings_widget_->set_theme(t);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(t);
    }
    if (login_view_)
    {
        login_view_->set_theme(t);
    }

    // Pop-out room windows track the theme too.
    apply_theme_to_secondary_windows_(t);

    // Tell GTK itself about the dark preference so native chrome follows.
    bool dark = (t.mode == tk::ThemeMode::Dark);
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", dark ? TRUE : FALSE,
                 nullptr);

    // Rebuild dynamic CSS rules. The compose-area rule is static but lives
    // here because load_from_string replaces all prior content in the provider.
    if (theme_css_provider_)
    {
        char css[512];
        std::snprintf(css, sizeof(css),
                      ".sidebar { background-color: #%02x%02x%02x; }\n"
                      ".sidebar-separator { background-color: #%02x%02x%02x; "
                      "min-width: 1px; }\n"
                      "textview.compose-area,"
                      "textview.compose-area text {"
                      " background: transparent; }\n",
                      t.palette.sidebar_bg.r, t.palette.sidebar_bg.g,
                      t.palette.sidebar_bg.b, t.palette.separator.r,
                      t.palette.separator.g, t.palette.separator.b);
        gtk_css_provider_load_from_string(theme_css_provider_, css);
    }
}

MainWindow::~MainWindow()
{
    if (theme_css_provider_)
    {
        g_object_unref(theme_css_provider_);
        theme_css_provider_ = nullptr;
    }
    if (search_debounce_id_)
    {
        g_source_remove(search_debounce_id_);
        search_debounce_id_ = 0;
    }
    if (scroll_debounce_id_)
    {
        g_source_remove(scroll_debounce_id_);
        scroll_debounce_id_ = 0;
    }
    if (tk_anim_tick_id_)
    {
        g_source_remove(tk_anim_tick_id_);
        tk_anim_tick_id_ = 0;
    }
    if (sync_status_debounce_id_)
    {
        g_source_remove(sync_status_debounce_id_);
        sync_status_debounce_id_ = 0;
    }
    // GTK4 top-level windows hold their own reference and must be destroyed
    // explicitly; they are not freed when their transient parent is destroyed.
    if (join_room_dialog_window_)
    {
        gtk_window_destroy(GTK_WINDOW(join_room_dialog_window_));
        join_room_dialog_window_ = nullptr;
    }
    // Drain background workers BEFORE tearing the client down. Each
    // worker calls `client_->fetch_*` (which takes `&mut self` on the
    // Rust side); racing one against `~ClientFfi` is a data race that
    // surfaces as `panic_in_cleanup` through cxx's `prevent_unwind`.
    // Order: flip the flag → wait (bounded) for in-flight ones →
    // only then stop_sync + destroy members.
    shutting_down_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                             [this]
                             {
                                 return workers_in_flight_ == 0;
                             });
    }
    // Stop sync on all accounts before any client is destroyed.
    for (auto& sess : accounts_)
    {
        if (sess->sync_started)
        {
            sess->client->stop_sync();
        }
    }
    // login_view_ holds pending_login_client_* — cancel + join its worker
    // before we destroy pending_login_client_ and the accounts vector.
    login_view_.reset();
    pending_login_client_.reset();
}

// ---------------------------------------------------------------------------

void MainWindow::do_login()
{
    tesseract::SessionStore::migrate_legacy_layout();

    auto index = tesseract::SessionStore::load_index();
    if (!index.user_ids.empty())
    {
        gtk_label_set_text(GTK_LABEL(status_bar_),
                           _("Restoring session\xe2\x80\xa6"));
        int first_active = -1;
        for (const auto& uid : index.user_ids)
        {
            auto saved = tesseract::SessionStore::load_account(uid);
            if (!saved)
            {
                continue;
            }

            auto sess = std::make_unique<tesseract::AccountSession>();
            sess->client = std::make_unique<tesseract::Client>();
            sess->client->set_data_dir(
                tesseract::SessionStore::sdk_store_dir(uid).string());
            auto res = sess->client->restore_session(*saved);
            if (!res)
            {
                tesseract::SessionStore::clear_account(uid);
                continue;
            }
            sess->user_id = sess->client->get_user_id();
            sess->display_name = sess->client->get_display_name();
            sess->avatar_url = sess->client->get_avatar_url();
            sess->last_room =
                tesseract::Prefs::parse(sess->client->load_prefs_json())
                    .last_room;

            auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
            bridge->set_user_id(sess->user_id);
            sess->client->start_sync(bridge.get());
            sess->bridge = std::move(bridge);
            sess->sync_started = true;

            // Per-account notifier: click switches to this account then navigates.
            const std::string notif_uid = sess->user_id;
            sess->notifier = std::make_unique<LinuxNotifierGtk>(
                [this, notif_uid](std::string room_id, std::string token)
                {
                    for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
                    {
                        if (accounts_[i]->user_id == notif_uid)
                        {
                            switch_active_account(i);
                            break;
                        }
                    }
                    // Set xdg_activation_v1 token (non-empty on modern Wayland)
                    // before gtk_window_present so the compositor grants focus.
                    if (!token.empty())
                    {
                        gtk_window_set_startup_id(GTK_WINDOW(window_),
                                                  token.c_str());
                    }
                    navigate_to_room(std::move(room_id));
                });

            // Per-account UnifiedPush connector.
            {
                auto up = std::make_unique<LinuxUpConnectorGtk>();
                up->start(sess->client.get(), sess->user_id);
                sess->up_connector = std::move(up);
            }

            int idx = static_cast<int>(accounts_.size());
            if (uid == index.active_user_id)
            {
                first_active = idx;
            }
            accounts_.push_back(std::move(sess));
        }

        if (!accounts_.empty())
        {
            if (first_active < 0)
            {
                first_active = 0;
            }
            switch_active_account(first_active);
            gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
            gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
            maybe_show_recovery_banner();
            start_tray_if_needed_();
            return;
        }
    }

    // No accounts: fresh install or all restores failed → show login view.
    pending_login_is_add_account_ = false;
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    login_view_->set_client(pending_login_client_.get());
    login_view_->set_on_begin_oauth(
        [this]
        {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" + std::to_string(ts));
            pending_login_client_->set_data_dir(
                (pending_login_temp_dir_ / "matrix-store").string());
        });
    login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
    login_view_->reset();
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Not logged in"));
}

void MainWindow::on_login_succeeded()
{
    // Export session before dropping the in-flight client.
    std::string uid = pending_login_client_->get_user_id();

    // Reject if this account is already signed in.
    for (const auto& a : accounts_)
    {
        if (a->user_id == uid)
        {
            pending_login_client_.reset();
            std::filesystem::remove_all(pending_login_temp_dir_);
            pending_login_temp_dir_.clear();
            gtk_label_set_text(GTK_LABEL(status_bar_),
                               ("Already signed in as " + uid).c_str());
            if (pending_login_is_add_account_ && add_account_return_idx_ >= 0)
            {
                switch_active_account(add_account_return_idx_);
                gtk_stack_set_visible_child_name(GTK_STACK(content_stack_),
                                                 "main");
            }
            pending_login_is_add_account_ = false;
            add_account_return_idx_ = -1;
            return;
        }
    }

    std::string exported = pending_login_client_->export_session();

    // Drop the in-flight client to release SQLite handles before rename.
    pending_login_client_.reset();

    // Rename temp dir → final per-account dir.
    auto target = tesseract::SessionStore::account_dir(uid);
    std::error_code ec;
    std::filesystem::rename(pending_login_temp_dir_, target, ec);
    if (ec)
    {
        std::filesystem::copy(pending_login_temp_dir_, target,
                              std::filesystem::copy_options::recursive, ec);
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
    }
    pending_login_temp_dir_.clear();

    tesseract::SessionStore::save_account(uid, exported);

    // Reopen the store at the final path.
    auto sess = std::make_unique<tesseract::AccountSession>();
    sess->client = std::make_unique<tesseract::Client>();
    sess->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(uid).string());
    auto res = sess->client->restore_session(exported);
    if (!res)
    {
        gtk_label_set_text(
            GTK_LABEL(status_bar_),
            (std::string(_("Login error: ")) + res.message).c_str());
        return;
    }
    sess->user_id = sess->client->get_user_id();
    sess->display_name = sess->client->get_display_name();
    sess->avatar_url = sess->client->get_avatar_url();
    sess->last_room =
        tesseract::Prefs::parse(sess->client->load_prefs_json()).last_room;

    auto bridge = std::make_unique<tesseract::EventHandlerBase>(this);
    bridge->set_user_id(sess->user_id);
    sess->client->start_sync(bridge.get());
    sess->bridge = std::move(bridge);
    sess->sync_started = true;

    // Per-account notifier: click switches to this account then navigates.
    const std::string notif_uid = sess->user_id;
    sess->notifier = std::make_unique<LinuxNotifierGtk>(
        [this, notif_uid](std::string room_id, std::string token)
        {
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
            {
                if (accounts_[i]->user_id == notif_uid)
                {
                    switch_active_account(i);
                    break;
                }
            }
            if (!token.empty())
            {
                gtk_window_set_startup_id(GTK_WINDOW(window_), token.c_str());
            }
            navigate_to_room(std::move(room_id));
        });

    // Per-account UnifiedPush connector.
    {
        auto up = std::make_unique<LinuxUpConnectorGtk>();
        up->start(sess->client.get(), sess->user_id);
        sess->up_connector = std::move(up);
    }

    int new_idx = static_cast<int>(accounts_.size());
    accounts_.push_back(std::move(sess));

    // Update accounts.json index.
    auto index = tesseract::SessionStore::load_index();
    if (std::find(index.user_ids.begin(), index.user_ids.end(), uid) ==
        index.user_ids.end())
    {
        index.user_ids.push_back(uid);
    }
    index.active_user_id = uid;
    tesseract::SessionStore::save_index(index);

    switch_active_account(new_idx);
    gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    maybe_show_recovery_banner();
    start_tray_if_needed_();
}

void MainWindow::on_send_clicked()
{
    if (room_view_)
    {
        room_view_->compose_bar()->trigger_send();
    }
}

void MainWindow::on_room_selected(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }

    // Drill into a space if the clicked row is one.
    for (const auto& r : rooms_)
    {
        if (r.id == room_id && r.is_space)
        {
            space_stack_.push_back(room_id);
            refresh_room_list();
            return;
        }
    }

    hide_shortcode_popup_();
    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

    current_room_id_ = room_id;
    clear_focused_state_(room_id);
    if (mark_read_timer_id_)
    {
        g_source_remove(mark_read_timer_id_);
        mark_read_timer_id_ = 0;
    }
    mark_read_timer_id_ = g_timeout_add(
        static_cast<guint>(
            tesseract::Settings::instance().mark_as_read_delay_ms),
        [](gpointer user_data) -> gboolean
        {
            auto* self = static_cast<MainWindow*>(user_data);
            self->mark_read_timer_id_ = 0;
            self->mark_room_read_(self->current_room_id_);
            return G_SOURCE_REMOVE;
        },
        this);
    update_typing_bar_({}, false);
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (room_view_)
    {
        room_view_->compose_bar()->clear_reply();
        room_view_->compose_bar()->clear_editing();
    }
    if (room_text_area_)
    {
        room_text_area_->set_text("");
    }
    if (room_text_area_)
    {
        room_text_area_->set_focused(true);
    }
    if (room_view_)
    {
        room_view_->clear_compose_text();
    }

    for (const auto& r : rooms_)
    {
        if (r.id == current_room_id_)
        {
            room_view_->set_room(r);
            break;
        }
    }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the GTK main loop stays responsive.
    auto visible_ids = room_list_view_ ? room_list_view_->visible_room_ids()
                                       : std::vector<std::string>{};
    std::string sub_room = current_room_id_;
    run_async_(
        [this, sub_room, visible_ids = std::move(visible_ids)]
        {
            auto res = client_->subscribe_room(sub_room);
            bool reached = false;
            if (res)
            {
                auto pr = client_->paginate_back_with_status(sub_room,
                                                             kPaginationBatch);
                reached = pr.ok && pr.reached_start;
                client_->start_background_backfill(visible_ids);
            }
            auto* d = new IdleSubscribeResult{this, sub_room, reached};
            g_idle_add(
                [](gpointer data) -> gboolean
                {
                    auto* dd = static_cast<IdleSubscribeResult*>(data);
                    dd->window->push_subscribe_result(std::move(dd->room_id),
                                                      dd->reached_start);
                    delete dd;
                    return G_SOURCE_REMOVE;
                },
                d);
        });
}

void MainWindow::push_paginate_result(std::string room_id, bool reached_start)
{
    bool is_current = (room_id == current_room_id_);
    push_paginate_result_(std::move(room_id), reached_start);
    if (is_current && room_view_)
    {
        room_view_->message_list()->reset_near_top_latch();
    }
}

void MainWindow::push_subscribe_result(std::string room_id, bool reached_start)
{
    if (room_id != current_room_id_)
    {
        return;
    }
    auto& state = pagination_[room_id];
    state.in_flight = false;
    state.reached_start = reached_start;
}

void MainWindow::request_more_history(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    state.in_flight = true;

    // Worker thread: invoke the blocking SDK call, marshal the result
    // back via g_idle_add on the main loop.
    run_async_(
        [this, room_id]
        {
            auto pr =
                client_->paginate_back_with_status(room_id, kPaginationBatch);
            auto* p = new IdlePaginateResult{this, room_id,
                                             pr.ok && pr.reached_start};
            g_idle_add(
                [](gpointer data) -> gboolean
                {
                    auto* d = static_cast<IdlePaginateResult*>(data);
                    d->window->push_paginate_result(std::move(d->room_id),
                                                    d->reached_start);
                    delete d;
                    return G_SOURCE_REMOVE;
                },
                p);
        });
}

void MainWindow::open_jump_to_date_dialog()
{
    if (current_room_id_.empty())
    {
        return;
    }

    auto* dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), _("Jump to Date"));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(window_));
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 300, -1);

    auto* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    auto* calendar = gtk_calendar_new();
    gtk_box_append(GTK_BOX(vbox), calendar);

    auto* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    auto* cancel_btn = gtk_button_new_with_label(_("Cancel"));
    auto* ok_btn = gtk_button_new_with_label(_("Jump"));
    gtk_box_append(GTK_BOX(btn_row), cancel_btn);
    gtk_box_append(GTK_BOX(btn_row), ok_btn);
    gtk_box_append(GTK_BOX(vbox), btn_row);

    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    auto* ctx = new JumpDlgCtx{this, calendar, dlg};
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_jump_dialog_ok_), ctx);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_jump_dialog_cancel_),
                     ctx);
    // "destroy" fires when the window is torn down (by button OR window-manager X).
    // It is the single place we free ctx; button callbacks must not free it themselves.
    g_signal_connect(dlg, "destroy", G_CALLBACK(on_jump_dialog_destroy_), ctx);

    gtk_window_present(GTK_WINDOW(dlg));
}

void MainWindow::on_jump_dialog_cancel_(GtkButton*, gpointer user_data)
{
    auto* ctx = static_cast<JumpDlgCtx*>(user_data);
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    // ctx freed by on_jump_dialog_destroy_
}

void MainWindow::on_jump_dialog_ok_(GtkButton*, gpointer user_data)
{
    auto* ctx = static_cast<JumpDlgCtx*>(user_data);
    MainWindow* self = ctx->self;

    // Extract date BEFORE destroying the dialog (which unrefs all children).
    GDateTime* gdt = gtk_calendar_get_date(GTK_CALENDAR(ctx->calendar));
    int year, month, day;
    g_date_time_get_ymd(gdt, &year, &month, &day);
    g_date_time_unref(gdt);

    const std::string room_id = self->current_room_id_;
    gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    // ctx freed by on_jump_dialog_destroy_; do NOT access ctx after this point.

    if (room_id.empty())
    {
        return;
    }

    // Reject pre-epoch dates to avoid uint64_t wrap-around.
    if (year < 1970)
    {
        if (self->status_bar_)
        {
            gtk_label_set_text(
                GTK_LABEL(self->status_bar_),
                _("Jump to date: please select a date from 1970 onwards"));
        }
        return;
    }

    GTimeZone* utc_tz = g_time_zone_new_utc();
    GDateTime* midnight = g_date_time_new(utc_tz, year, month, day, 0, 0, 0.0);
    g_time_zone_unref(utc_tz);
    const gint64 unix_s = g_date_time_to_unix(midnight);
    g_date_time_unref(midnight);

    const uint64_t ts_ms = static_cast<uint64_t>(unix_s) * 1000ULL;

    self->run_async_(
        [self, room_id, ts_ms]
        {
            auto res = self->client_->timestamp_to_event(room_id, ts_ms, "f");
            if (!res.ok)
            {
                auto* e = new IdleJumpError{self, res.message};
                g_idle_add(
                    [](gpointer p) -> gboolean
                    {
                        auto* d = static_cast<IdleJumpError*>(p);
                        if (d->window->status_bar_)
                        {
                            gtk_label_set_text(
                                GTK_LABEL(d->window->status_bar_),
                                d->message.c_str());
                        }
                        delete d;
                        return G_SOURCE_REMOVE;
                    },
                    e);
                return;
            }
            auto* d = new IdleJumpResult{self, room_id, res.message};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* data = static_cast<IdleJumpResult*>(p);
                    MainWindow* w = data->window;
                    std::string rid = std::move(data->room_id);
                    std::string eid = std::move(data->event_id);
                    delete data;
                    w->begin_focused_subscription_(rid, eid);
                    w->run_async_(
                        [w, rid, eid]
                        {
                            w->client_->subscribe_room_at(rid, eid);
                        });
                    return G_SOURCE_REMOVE;
                },
                d);
        });
}

void MainWindow::on_jump_dialog_destroy_(GtkWidget*, gpointer user_data)
{
    delete static_cast<JumpDlgCtx*>(user_data);
}

void MainWindow::on_login_clicked(GtkButton*, gpointer user_data)
{
    static_cast<MainWindow*>(user_data)->do_login();
}

// ---------------------------------------------------------------------------

void MainWindow::push_message_inserted(std::string room_id, std::size_t index,
                                       std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (room_id == current_room_id_)
    {
        ensure_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->insert_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        main_app_surface_->relayout();
    }

    dispatch_message_inserted_secondary_(room_id, index, *ev);
}

void MainWindow::push_message_updated(std::string room_id, std::size_t index,
                                      std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (room_id == current_room_id_)
    {
        ensure_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->update_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        main_app_surface_->relayout();
    }

    dispatch_message_updated_secondary_(room_id, index, *ev);
}

void MainWindow::push_message_removed(std::string room_id, std::size_t index)
{
    if (room_id == current_room_id_)
    {
        room_view_->remove_message(index);
        main_app_surface_->relayout();
    }
    dispatch_message_removed_secondary_(room_id, index);
}

void MainWindow::push_rooms(std::string user_id,
                            std::vector<tesseract::RoomInfo> rooms)
{
    push_rooms_(std::move(user_id), std::move(rooms));
}

void MainWindow::on_rooms_updated_()
{
    refresh_room_list();
    if (!current_room_id_.empty() && room_view_)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == current_room_id_)
            {
                room_view_->set_room(r);
                break;
            }
        }
    }
    else if (!pending_restore_room_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                on_room_selected(target);
                break;
            }
        }
    }

    update_secondary_room_infos_();
}

void MainWindow::handle_reconnect(const std::string& user_id)
{
    gtk_label_set_text(GTK_LABEL(status_bar_),
                       _("Sync error: reconnecting\xe2\x80\xa6"));
    for (auto& sess : accounts_)
    {
        if (sess->user_id == user_id && sess->client)
        {
            sess->client->stop_sync();
            sess->sync_started = false;
            break;
        }
    }
    // Restart the affected account's sync after a short delay.  do not
    // call do_login() (which rebuilds all sessions), as that causes a tight
    // loop when the server rejects key uploads on every new session.
    struct DelayData
    {
        MainWindow* w;
        std::string uid;
        std::weak_ptr<bool> alive;
    };
    auto* dd = new DelayData{this, user_id, alive_};
    g_timeout_add(
        5000,
        [](gpointer data) -> gboolean
        {
            auto* d = static_cast<DelayData*>(data);
            if (!d->alive.expired())
            {
                for (auto& s : d->w->accounts_)
                {
                    if (s->user_id == d->uid && !s->sync_started && s->client)
                    {
                        s->sync_started = true;
                        s->client->start_sync(s->bridge.get());
                    }
                }
            }
            delete d;
            return G_SOURCE_REMOVE;
        },
        dd);
}

void MainWindow::handle_auth_error(bool soft_logout)
{
    if (soft_logout && active_account_index_ >= 0)
    {
        const std::string& uid = accounts_[active_account_index_]->user_id;
        if (auto saved = tesseract::SessionStore::load_account(uid))
        {
            gtk_label_set_text(GTK_LABEL(status_bar_),
                               _("Reconnecting session\xe2\x80\xa6"));
            if (client_->restore_session(*saved))
            {
                my_user_id_ = client_->get_user_id();
                my_display_name_ = client_->get_display_name();
                my_avatar_url_ = client_->get_avatar_url();
                populate_user_strip();
                client_->start_sync(event_handler_);
                gtk_label_set_text(GTK_LABEL(status_bar_), _("Reconnected"));
                maybe_show_recovery_banner();
                return;
            }
        }
    }
    if (active_account_index_ >= 0)
    {
        tesseract::SessionStore::clear_account(
            accounts_[active_account_index_]->user_id);
    }
    if (client_)
    {
        client_->stop_sync();
    }
    gtk_label_set_text(GTK_LABEL(status_bar_),
                       _("Session expired; please log in again."));
    do_login();
}

void MainWindow::push_error(std::string description)
{
    gtk_label_set_text(GTK_LABEL(status_bar_), description.c_str());
}

void MainWindow::push_timeline_reset(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    if (room_id == current_room_id_)
    {
        auto rows = build_rows_(snapshot);
        // A genuine switch, OR a re-population of an emptied view (e.g.
        // logout → login → same room): both warrant the display gate.
        const auto* ml = room_view_ ? room_view_->message_list() : nullptr;
        const bool room_switch = view_displayed_room_id_ != room_id ||
                                 (ml && ml->messages().empty());
        view_displayed_room_id_ = room_id;
        if (room_view_)
        {
            room_view_->set_messages(std::move(rows), room_switch);
        }
        main_app_surface_->relayout();
        if (room_view_ && room_view_->message_list())
        {
            const auto& pstate = pagination_[room_id];
            if (room_switch && pstate.is_focused)
            {
                room_view_->message_list()->begin_focused_gate(
                    pstate.focus_event_id);
            }
            room_view_->message_list()->set_historical_mode(pstate.is_focused);
            if (pstate.is_focused)
            {
                room_view_->message_list()->scroll_to_event_id(
                    pstate.focus_event_id);
            }
        }
    }

    dispatch_timeline_reset_secondary_(room_id, snapshot);
}

void MainWindow::clear_messages()
{
    if (room_view_)
    {
        room_view_->clear_room();
        room_view_->set_messages({});
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------
//  Avatar / inline-media decode into tk::Image
// ---------------------------------------------------------------------------

namespace
{

// Decode raw image bytes to a premultiplied-ARGB32 cairo_surface_t the
// shared CairoImage wrapper expects. Reuses GdkPixbufLoader so the
// existing matrix-sdk attachments path (PNG/JPEG/WebP/AVIF) decodes
// identically to the legacy GTK rendering.
//
// Inner helper: convert an already-decoded GdkPixbuf into a premultiplied
// ARGB32 cairo surface. Reused by both the static decoder and the
// animated-frame iterator below.
cairo_surface_t* pixbuf_to_premultiplied_argb32(GdkPixbuf* pb)
{
    if (!pb)
    {
        return nullptr;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int channels = gdk_pixbuf_get_n_channels(pb);
    int in_stride = gdk_pixbuf_get_rowstride(pb);
    const guchar* pixels = gdk_pixbuf_read_pixels(pb);

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    unsigned char* dst = cairo_image_surface_get_data(surface);
    int out_stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < h; ++y)
    {
        const guchar* src_row = pixels + y * in_stride;
        unsigned char* dst_row = dst + y * out_stride;
        for (int x = 0; x < w; ++x)
        {
            guchar r = src_row[x * channels + 0];
            guchar g = src_row[x * channels + 1];
            guchar b = src_row[x * channels + 2];
            guchar a = channels == 4 ? src_row[x * channels + 3] : 255;
            unsigned r_p = (r * a + 127) / 255;
            unsigned g_p = (g * a + 127) / 255;
            unsigned b_p = (b * a + 127) / 255;
            dst_row[x * 4 + 0] = static_cast<unsigned char>(b_p);
            dst_row[x * 4 + 1] = static_cast<unsigned char>(g_p);
            dst_row[x * 4 + 2] = static_cast<unsigned char>(r_p);
            dst_row[x * 4 + 3] = a;
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

cairo_surface_t*
decode_image_to_cairo_surface(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return nullptr;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return nullptr;
    }
    if (!gdk_pixbuf_loader_close(loader, &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return nullptr;
    }
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
    cairo_surface_t* surface = pixbuf_to_premultiplied_argb32(pb);
    g_object_unref(loader);
    return surface;
}

// Decode an animated GIF / WebP / APNG into a list of premultiplied
// ARGB32 cairo surfaces + a per-frame delay (ms). Returns nullopt for
// non-animated payloads — callers should fall back to the static path.
//
// Termination: walks the GdkPixbufAnimationIter forwards with a
// synthesised clock advanced by each frame's reported delay. Capped at
// `kMaxFrames` to keep runaway / never-ending GIFs from blowing memory.
// Most animated stickers ship ≤ 30 frames.
struct DecodedAnimation
{
    std::vector<cairo_surface_t*> frames; // caller owns each
    std::vector<int> delays_ms;
};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
std::optional<DecodedAnimation>
decode_animation(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return std::nullopt;
    }
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    if (!gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return std::nullopt;
    }
    if (!gdk_pixbuf_loader_close(loader, &err))
    {
        if (err)
        {
            g_error_free(err);
        }
        g_object_unref(loader);
        return std::nullopt;
    }
    GdkPixbufAnimation* anim = gdk_pixbuf_loader_get_animation(loader);
    if (!anim || gdk_pixbuf_animation_is_static_image(anim))
    {
        g_object_unref(loader);
        return std::nullopt;
    }

    GTimeVal t = {0, 0};
    GdkPixbufAnimationIter* iter = gdk_pixbuf_animation_get_iter(anim, &t);
    if (!iter)
    {
        g_object_unref(loader);
        return std::nullopt;
    }

    DecodedAnimation out;
    constexpr int kMaxFrames = 200;
    for (int i = 0; i < kMaxFrames; ++i)
    {
        GdkPixbuf* pb = gdk_pixbuf_animation_iter_get_pixbuf(iter);
        if (!pb)
        {
            break;
        }
        cairo_surface_t* surf = pixbuf_to_premultiplied_argb32(pb);
        if (!surf)
        {
            break;
        }
        int delay = gdk_pixbuf_animation_iter_get_delay_time(iter);
        // -1 means there's no upcoming frame (last frame of a
        // non-looping animation). Capture this final frame and stop.
        if (delay < 0)
        {
            out.frames.push_back(surf);
            out.delays_ms.push_back(100); // arbitrary tail-hold
            break;
        }
        if (delay < 20)
        {
            delay = 20;
        }
        out.frames.push_back(surf);
        out.delays_ms.push_back(delay);

        // Advance the synthesised clock by the just-captured delay.
        t.tv_usec += delay * 1000;
        while (t.tv_usec >= G_USEC_PER_SEC)
        {
            t.tv_sec += 1;
            t.tv_usec -= G_USEC_PER_SEC;
        }
        if (!gdk_pixbuf_animation_iter_advance(iter, &t))
        {
            // Iterator decided no new frame would be shown — we'd
            // duplicate the same pixbuf on the next iteration. Stop.
            break;
        }
    }
    g_object_unref(iter);
    g_object_unref(loader);
    if (out.frames.empty())
    {
        return std::nullopt;
    }
    return out;
}
G_GNUC_END_IGNORE_DEPRECATIONS

} // namespace

void MainWindow::start_anim_tick_if_needed_()
{
    if (tk_anim_tick_id_ != 0)
    {
        return;
    }
    if (anim_cache_.empty())
    {
        return;
    }
    tk_anim_tick_id_ = g_timeout_add(16, on_tk_anim_tick_, this);
}

void MainWindow::invalidate_anim_consumers_()
{
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

gboolean MainWindow::on_tk_anim_tick_(gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->anim_cache_.empty())
    {
        self->tk_anim_tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    const std::int64_t now_ms = g_get_monotonic_time() / 1000;
    if (self->anim_cache_.advance(now_ms))
    {
        self->invalidate_anim_consumers_();
    }
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------

void MainWindow::show_rooms(const std::vector<tesseract::RoomInfo>& rooms)
{
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms)
    {
        if (!r.is_space)
        {
            sorted.push_back(r);
        }
    }
    for (const auto& r : rooms)
    {
        if (r.is_space)
        {
            sorted.push_back(r);
        }
    }

    // Eagerly fetch avatars for the new room set so the first paint has
    // them ready. Bytes-already-cached is a no-op via tk_avatars_.count.
    for (const auto& r : sorted)
    {
        ensure_room_avatar_(r);
    }

    room_list_view_->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
    {
        room_list_view_->set_selected_room(current_room_id_);
    }
    main_app_surface_->relayout();
}

void MainWindow::refresh_room_list()
{
    // Both branches dereference client_ (space_children). client_ is null
    // after the last account logs out — render an empty list, don't crash.
    if (!client_)
    {
        if (main_app_)
        {
            main_app_->set_space_nav(false);
        }
        show_rooms({});
        return;
    }
    if (space_stack_.empty())
    {
        if (!search_pending_text_.empty())
        {
            if (main_app_)
            {
                main_app_->set_space_nav(false);
            }
            show_rooms(rooms_);
            return;
        }
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
        {
            if (!r.is_space)
            {
                continue;
            }
            for (const auto& id : client_->space_children(r.id))
            {
                in_space.insert(id);
            }
        }
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        for (const auto& r : rooms_)
        {
            if (r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        if (main_app_)
        {
            main_app_->set_space_nav(false);
        }
        show_rooms(filtered);
    }
    else
    {
        const std::string& space_id = space_stack_.back();
        auto child_ids = client_->space_children(space_id);
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
            {
                filtered.push_back(r);
            }
        }
        if (main_app_)
        {
            for (const auto& r : rooms_)
            {
                if (r.id == space_id)
                {
                    ensure_room_avatar_(r);
                    main_app_->set_space_nav(true, r.name, r.avatar_url);
                    break;
                }
            }
        }
        show_rooms(filtered);
    }
}

// ---------------------------------------------------------------------------
//  GTK4-specific ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::post_to_ui_(std::function<void()> fn)
{
    struct Data
    {
        std::function<void()> fn;
    };
    auto* d = new Data{std::move(fn)};
    g_idle_add(
        [](gpointer p) -> gboolean
        {
            auto* data = static_cast<Data*>(p);
            data->fn();
            delete data;
            return G_SOURCE_REMOVE;
        },
        d);
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                       MediaKind kind,
                                       std::vector<uint8_t> bytes)
{
    if (bytes.empty())
    {
        return;
    }
    if (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar)
    {
        if (tk_avatars_.count(cache_key))
        {
            return;
        }
        if (cairo_surface_t* surface = decode_image_to_cairo_surface(bytes))
        {
            auto img = tk::cairo_pango::make_image(surface);
            cairo_surface_destroy(surface);
            tk_avatars_.emplace(cache_key, std::move(img));
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
    }
    else if (kind == MediaKind::Tile)
    {
        if (tk_images_.count(cache_key))
        {
            return;
        }
        if (cairo_surface_t* surface = decode_image_to_cairo_surface(bytes))
        {
            auto img = tk::cairo_pango::make_image(surface);
            cairo_surface_destroy(surface);
            tk_images_.emplace(cache_key, std::move(img));
            if (room_view_)
            {
                room_view_->message_list()->invalidate_data();
            }
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
        }
    }
    else
    { // MediaImage
        if (tk_images_.count(cache_key) || anim_cache_.has(cache_key))
        {
            return;
        }
        if (auto anim = decode_animation(bytes))
        {
            std::vector<std::unique_ptr<tk::Image>> frames;
            frames.reserve(anim->frames.size());
            for (cairo_surface_t* s : anim->frames)
            {
                frames.push_back(tk::cairo_pango::make_image(s));
                cairo_surface_destroy(s);
            }
            if (!frames.empty())
            {
                const gint64 now_ms = g_get_monotonic_time() / 1000;
                anim_cache_.store(cache_key, std::move(frames),
                                  std::move(anim->delays_ms), now_ms);
                start_anim_tick_if_needed_();
                if (room_view_)
                {
                    room_view_->notify_image_ready(cache_key);
                }
                if (main_app_surface_)
                {
                    main_app_surface_->relayout();
                }
            }
        }
        else if (cairo_surface_t* surface =
                     decode_image_to_cairo_surface(bytes))
        {
            auto img = tk::cairo_pango::make_image(surface);
            cairo_surface_destroy(surface);
            tk_images_.emplace(cache_key, std::move(img));
            if (room_view_)
            {
                room_view_->notify_image_ready(cache_key);
            }
            if (main_app_surface_)
            {
                main_app_surface_->relayout();
            }
            if (shortcode_popup_visible_() && shortcode_popup_surface_)
            {
                shortcode_popup_surface_->relayout();
            }
        }
    }
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int /*max_w*/,
                          int /*max_h*/)
{
    // decode_image_to_cairo_surface / decode_animation are in this
    // file's anonymous namespace and are thread-safe (GdkPixbuf + cairo).
    // tk::cairo_pango::make_image refcounts the surface (thread-safe).
    DecodedImage d;
    if (auto anim = decode_animation(bytes))
    {
        d.frames.reserve(anim->frames.size());
        for (cairo_surface_t* s : anim->frames)
        {
            d.frames.push_back(tk::cairo_pango::make_image(s));
            cairo_surface_destroy(s);
        }
        d.delays_ms = std::move(anim->delays_ms);
        if (!d.frames.empty())
        {
            return d;
        }
        d.delays_ms.clear();
    }
    if (cairo_surface_t* surf = decode_image_to_cairo_surface(bytes))
    {
        d.still = tk::cairo_pango::make_image(surf);
        cairo_surface_destroy(surf);
    }
    return d;
}

std::int64_t MainWindow::monotonic_ms_()
{
    return g_get_monotonic_time() / 1000;
}

void MainWindow::start_anim_tick_()
{
    start_anim_tick_if_needed_();
}

void MainWindow::repaint_pickers_()
{
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->invalidate_image_cache();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->invalidate_image_cache();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    invalidate_anim_consumers_();
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& video_url)
{
    const std::string eid = event_id;
    run_async_(
        [this, eid, src = video_url]() mutable
        {
            auto bytes = client_->fetch_source_bytes(src);
            if (bytes.empty())
            {
                return;
            }
            // Extract first frame via GStreamer appsink.
            GstElement* pipe = gst_pipeline_new(nullptr);
            GstElement* gsrc =
                gst_element_factory_make("giostreamsrc", nullptr);
            GstElement* dec = gst_element_factory_make("decodebin", nullptr);
            GstElement* vconv =
                gst_element_factory_make("videoconvert", nullptr);
            GstElement* vsink = gst_element_factory_make("appsink", nullptr);
            if (!pipe || !gsrc || !dec || !vconv || !vsink)
            {
                if (pipe)
                {
                    gst_object_unref(pipe);
                }
                if (gsrc)
                {
                    gst_object_unref(gsrc);
                }
                if (dec)
                {
                    gst_object_unref(dec);
                }
                if (vconv)
                {
                    gst_object_unref(vconv);
                }
                if (vsink)
                {
                    gst_object_unref(vsink);
                }
                return;
            }
            GstCaps* caps = gst_caps_from_string("video/x-raw,format=BGRA");
            gst_app_sink_set_caps(GST_APP_SINK(vsink), caps);
            gst_caps_unref(caps);
            gst_app_sink_set_drop(GST_APP_SINK(vsink), FALSE);
            gst_app_sink_set_max_buffers(GST_APP_SINK(vsink), 1);
            GInputStream* mem_stream = g_memory_input_stream_new_from_data(
                bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
            g_object_set(gsrc, "stream", mem_stream, nullptr);
            g_object_unref(mem_stream);
            gst_bin_add_many(GST_BIN(pipe), gsrc, dec, vconv, vsink, nullptr);
            gst_element_link(gsrc, dec);
            gst_element_link(vconv, vsink);
            struct PadCtx
            {
                GstElement* vconv;
            };
            auto* pad_ctx = new PadCtx{vconv};
            g_signal_connect(
                dec, "pad-added",
                G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer ud)
                           {
                               auto* pc = static_cast<PadCtx*>(ud);
                               GstCaps* c2 = gst_pad_get_current_caps(pad);
                               if (!c2)
                               {
                                   c2 = gst_pad_query_caps(pad, nullptr);
                               }
                               GstStructure* st = gst_caps_get_structure(c2, 0);
                               if (g_str_has_prefix(gst_structure_get_name(st),
                                                    "video"))
                               {
                                   GstPad* sp = gst_element_get_static_pad(
                                       pc->vconv, "sink");
                                   if (sp && !gst_pad_is_linked(sp))
                                   {
                                       gst_pad_link(pad, sp);
                                   }
                                   if (sp)
                                   {
                                       gst_object_unref(sp);
                                   }
                               }
                               gst_caps_unref(c2);
                           }),
                pad_ctx);
            gst_element_set_state(pipe, GST_STATE_PLAYING);
            // Pull exactly one preroll frame.
            GstSample* sample = gst_app_sink_pull_preroll(GST_APP_SINK(vsink));
            gst_element_set_state(pipe, GST_STATE_NULL);
            delete pad_ctx;
            gst_object_unref(pipe);
            if (!sample)
            {
                return;
            }
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstCaps* scaps = gst_sample_get_caps(sample);
            int w = 0, h = 0;
            if (scaps)
            {
                GstStructure* st = gst_caps_get_structure(scaps, 0);
                gst_structure_get_int(st, "width", &w);
                gst_structure_get_int(st, "height", &h);
            }
            if (!buf || w <= 0 || h <= 0)
            {
                gst_sample_unref(sample);
                return;
            }
            GstMapInfo map;
            if (!gst_buffer_map(buf, &map, GST_MAP_READ))
            {
                gst_sample_unref(sample);
                return;
            }
            std::vector<uint8_t> frame_bytes(map.data, map.data + map.size);
            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
            // BGRA → cairo surface on the main thread.
            struct Ctx
            {
                MainWindow* self;
                std::string key;
                std::vector<uint8_t> pixels;
                int w, h;
            };
            auto* ctx =
                new Ctx{this, "thumb::" + eid, std::move(frame_bytes), w, h};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (!c->self->tk_images_.count(c->key))
                    {
                        // Create an owned cairo surface and blit the BGRA pixels in.
                        cairo_surface_t* surf = cairo_image_surface_create(
                            CAIRO_FORMAT_ARGB32, c->w, c->h);
                        if (surf &&
                            cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
                        {
                            int dst_stride =
                                cairo_image_surface_get_stride(surf);
                            unsigned char* dst =
                                cairo_image_surface_get_data(surf);
                            int src_stride = c->w * 4;
                            for (int row = 0; row < c->h; ++row)
                            {
                                std::memcpy(
                                    dst + row * dst_stride,
                                    c->pixels.data() + row * src_stride,
                                    static_cast<std::size_t>(src_stride));
                            }
                            cairo_surface_mark_dirty(surf);
                            c->self->tk_images_.emplace(
                                c->key, tk::cairo_pango::make_image(surf));
                            cairo_surface_destroy(surf);
                            if (c->self->main_app_surface_)
                            {
                                c->self->main_app_surface_->relayout();
                            }
                        }
                        else if (surf)
                        {
                            cairo_surface_destroy(surf);
                        }
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
}

void MainWindow::on_url_preview_ready_(
    const std::string& url, const tesseract::Client::UrlPreview& preview)
{
    tesseract::views::UrlPreviewData d;
    d.title = preview.title;
    d.description = preview.description;
    d.image_mxc = preview.image_mxc;
    d.image_w = preview.image_w;
    d.image_h = preview.image_h;
    url_preview_data_.emplace(url, std::move(d));

    if (!preview.image_mxc.empty())
    {
        ensure_media_image_(preview.image_mxc, 64, 64);
    }

    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
            w->request_relayout();
        }
    }
}

void MainWindow::on_url_preview_failed_(const std::string& url)
{
    // No card to show (height unchanged) — just release the room-switch
    // gate so it doesn't wait the full timeout on a dead link.
    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
        }
    }
}

void MainWindow::cache_rgba_image_(const std::string& key, int w, int h,
                                   std::vector<uint8_t> rgba)
{
    if (tk_images_.count(key))
    {
        return;
    }
    GdkPixbuf* pb =
        gdk_pixbuf_new_from_data(rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8, w, h,
                                 w * 4, nullptr, nullptr);
    if (!pb)
    {
        return;
    }
    cairo_surface_t* surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surf);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(pb);
    tk_images_.emplace(key, tk::cairo_pango::make_image(surf));
    cairo_surface_destroy(surf);
    if (main_app_surface_)
    {
        gtk_widget_queue_draw(main_app_surface_->widget());
    }
}

// ---------------------------------------------------------------------------

void MainWindow::maybe_show_recovery_banner()
{
    if (recovery_banner_dismissed_)
    {
        return;
    }
    if (!client_->needs_recovery())
    {
        return;
    }
    if (!main_app_)
    {
        return;
    }
    // Verification takes priority — don't show recovery banner while the
    // verification banner is active. The "Use recovery key" link hands off.
    if (main_app_->verif_banner()->visible())
    {
        return;
    }
    if (!main_app_->recovery_banner()->visible())
    {
        if (recovery_shared_)
        {
            recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Form);
            recovery_shared_->set_current_key("");
        }
        if (recovery_key_field_)
        {
            recovery_key_field_->set_text("");
            recovery_key_field_->set_enabled(true);
        }
        main_app_->show_recovery_banner(true);
        main_app_surface_->relayout();
    }
}

void MainWindow::on_recovery_verify_clicked_(GtkButton*, gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    std::string key;
    if (self->recovery_key_field_)
    {
        key = self->recovery_key_field_->text();
    }
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
    {
        if (self->recovery_shared_)
        {
            self->recovery_shared_->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            self->recovery_shared_->set_failure_message(
                _("Please enter a recovery key or passphrase."));
            if (self->main_app_surface_)
            {
                self->main_app_surface_->relayout();
            }
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (self->recovery_shared_)
    {
        self->recovery_shared_->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
    }
    if (self->recovery_key_field_)
    {
        self->recovery_key_field_->set_enabled(false);
    }
    if (self->main_app_surface_)
    {
        self->main_app_surface_->relayout();
    }

    struct RecoverDone
    {
        MainWindow* window;
        bool ok;
        std::string message;
    };
    self->run_async_(
        [self, key]()
        {
            auto res = self->client_->recover(key);
            auto* p = new RecoverDone{self, res.ok, res.message};
            g_idle_add(
                [](gpointer data) -> gboolean
                {
                    auto* d = static_cast<RecoverDone*>(data);
                    if (d->ok)
                    {
                        if (d->window->recovery_shared_)
                        {
                            d->window->recovery_shared_->set_state(
                                tesseract::views::RecoveryBanner::State::
                                    Importing);
                        }
                    }
                    else
                    {
                        if (d->window->recovery_shared_)
                        {
                            d->window->recovery_shared_->set_state(
                                tesseract::views::RecoveryBanner::State::
                                    Failed);
                            d->window->recovery_shared_->set_failure_message(
                                d->message);
                        }
                        if (d->window->recovery_key_field_)
                        {
                            d->window->recovery_key_field_->set_enabled(true);
                            d->window->recovery_key_field_->set_focused(true);
                        }
                    }
                    if (d->window->main_app_surface_)
                    {
                        d->window->main_app_surface_->relayout();
                    }
                    delete d;
                    return G_SOURCE_REMOVE;
                },
                p);
        });
}

void MainWindow::on_recovery_dismiss_clicked_(GtkButton*, gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    self->recovery_banner_dismissed_ = true;
    if (self->main_app_)
    {
        self->main_app_->show_recovery_banner(false);
        if (self->main_app_surface_)
        {
            self->main_app_surface_->relayout();
        }
    }
}

void MainWindow::push_notification(const std::string& user_id,
                                   const std::string& room_id,
                                   const std::string& room_name,
                                   const std::string& sender,
                                   const std::string& body, bool is_mention,
                                   std::vector<uint8_t> avatar_bytes,
                                   std::vector<uint8_t> image_bytes)
{
    handle_notification(user_id, room_id, room_name, sender, body, is_mention,
                        std::move(avatar_bytes), std::move(image_bytes));
}

void MainWindow::handle_notification(const std::string& user_id,
                                     const std::string& room_id,
                                     const std::string& room_name,
                                     const std::string& sender,
                                     const std::string& body, bool is_mention,
                                     std::vector<uint8_t> avatar_bytes,
                                     std::vector<uint8_t> image_bytes)
{
    bool win_focused = gtk_window_is_active(GTK_WINDOW(window_));
    auto* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    auto state = gdk_toplevel_get_state(GDK_TOPLEVEL(surface));
    bool win_visible = gtk_widget_get_visible(GTK_WIDGET(window_)) &&
                       !(state & GDK_TOPLEVEL_STATE_MINIMIZED);

    for (auto& sess : accounts_)
    {
        if (sess->user_id != user_id)
        {
            continue;
        }
        // Already watching this exact room — suppress silently.
        if (win_focused && active_account_index_ >= 0 &&
            accounts_[active_account_index_]->user_id == user_id &&
            current_room_id_ == room_id)
        {
            return;
        }
        // Window on screen, not focused: GTK4 has no urgency-hint API, so
        // request attention with a GNotification instead (the GTK4-native
        // mechanism; on most shells it also flags the app in the dock /
        // taskbar). One reusable id so a newer message replaces the
        // previous banner; it is withdrawn when the window regains focus
        // (notify::is-active handler in the constructor).
        if (win_visible)
        {
            if (!win_focused && app_)
            {
                GNotification* notif = g_notification_new(sender.c_str());
                g_notification_set_body(notif, body.c_str());
                g_notification_set_priority(
                    notif, is_mention ? G_NOTIFICATION_PRIORITY_HIGH
                                      : G_NOTIFICATION_PRIORITY_NORMAL);
                if (!avatar_bytes.empty())
                {
                    GBytes* gb =
                        g_bytes_new(avatar_bytes.data(), avatar_bytes.size());
                    GIcon* ic = g_bytes_icon_new(gb);
                    g_notification_set_icon(notif, ic);
                    g_object_unref(ic);
                    g_bytes_unref(gb);
                }
                g_application_send_notification(G_APPLICATION(app_),
                                                kAttentionNotifId, notif);
                g_object_unref(notif);
            }
            // Focused (different room) or no app: the sidebar unread badge
            // is signal enough — no popup.
            return;
        }
        // Window minimised / hidden: send system notification.
        if (sess->notifier)
        {
            tesseract::Notification n;
            n.room_id = room_id;
            n.room_name = room_name;
            n.sender = sender;
            n.body = body;
            n.is_mention = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            n.image_bytes = std::move(image_bytes);
            sess->notifier->notify(n);
        }
        return;
    }
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (room_list_view_)
    {
        room_list_view_->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::refresh_pickers_packs_()
{
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_emoticon_packs();
    }
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

void MainWindow::push_backup_progress(tesseract::BackupProgress progress)
{
    maybe_show_recovery_banner();

    if (main_app_ && recovery_shared_ &&
        main_app_->recovery_banner()->visible() &&
        recovery_shared_->state() ==
            tesseract::views::RecoveryBanner::State::Importing &&
        progress.state == tesseract::BackupState::Downloading &&
        progress.imported_keys > 0)
    {
        recovery_shared_->set_import_progress(progress.imported_keys);
        main_app_surface_->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled &&
        !client_->needs_recovery() && main_app_)
    {
        main_app_->show_recovery_banner(false);
        main_app_surface_->relayout();
    }

    last_backup_state_ = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refresh_sync_status();
}

void MainWindow::push_room_list_state(tesseract::RoomListState state)
{
    push_room_list_state_(state);
    refresh_sync_status();
}

gboolean MainWindow::on_sync_status_debounce_(gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    self->sync_status_debounce_id_ = 0;
    using RLS = tesseract::RoomListState;
    if (self->status_bar_ && (self->last_room_list_state_ == RLS::Init ||
                              self->last_room_list_state_ == RLS::SettingUp))
    {
        self->sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(self->status_bar_),
                           _("Syncing rooms\xe2\x80\xa6"));
    }
    return G_SOURCE_REMOVE;
}

void MainWindow::refresh_sync_status()
{
    if (!status_bar_)
    {
        return;
    }
    using RLS = tesseract::RoomListState;
    using BS = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init ||
                            last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy)
    {
        if (!sync_progress_shown_ && sync_status_debounce_id_ == 0)
        {
            sync_status_debounce_id_ =
                g_timeout_add(300, on_sync_status_debounce_, this);
        }
        else if (sync_progress_shown_)
        {
            gtk_label_set_text(GTK_LABEL(status_bar_),
                               _("Syncing rooms\xe2\x80\xa6"));
        }
        return;
    }

    if (sync_status_debounce_id_ != 0)
    {
        g_source_remove(sync_status_debounce_id_);
        sync_status_debounce_id_ = 0;
    }

    if (reconnecting)
    {
        sync_progress_shown_ = true;
        gtk_label_set_text(GTK_LABEL(status_bar_),
                           _("Reconnecting\xe2\x80\xa6"));
        return;
    }
    if (keys_busy)
    {
        sync_progress_shown_ = true;
        std::string msg = std::string(_("Downloading encryption keys (")) +
                          std::to_string(last_imported_keys_) + ")\xe2\x80\xa6";
        gtk_label_set_text(GTK_LABEL(status_bar_), msg.c_str());
        return;
    }
    if (sync_progress_shown_)
    {
        sync_progress_shown_ = false;
        gtk_label_set_text(GTK_LABEL(status_bar_), _("Connected"));
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populate_user_strip()
{
    if (!main_app_)
    {
        return;
    }
    auto* ui = main_app_->user_info();
    std::string shown =
        my_display_name_.empty() ? my_user_id_ : my_display_name_;
    ui->set_display_name(shown);
    ui->set_user_id(my_user_id_);
    ui->set_avatar_url(my_avatar_url_);
    ui->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            auto it = tk_avatars_.find(mxc);
            return it == tk_avatars_.end() ? nullptr : it->second.get();
        });
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }

    // Kick off avatar fetch if not yet cached (result arrives via
    // on_media_bytes_ready_ → tk_avatars_ → relayout).
    if (!my_avatar_url_.empty() && client_)
    {
        ensure_user_avatar_(my_avatar_url_);
    }
}

void MainWindow::on_add_account_activate_(GSimpleAction* /*action*/,
                                          GVariant* /*parameter*/,
                                          gpointer user_data)
{
    gtk_popover_popdown(
        GTK_POPOVER(static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->begin_add_account();
}

void MainWindow::on_logout_activate_(GSimpleAction* /*action*/,
                                     GVariant* /*parameter*/,
                                     gpointer user_data)
{
    gtk_popover_popdown(
        GTK_POPOVER(static_cast<MainWindow*>(user_data)->user_popover_));
    static_cast<MainWindow*>(user_data)->logout_active_account();
}

void MainWindow::on_settings_activate_(GSimpleAction* /*action*/,
                                       GVariant* /*param*/, gpointer self)
{
    static_cast<MainWindow*>(self)->open_settings_();
}

void MainWindow::on_quit_user_activate_(GSimpleAction* /*action*/,
                                        GVariant* /*parameter*/,
                                        gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    gtk_popover_popdown(GTK_POPOVER(self->user_popover_));
    self->tray_.reset();
    g_application_quit(G_APPLICATION(self->app_));
}

void MainWindow::open_settings_()
{
    settings_widget_->populate(
        my_display_name_, my_user_id_, my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        {
            auto it = tk_avatars_.find(mxc);
            return (it != tk_avatars_.end()) ? it->second.get() : nullptr;
        },
        tesseract::Settings::instance().theme_pref,
        tesseract::Settings::instance().notifications_enabled);
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "settings");
}

void MainWindow::do_logout()
{
    logout_active_account();
}

// ---------------------------------------------------------------------------
// Shortcode popup — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::ShortcodePopup suggestion list.
// ---------------------------------------------------------------------------

void MainWindow::show_shortcode_popup_(
    const std::vector<tesseract::views::ShortcodeSuggestion>& suggestions,
    tk::Rect cursor_local)
{
    if (!shortcode_popover_)
    {
        shortcode_popover_ = gtk_popover_new();
        gtk_widget_set_parent(shortcode_popover_, main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(shortcode_popover_), GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(shortcode_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(shortcode_popover_), TRUE);

        shortcode_popup_surface_ =
            std::make_unique<tk::gtk4::Surface>(main_app_surface_->theme());

        auto popup_widget =
            std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = popup_widget.get();
        shortcode_popup_surface_->set_root(std::move(popup_widget));

        shortcode_popup_widget_->on_accepted =
            [this](tesseract::views::ShortcodeSuggestion s)
        {
            std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
            room_text_area_->replace_range(shortcode_active_match_.start,
                                           shortcode_active_match_.end,
                                           std::move(r));
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->on_dismissed = [this]
        {
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });

        GtkWidget* surface_widget = shortcode_popup_surface_->widget();
        gtk_popover_set_child(GTK_POPOVER(shortcode_popover_), surface_widget);
    }

    shortcode_popup_widget_->set_suggestions(suggestions);

    int rows = std::min((int)suggestions.size(),
                        (int)tesseract::views::ShortcodePopup::kMaxRows);
    int w = int(tesseract::views::ShortcodePopup::kWidth);
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    gtk_widget_set_size_request(shortcode_popup_surface_->widget(), w, h);

    GdkRectangle rect{int(cursor_local.x), int(cursor_local.y),
                      int(cursor_local.w), int(cursor_local.h)};
    gtk_popover_set_pointing_to(GTK_POPOVER(shortcode_popover_), &rect);
    gtk_popover_popup(GTK_POPOVER(shortcode_popover_));
}

void MainWindow::hide_shortcode_popup_()
{
    if (shortcode_popover_)
    {
        gtk_popover_popdown(GTK_POPOVER(shortcode_popover_));
    }
    if (room_text_area_)
    {
        room_text_area_->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Emoji picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::EmojiPicker. The search row is a native
// GtkEntry overlaid by the Surface; selection routes back through the
// shared widget's on_selected callback.
// ---------------------------------------------------------------------------

void MainWindow::build_emoji_popover()
{
    emoji_popover_ = gtk_popover_new();
    gtk_widget_set_parent(emoji_popover_, main_app_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(emoji_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(emoji_popover_), TRUE);

    emoji_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::EmojiPicker>();
    emoji_picker_shared_ = shared.get();
    emoji_picker_shared_->set_client(client_);
    emoji_picker_shared_->on_selected = [this](const std::string& glyph)
    {
        emoji_selected(glyph);
    };
    // Async fetch for custom emoticon images — mirrors the sticker picker.
    emoji_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
               const std::string& /*source_token*/) -> const tk::Image*
        {
            if (const auto* f = anim_cache_.current_frame(cache_key))
            {
                return f;
            }
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end())
            {
                return it->second.get();
            }
            ensure_picker_image_(cache_key, /*is_sticker=*/false);
            return nullptr;
        });
    emoji_picker_surface_->set_root(std::move(shared));

    // Native GtkEntry overlay for the search row. The shared widget paints
    // the affordance; the entry handles IME + selection natively.
    emoji_picker_search_field_ =
        emoji_picker_surface_->host().make_text_field();
    emoji_picker_search_field_->set_placeholder(_("Search emoji"));
    emoji_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (emoji_picker_shared_)
            {
                emoji_picker_shared_->set_search_query(q);
            }
            if (emoji_picker_surface_)
            {
                emoji_picker_surface_->relayout();
            }
        });
    emoji_picker_surface_->set_on_layout(
        [this]
        {
            if (emoji_picker_search_field_ && emoji_picker_shared_)
            {
                emoji_picker_search_field_->set_rect(
                    emoji_picker_shared_->search_field_rect());
            }
        });

    // The popover content area is the surface widget. Size to a sensible
    // default; the surface relayouts on resize.
    GtkWidget* surface_widget = emoji_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 320, 360);
    gtk_popover_set_child(GTK_POPOVER(emoji_popover_), surface_widget);
}

// ---------------------------------------------------------------------------
// Sticker picker — GtkPopover hosting a tk::gtk4::Surface that paints the
// shared tesseract::views::StickerPicker. Mirrors the emoji popover.
// ---------------------------------------------------------------------------

void MainWindow::build_sticker_popover()
{
    sticker_popover_ = gtk_popover_new();
    gtk_widget_set_parent(sticker_popover_, main_app_surface_->widget());
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(sticker_popover_), TRUE);

    sticker_picker_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::StickerPicker>();
    sticker_picker_shared_ = shared.get();
    sticker_picker_shared_->set_client(client_);
    sticker_picker_shared_->on_selected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (current_room_id_.empty())
        {
            return;
        }
        std::string body = img.body.empty() ? img.shortcode : img.body;
        client_->send_sticker(current_room_id_, body, img.url, img.info_json);
        if (sticker_popover_)
        {
            gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        }
    };
    // Share the same caches the message list reads from. Animated
    // entries take priority; static entries are the second hop; on
    // miss kick off an async fetch via the shared `ensure_picker_image_`
    // so the next paint after the worker posts back finds the bitmap.
    sticker_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
               const std::string& /*source_token*/) -> const tk::Image*
        {
            if (const auto* f = anim_cache_.current_frame(cache_key))
            {
                return f;
            }
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end())
            {
                return it->second.get();
            }
            ensure_picker_image_(cache_key, /*is_sticker=*/true);
            return nullptr;
        });
    sticker_picker_surface_->set_root(std::move(shared));

    sticker_picker_search_field_ =
        sticker_picker_surface_->host().make_text_field();
    sticker_picker_search_field_->set_placeholder(_("Search stickers"));
    sticker_picker_search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (sticker_picker_shared_)
            {
                sticker_picker_shared_->set_search_query(q);
            }
            if (sticker_picker_surface_)
            {
                sticker_picker_surface_->relayout();
            }
        });
    sticker_picker_surface_->set_on_layout(
        [this]
        {
            if (sticker_picker_search_field_ && sticker_picker_shared_)
            {
                sticker_picker_search_field_->set_rect(
                    sticker_picker_shared_->search_field_rect());
            }
        });

    GtkWidget* surface_widget = sticker_picker_surface_->widget();
    gtk_widget_set_size_request(surface_widget, 360, 420);
    gtk_popover_set_child(GTK_POPOVER(sticker_popover_), surface_widget);
}

void MainWindow::toggle_sticker_picker()
{
    if (!sticker_popover_)
    {
        return;
    }
    if (gtk_widget_get_visible(sticker_popover_))
    {
        gtk_popover_popdown(GTK_POPOVER(sticker_popover_));
        return;
    }
    GtkWidget* desired_parent =
        main_app_surface_ ? main_app_surface_->widget() : nullptr;
    if (desired_parent &&
        gtk_widget_get_parent(sticker_popover_) != desired_parent)
    {
        gtk_widget_unparent(sticker_popover_);
        gtk_widget_set_parent(sticker_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), nullptr);
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

// ---------------------------------------------------------------------------
// Sticker context menu — right-click on a sticker row offers
// "Add to Saved Stickers" (suppressed for stickers already saved).
// ---------------------------------------------------------------------------

void MainWindow::build_sticker_context_menu()
{
    GMenu* menu = g_menu_new();
    g_menu_append(menu, _("Add to Saved Stickers"), "sticker.save");

    sticker_ctx_menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(sticker_ctx_menu_), FALSE);
    gtk_widget_set_parent(sticker_ctx_menu_, main_app_surface_->widget());
    g_object_unref(menu);

    sticker_ctx_actions_ = g_simple_action_group_new();
    GSimpleAction* save = g_simple_action_new("save", nullptr);
    g_signal_connect(save, "activate", G_CALLBACK(on_sticker_save_activate_),
                     this);
    g_action_map_add_action(G_ACTION_MAP(sticker_ctx_actions_), G_ACTION(save));
    g_object_unref(save);
    gtk_widget_insert_action_group(main_app_surface_->widget(), "sticker",
                                   G_ACTION_GROUP(sticker_ctx_actions_));
}

void MainWindow::on_msg_right_click_(GtkGestureClick* gesture, int /*n_press*/,
                                     double x, double y, gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->room_view_ || !self->sticker_ctx_menu_)
    {
        return;
    }

    auto hit = self->room_view_->message_list()->sticker_hit_at(
        tk::Point{static_cast<float>(x), static_cast<float>(y)});
    if (!hit)
    {
        return;
    }

    // Claim the gesture so the underlying surface doesn't also process it
    // (e.g. as a drag-start or text-selection event).
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Capture sticker fields for the action handler. The hit_at result
    // points into MessageListView's per-frame sticker_geom_ map and would
    // dangle by the time the action fires.
    self->ctx_sticker_event_id_ = hit->event_id;
    self->ctx_sticker_mxc_url_ = hit->mxc_url;
    self->ctx_sticker_body_ = hit->body;
    self->ctx_sticker_info_json_ = hit->info_json;

    // Disable the action when the sticker is already saved so the menu item
    // renders grayed-out rather than the menu being suppressed entirely.
    {
        const bool already_saved =
            self->client_->user_pack_has_sticker(hit->mxc_url);
        GAction* act = g_action_map_lookup_action(
            G_ACTION_MAP(self->sticker_ctx_actions_), "save");
        if (act)
        {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(act), !already_saved);
        }
    }

    GdkRectangle r{.x = static_cast<int>(x),
                   .y = static_cast<int>(y),
                   .width = 1,
                   .height = 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(self->sticker_ctx_menu_), &r);
    gtk_popover_popup(GTK_POPOVER(self->sticker_ctx_menu_));
}

gboolean MainWindow::on_window_key_pressed_(GtkEventControllerKey*,
                                            guint keyval, guint,
                                            GdkModifierType, gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (keyval == GDK_KEY_Escape)
    {
        if (self->vid_viewer_ && self->vid_viewer_->is_open())
        {
            self->vid_viewer_->close();
            if (self->main_app_)
            {
                self->main_app_->show_video_viewer(false);
            }
            if (self->main_app_surface_)
            {
                self->main_app_surface_->relayout();
            }
            return TRUE;
        }
        if (self->img_viewer_ && self->img_viewer_->is_open())
        {
            self->img_viewer_->close();
            if (self->main_app_)
            {
                self->main_app_->show_image_viewer(false);
            }
            if (self->main_app_surface_)
            {
                self->main_app_surface_->relayout();
            }
            return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::on_sticker_save_activate_(GSimpleAction* /*action*/,
                                           GVariant* /*parameter*/,
                                           gpointer user_data)
{
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->ctx_sticker_mxc_url_.empty())
    {
        return;
    }
    auto res = self->client_->save_sticker_to_user_pack(
        self->ctx_sticker_body_, self->ctx_sticker_body_,
        self->ctx_sticker_mxc_url_, self->ctx_sticker_info_json_);
    if (!res.ok)
    {
        self->push_error(res.message);
    }
    self->ctx_sticker_event_id_.clear();
    self->ctx_sticker_mxc_url_.clear();
    self->ctx_sticker_body_.clear();
    self->ctx_sticker_info_json_.clear();
    if (self->sticker_ctx_menu_)
    {
        gtk_popover_popdown(GTK_POPOVER(self->sticker_ctx_menu_));
    }
}

void MainWindow::toggle_emoji_picker()
{
    if (!emoji_popover_)
    {
        return;
    }
    if (gtk_widget_get_visible(emoji_popover_))
    {
        gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        return;
    }
    // Compose-bar path: ensure the popover is parented to the main surface
    // and clear any prior `pointing_to` from a reaction popup.
    GtkWidget* desired_parent =
        main_app_surface_ ? main_app_surface_->widget() : nullptr;
    if (desired_parent &&
        gtk_widget_get_parent(emoji_popover_) != desired_parent)
    {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, desired_parent);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), nullptr);
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

void MainWindow::popup_emoji_at_rect(GtkWidget* parent, tk::Rect local_rect)
{
    if (!emoji_popover_ || !parent)
    {
        return;
    }
    // Reparent the popover to the target widget so `pointing_to` is
    // interpreted in that widget's coordinate space.
    if (gtk_widget_get_parent(emoji_popover_) != parent)
    {
        gtk_widget_unparent(emoji_popover_);
        gtk_widget_set_parent(emoji_popover_, parent);
    }
    GdkRectangle r{
        .x = static_cast<int>(local_rect.x),
        .y = static_cast<int>(local_rect.y),
        .width = static_cast<int>(local_rect.w),
        .height = static_cast<int>(local_rect.h),
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(emoji_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(emoji_popover_), GTK_POS_TOP);
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->refresh_frequents();
    }
    if (emoji_picker_search_field_)
    {
        emoji_picker_search_field_->set_text("");
    }
    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(emoji_popover_));
    if (emoji_picker_surface_)
    {
        emoji_picker_surface_->relayout();
    }
}

void MainWindow::popup_sticker_at_rect(GtkWidget* parent, tk::Rect local_rect)
{
    if (!sticker_popover_ || !parent)
    {
        return;
    }
    if (gtk_widget_get_parent(sticker_popover_) != parent)
    {
        gtk_widget_unparent(sticker_popover_);
        gtk_widget_set_parent(sticker_popover_, parent);
    }
    GdkRectangle r{
        .x = static_cast<int>(local_rect.x),
        .y = static_cast<int>(local_rect.y),
        .width = static_cast<int>(local_rect.w),
        .height = static_cast<int>(local_rect.h),
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(sticker_popover_), &r);
    gtk_popover_set_position(GTK_POPOVER(sticker_popover_), GTK_POS_TOP);
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->refresh_packs();
    }
    if (sticker_picker_search_field_)
    {
        sticker_picker_search_field_->set_text("");
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_search_query("");
    }
    gtk_popover_popup(GTK_POPOVER(sticker_popover_));
    if (sticker_picker_surface_)
    {
        sticker_picker_surface_->relayout();
    }
}

void MainWindow::emoji_selected(const std::string& glyph)
{
    // Reaction mode: a "+" chip set pending_reaction_event_id_ before
    // opening the picker. Route the glyph through send_reaction
    // (Rust-side toggle) and skip the compose insert.
    if (!pending_reaction_event_id_.empty())
    {
        std::string ev = std::move(pending_reaction_event_id_);
        pending_reaction_event_id_.clear();
        if (!current_room_id_.empty())
        {
            client_->send_reaction(current_room_id_, ev, glyph);
        }
        if (emoji_popover_)
        {
            gtk_popover_popdown(GTK_POPOVER(emoji_popover_));
        }
        return;
    }
    if (!room_text_area_)
    {
        return;
    }
    room_text_area_->insert_at_cursor(glyph);
    if (room_view_)
    {
        room_view_->set_current_text(room_text_area_->text());
    }
    room_text_area_->set_focused(true);
    // The shared picker already calls recent_emoji_bump before invoking
    // this callback. Keep the popover open so users can pick several.
}

// ---------------------------------------------------------------------------
// Multi-account management
// ---------------------------------------------------------------------------

void MainWindow::switch_active_account(int new_idx)
{
    // Unsubscribe the previous account's open room and drop per-account,
    // room-id-keyed state so it can't bleed into the next account.
    if (client_ && !current_room_id_.empty() &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }
    current_room_id_.clear();
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    message_cache_.clear();
    message_cache_lru_.clear();
    clear_messages();

    active_account_index_ = new_idx;
    auto& sess = *accounts_[new_idx];

    client_ = sess.client.get();
    event_handler_ = sess.bridge.get();

    my_user_id_ = sess.user_id;
    my_display_name_ = sess.display_name;
    my_avatar_url_ = sess.avatar_url;
    pending_restore_room_ = sess.last_room;

    populate_user_strip();

    if (emoji_picker_shared_)
    {
        emoji_picker_shared_->set_client(client_);
    }
    if (sticker_picker_shared_)
    {
        sticker_picker_shared_->set_client(client_);
    }

    // Load room snapshot for this account.
    auto it = per_account_rooms_.find(my_user_id_);
    if (it != per_account_rooms_.end())
    {
        rooms_ = it->second;
        refresh_room_list();
    }
    else
    {
        rooms_.clear();
        refresh_room_list();
    }

    // Rewrite accounts.json active pointer.
    auto index = tesseract::SessionStore::load_index();
    index.active_user_id = my_user_id_;
    tesseract::SessionStore::save_index(index);

    rebuild_account_picker();
}

void MainWindow::begin_add_account()
{
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    login_view_->set_client(pending_login_client_.get());
    login_view_->set_on_begin_oauth(
        [this]
        {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" + std::to_string(ts));
            pending_login_client_->set_data_dir(
                (pending_login_temp_dir_ / "matrix-store").string());
        });
    login_view_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    login_view_->reset();
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
}

void MainWindow::logout_active_account()
{
    if (active_account_index_ < 0)
    {
        return;
    }

    auto& sess = *accounts_[active_account_index_];
    // Copy before the accounts_.erase() below: `sess` dangles afterward but
    // the user id is still needed to prune accounts.json.
    const std::string logged_out_uid = sess.user_id;

    if (!current_room_id_.empty())
    {
        if (room_subscription_refs_.count(current_room_id_) == 0)
        {
            client_->unsubscribe_room(current_room_id_);
        }
        current_room_id_.clear();
    }

    if (sess.up_connector)
    {
        sess.up_connector->logout();
    }
    auto res = client_->logout();
    client_->stop_sync();

    tesseract::SessionStore::clear_account(sess.user_id);

    // Remove from the accounts vector.
    accounts_.erase(accounts_.begin() + active_account_index_);

    // Reset UI state.
    clear_messages();
    rooms_.clear();
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    message_cache_.clear();
    message_cache_lru_.clear();
    refresh_room_list();
    if (main_app_)
    {
        main_app_->show_recovery_banner(false);
        if (main_app_surface_)
        {
            main_app_surface_->relayout();
        }
    }
    recovery_banner_dismissed_ = false;

    gtk_label_set_text(
        GTK_LABEL(status_bar_),
        res ? _("Signed out")
            : (std::string(_("Sign out failed: ")) + res.message).c_str());

    if (accounts_.empty())
    {
        client_ = nullptr;
        event_handler_ = nullptr;
        active_account_index_ = -1;
        my_user_id_.clear();
        my_display_name_.clear();
        my_avatar_url_.clear();

        // Update accounts.json.
        tesseract::SessionStore::AccountIndex idx;
        tesseract::SessionStore::save_index(idx);

        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        login_view_->set_client(pending_login_client_.get());
        login_view_->set_on_begin_oauth(
            [this]
            {
                if (!pending_login_temp_dir_.empty())
                {
                    return;
                }
                auto ts =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                    "pending-" + std::to_string(ts));
                pending_login_client_->set_data_dir(
                    (pending_login_temp_dir_ / "matrix-store").string());
            });
        login_view_->set_mode(tesseract::views::LoginView::Mode::Initial);
        login_view_->reset();
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "login");
    }
    else
    {
        // Switch to the closest remaining account.
        int next = std::min(static_cast<int>(accounts_.size()) - 1,
                            active_account_index_);
        active_account_index_ =
            -1; // reset so switch_active_account does full rebind
        switch_active_account(next);

        auto idx = tesseract::SessionStore::load_index();
        idx.active_user_id = my_user_id_;
        // Remove logged-out uid from index.
        idx.user_ids.erase(std::remove(idx.user_ids.begin(), idx.user_ids.end(),
                                       logged_out_uid),
                           idx.user_ids.end());
        tesseract::SessionStore::save_index(idx);
    }
}

void MainWindow::on_login_cancelled()
{
    pending_login_client_.reset();
    if (!pending_login_temp_dir_.empty())
    {
        std::filesystem::remove_all(pending_login_temp_dir_);
        pending_login_temp_dir_.clear();
    }

    if (pending_login_is_add_account_ && add_account_return_idx_ >= 0)
    {
        switch_active_account(add_account_return_idx_);
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "main");
    }
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
}

void MainWindow::rebuild_account_picker()
{
    if (!account_picker_)
    {
        return;
    }
    std::vector<tesseract::views::AccountEntry> entries;
    entries.reserve(accounts_.size());
    for (const auto& sess : accounts_)
    {
        tesseract::views::AccountEntry e;
        e.user_id = sess->user_id;
        e.display_name = sess->display_name;
        e.avatar_url = sess->avatar_url;
        e.active = (sess->user_id == my_user_id_);
        entries.push_back(std::move(e));
    }
    account_picker_->set_entries(std::move(entries));
    if (account_picker_surface_)
    {
        account_picker_surface_->relayout();
    }
}

void MainWindow::open_account_picker(double /*ax*/, double /*ay*/)
{
    if (accounts_.size() < 2)
    {
        return;
    }

    if (!account_picker_popover_)
    {
        // Build once; a GtkPopover parented to the user strip.
        account_picker_surface_ =
            std::make_unique<tk::gtk4::Surface>(tk::Theme::light());
        auto picker = std::make_unique<tesseract::views::AccountPicker>();
        account_picker_ = picker.get();
        account_picker_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image*
            {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        account_picker_->on_select = [this](const std::string& uid)
        {
            if (account_picker_popover_)
            {
                gtk_popover_popdown(GTK_POPOVER(account_picker_popover_));
            }
            // Find the index of this account.
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
            {
                if (accounts_[i]->user_id == uid)
                {
                    switch_active_account(i);
                    break;
                }
            }
        };
        account_picker_surface_->set_root(std::move(picker));

        account_picker_popover_ = gtk_popover_new();
        gtk_popover_set_child(GTK_POPOVER(account_picker_popover_),
                              account_picker_surface_->widget());
        gtk_widget_set_parent(account_picker_popover_,
                              main_app_surface_->widget());
        gtk_popover_set_position(GTK_POPOVER(account_picker_popover_),
                                 GTK_POS_TOP);
        gtk_popover_set_has_arrow(GTK_POPOVER(account_picker_popover_), FALSE);
        gtk_popover_set_autohide(GTK_POPOVER(account_picker_popover_), TRUE);

        // Size to fit rows.
        const int row_h = 48;
        gtk_widget_set_size_request(account_picker_surface_->widget(), 240,
                                    row_h * static_cast<int>(accounts_.size()));
    }

    rebuild_account_picker();
    gtk_popover_popup(GTK_POPOVER(account_picker_popover_));
}

// ---------------------------------------------------------------------------
// Join room dialog — transient GtkWindow hosting JoinRoomView.
// ---------------------------------------------------------------------------

void MainWindow::build_join_room_dialog()
{
    join_room_dialog_window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(join_room_dialog_window_),
                         _("Join a Room"));
    gtk_window_set_modal(GTK_WINDOW(join_room_dialog_window_), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(join_room_dialog_window_),
                                 GTK_WINDOW(window_));
    gtk_window_set_resizable(GTK_WINDOW(join_room_dialog_window_), FALSE);
    gtk_window_set_default_size(
        GTK_WINDOW(join_room_dialog_window_),
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredW),
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredH));

    join_room_surface_ =
        std::make_unique<tk::gtk4::Surface>(tk::Theme::light());

    auto jrv = std::make_unique<tesseract::views::JoinRoomView>();
    join_room_shared_ = jrv.get();

    join_room_shared_->set_avatar_provider(
        [this](const std::string& mxc_url) -> const tk::Image*
        {
            auto it = tk_avatars_.find(mxc_url);
            return (it != tk_avatars_.end()) ? it->second.get() : nullptr;
        });

    join_room_shared_->on_lookup_requested = [this](const std::string& alias)
    {
        if (!client_ || alias.empty())
        {
            return;
        }
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Loading);
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        uint32_t gen = join_room_gen_;
        auto snap = client_;
        run_async_(
            [this, alias, gen, snap]
            {
                tesseract::RoomSummary s = snap->get_room_summary(alias);
                post_to_ui_(
                    [this, s = std::move(s), gen]
                    {
                        if (!join_room_shared_ || join_room_gen_ != gen)
                        {
                            return;
                        }
                        if (s.ok())
                        {
                            join_room_shared_->set_preview(s);
                        }
                        else
                        {
                            join_room_shared_->set_error(_("Room not found."));
                        }
                        if (join_room_surface_)
                        {
                            join_room_surface_->relayout();
                        }
                    });
            });
    };

    join_room_shared_->on_join_requested =
        [this](const std::string& room_id_or_alias)
    {
        if (!client_ || room_id_or_alias.empty())
        {
            return;
        }
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Joining);
        if (join_room_surface_)
        {
            join_room_surface_->relayout();
        }
        uint32_t gen = join_room_gen_;
        auto snap = client_;
        run_async_(
            [this, room_id_or_alias, gen, snap]
            {
                std::string canonical_id = snap->join_room(room_id_or_alias);
                post_to_ui_(
                    [this, canonical_id, gen]
                    {
                        if (!join_room_shared_ || join_room_gen_ != gen)
                        {
                            return;
                        }
                        if (!canonical_id.empty())
                        {
                            if (join_room_dialog_window_)
                            {
                                gtk_widget_set_visible(join_room_dialog_window_,
                                                       FALSE);
                            }
                            navigate_to_room(canonical_id);
                        }
                        else
                        {
                            join_room_shared_->set_error(_("Join failed."));
                            if (join_room_surface_)
                            {
                                join_room_surface_->relayout();
                            }
                        }
                    });
            });
    };

    join_room_shared_->on_cancel = [this]
    {
        if (join_room_dialog_window_)
        {
            gtk_widget_set_visible(join_room_dialog_window_, FALSE);
        }
    };

    join_room_surface_->set_root(std::move(jrv));

    join_room_alias_field_ = join_room_surface_->host().make_text_field();
    join_room_alias_field_->set_placeholder(_("#room:server.org"));
    join_room_alias_field_->set_on_changed(
        [this](const std::string& text)
        {
            if (join_room_shared_)
            {
                join_room_shared_->set_alias_text(text);
            }
        });
    join_room_surface_->set_on_layout(
        [this]
        {
            if (join_room_alias_field_ && join_room_shared_)
            {
                join_room_alias_field_->set_rect(
                    join_room_shared_->alias_field_rect());
                join_room_alias_field_->set_visible(
                    join_room_shared_->alias_field_visible());
            }
        });

    GtkWidget* surface_widget = join_room_surface_->widget();
    gtk_window_set_child(GTK_WINDOW(join_room_dialog_window_), surface_widget);
}

void MainWindow::open_join_room_dialog()
{
    if (!join_room_dialog_window_)
    {
        return;
    }

    ++join_room_gen_; // invalidate any in-flight lookup/join callbacks

    if (join_room_shared_)
    {
        join_room_shared_->set_state(
            tesseract::views::JoinRoomView::State::Idle);
        join_room_shared_->set_alias_text("");
    }
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_text("");
    }
    if (join_room_surface_)
    {
        join_room_surface_->relayout();
    }

    gtk_window_present(GTK_WINDOW(join_room_dialog_window_));
    if (join_room_alias_field_)
    {
        join_room_alias_field_->set_focused(true);
    }
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    if (!main_app_)
    {
        return;
    }

    auto* tb = main_app_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    main_app_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string name;
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id)
                {
                    continue;
                }
                name = r.name;
                if (!r.avatar_url.empty())
                {
                    auto it = tk_avatars_.find(r.avatar_url);
                    if (it != tk_avatars_.end())
                    {
                        avatar = it->second.get();
                    }
                }
                break;
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    if (active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        try_restore_message_cache_(active.room_id);
        on_room_selected(active.room_id);
        if (!active.compose_draft.empty())
        {
            if (room_text_area_)
            {
                room_text_area_->set_text(active.compose_draft);
            }
            if (room_view_)
            {
                room_view_->set_current_text(active.compose_draft);
            }
        }
    }

    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

float MainWindow::get_message_scroll_fraction_()
{
    if (!room_view_ || !room_view_->message_list())
    {
        return 0.f;
    }
    return room_view_->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t)
{
    if (!room_view_ || !room_view_->message_list())
    {
        return;
    }
    room_view_->message_list()->scroll_to_offset(t);
}

std::string MainWindow::get_compose_draft_()
{
    if (!room_view_ || !room_view_->compose_bar())
    {
        return {};
    }
    return room_view_->compose_bar()->current_text();
}

void MainWindow::set_compose_draft_(const std::string& draft)
{
    if (room_text_area_)
    {
        room_text_area_->set_text(draft);
    }
    if (room_view_)
    {
        room_view_->set_current_text(draft);
    }
}

const std::vector<tesseract::views::MessageRowData>* MainWindow::get_current_messages_()
{
    auto* ml = room_view_ ? room_view_->message_list() : nullptr;
    return ml ? &ml->messages() : nullptr;
}

void MainWindow::apply_cached_messages_(
    const std::vector<tesseract::views::MessageRowData>& msgs)
{
    if (room_view_)
    {
        room_view_->set_messages(msgs, /*room_switch=*/false);
    }
    if (main_app_surface_)
    {
        main_app_surface_->relayout();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

} // namespace gtk4
