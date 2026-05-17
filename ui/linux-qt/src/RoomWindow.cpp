#include "RoomWindow.h"
#include "MainWindow.h"

#include "views/RoomView.h"
#include <tesseract/client.h>

#include <QCloseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace qt6 {

RoomWindow::RoomWindow(MainWindow* parent_shell, const std::string& room_id)
    : QWidget(nullptr, Qt::Window)
    , tesseract::RoomWindowBase(parent_shell, room_id)
    , parent_shell_(parent_shell)
{
    setAttribute(Qt::WA_DeleteOnClose, false);  // we manage lifetime via unique_ptr
    resize(800, 600);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    auto room_root = std::make_unique<tesseract::views::RoomView>();
    room_view_ = room_root.get();
    surface_->set_root(std::move(room_root));

    // ── RoomView providers ────────────────────────────────────────────────
    room_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            return shell_avatar_(mxc);
        });
    room_view_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            return shell_image_(mxc);
        });
    room_view_->set_preview_provider(
        [this](const std::string& url) -> const tesseract::views::UrlPreviewData* {
            auto it = parent_shell_->url_preview_data_.find(url);
            return it == parent_shell_->url_preview_data_.end() ? nullptr : &it->second;
        });
    if (auto player = surface_->host().make_audio_player())
        room_view_->set_audio_player(std::move(player));
    room_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return fetch_source_bytes_(source_json);
        });

    // ── Repaint / layout ─────────────────────────────────────────────────
    room_view_->set_repaint_requester([this] {
        if (surface_) surface_->update();
    });
    room_view_->on_layout_changed = [this] {
        if (surface_) surface_->relayout();
    };

    // ── Compose callbacks ────────────────────────────────────────────────
    room_view_->on_send = [this](const std::string& body) {
        std::string trimmed = body;
        auto l = trimmed.find_first_not_of(" \t\n\r");
        auto r = trimmed.find_last_not_of (" \t\n\r");
        if (l == std::string::npos) return;
        trimmed = trimmed.substr(l, r - l + 1);
        if (trimmed.empty()) return;
        send_message_(trimmed);
        room_view_->set_current_text({});
    };
    room_view_->on_send_reply = [this](const std::string& reply_id,
                                        const std::string& body) {
        if (body.empty()) return;
        send_reply_(reply_id, body);
        room_view_->set_current_text({});
    };
    room_view_->on_send_edit = [this](const std::string& event_id,
                                       const std::string& new_body) {
        if (new_body.empty()) return;
        send_edit_(event_id, new_body);
        room_view_->set_current_text({});
    };
    room_view_->on_edit_cancelled = [this] { room_view_->set_current_text({}); };
    room_view_->on_edit_prefill = [this](const std::string& body) {
        room_view_->set_current_text(body);
    };
    room_view_->on_reply_focus = [] { /* no NativeTextArea on Qt6 secondary */ };
    room_view_->on_delete_requested = [this](const std::string& event_id) {
        delete_event_(event_id);
    };
    room_view_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            toggle_reaction_(event_id, key);
        };
    room_view_->on_receipt_needed = [this](const std::string& event_id) {
        send_receipt_(event_id);
    };
    room_view_->on_link_clicked = [](const std::string& url) {
        tesseract::Client::open_in_browser(url);
    };
    room_view_->on_near_top = [this] { request_pagination_back_(); };

    // NativeTextArea is not used here: Qt6's host creates it lazily if needed.
    // The compose bar's built-in QTextEdit handles input on this platform.

    show();
    finish_init_();
}

RoomWindow::~RoomWindow() = default;

// ---------------------------------------------------------------------------

void RoomWindow::bring_to_front() {
    raise();
    activateWindow();
}

void RoomWindow::close_window() {
    close();
}

void RoomWindow::request_relayout() {
    if (surface_) { surface_->relayout(); surface_->update(); }
}

void RoomWindow::update_window_title_(const std::string& name) {
    setWindowTitle(QString::fromStdString(name));
}

void RoomWindow::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    if (surface_) { surface_->relayout(); surface_->update(); }
}

void RoomWindow::closeEvent(QCloseEvent* ev) {
    schedule_self_close_();
    ev->accept();
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase* MainWindow::create_secondary_room_window_(
    const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace qt6
