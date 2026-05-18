#include "RoomWindow.h"
#include "MainWindow.h"
#include "views/RoomView.h"
#include "views/text_util.h"
#include <tesseract/client.h>

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

    auto room_root = std::make_unique<tesseract::views::RoomView>();
    room_view_ = room_root.get();
    surface_->set_root(std::move(room_root));

    // ── RoomView providers ────────────────────────────────────────────────
    room_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_avatar_(mxc);
        });
    room_view_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_image_(mxc);
        });
    room_view_->set_preview_provider(
        [this](
            const std::string& url) -> const tesseract::views::UrlPreviewData*
        {
            auto it = parent_shell_->url_preview_data_.find(url);
            return it == parent_shell_->url_preview_data_.end() ? nullptr
                                                                : &it->second;
        });
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }
    room_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t>
        {
            return fetch_source_bytes_(source_json);
        });

    // ── Repaint / layout ─────────────────────────────────────────────────
    room_view_->set_repaint_requester(
        [this]
        {
            if (surface_)
            {
                gtk_widget_queue_draw(surface_->widget());
            }
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

    // ── Compose callbacks ────────────────────────────────────────────────
    room_view_->on_send = [this](const std::string& body)
    {
        std::string trimmed = tesseract::text::trim(body);
        if (trimmed.empty())
        {
            return;
        }
        send_message_(trimmed);
        room_view_->set_current_text({});
    };
    room_view_->on_send_reply =
        [this](const std::string& reply_id, const std::string& body)
    {
        if (body.empty())
        {
            return;
        }
        send_reply_(reply_id, body);
        room_view_->set_current_text({});
    };
    room_view_->on_send_edit =
        [this](const std::string& event_id, const std::string& new_body)
    {
        if (new_body.empty())
        {
            return;
        }
        send_edit_(event_id, new_body);
        room_view_->set_current_text({});
    };
    room_view_->on_edit_cancelled = [this]
    {
        room_view_->set_current_text({});
    };
    room_view_->on_edit_prefill = [this](const std::string& body)
    {
        room_view_->set_current_text(body);
    };
    room_view_->on_reply_focus =
        [] { /* no NativeTextArea on GTK4 secondary */ };
    room_view_->on_delete_requested = [this](const std::string& event_id)
    {
        delete_event_(event_id);
    };
    room_view_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key)
    {
        toggle_reaction_(event_id, key);
    };
    room_view_->on_receipt_needed = [this](const std::string& event_id)
    {
        send_receipt_(event_id);
    };
    room_view_->on_link_clicked = [](const std::string& url)
    {
        tesseract::Client::open_in_browser(url);
    };
    room_view_->on_near_top = [this]
    {
        request_pagination_back_();
    };

    // "destroy" fires when the GtkWindow is destroyed (user clicks X or
    // gtk_window_destroy is called). At that point the GtkWidget tree is
    // already gone; schedule the C++ object deletion for next idle.
    g_signal_connect(window_, "destroy", G_CALLBACK(on_destroy_), this);

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
void RoomWindow::on_destroy_(GtkWidget* /*widget*/, gpointer self)
{
    auto* w = static_cast<RoomWindow*>(self);
    w->window_ = nullptr; // already destroyed; prevent double-destroy in dtor
    w->schedule_self_close_();
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

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace gtk4
