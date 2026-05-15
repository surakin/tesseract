#include "JoinRoomDialog.h"

#include "tk/theme.h"

#include <tesseract/client.h>
#include <tesseract/types.h>

#include <QHBoxLayout>
#include <QResizeEvent>
#include <QRunnable>
#include <QShowEvent>
#include <QThreadPool>

JoinRoomDialog::JoinRoomDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    setWindowTitle("Join a Room");
    setFixedSize(
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredW),
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredH));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    layout->addWidget(surface_);

    auto jrv = std::make_unique<tesseract::views::JoinRoomView>();
    shared_  = jrv.get();

    shared_->on_lookup_requested = [this](const std::string& alias) {
        if (!client_ || alias.empty()) return;
        shared_->set_state(tesseract::views::JoinRoomView::State::Loading);
        surface_->relayout();

        struct LookupTask : public QRunnable {
            JoinRoomDialog*  dialog;
            tesseract::Client* client;
            std::string        alias;
            void run() override {
                tesseract::RoomSummary s = client->get_room_summary(alias);
                // Post back to UI thread via queued invocation.
                QMetaObject::invokeMethod(dialog, [d = dialog, s = std::move(s)] {
                    if (!d->shared_) return;
                    if (s.ok())
                        d->shared_->set_preview(s);
                    else
                        d->shared_->set_error("Room not found.");
                    d->surface_->relayout();
                }, Qt::QueuedConnection);
            }
        };
        auto* task    = new LookupTask;
        task->dialog  = this;
        task->client  = client_;
        task->alias   = alias;
        task->setAutoDelete(true);
        QThreadPool::globalInstance()->start(task);
    };

    shared_->on_join_requested = [this](const std::string& room_id_or_alias) {
        if (!client_ || room_id_or_alias.empty()) return;
        shared_->set_state(tesseract::views::JoinRoomView::State::Joining);
        surface_->relayout();

        struct JoinTask : public QRunnable {
            JoinRoomDialog*  dialog;
            tesseract::Client* client;
            std::string        id;
            void run() override {
                bool ok = client->join_room(id);
                std::string joined_id = ok ? id : "";
                QMetaObject::invokeMethod(dialog, [d = dialog, ok, joined_id] {
                    if (!d->shared_) return;
                    if (ok) {
                        d->hide();
                        if (d->onJoined) d->onJoined(joined_id);
                    } else {
                        d->shared_->set_error("Join failed.");
                        d->surface_->relayout();
                    }
                }, Qt::QueuedConnection);
            }
        };
        auto* task    = new JoinTask;
        task->dialog  = this;
        task->client  = client_;
        task->id      = room_id_or_alias;
        task->setAutoDelete(true);
        QThreadPool::globalInstance()->start(task);
    };

    shared_->on_cancel = [this] { hide(); };

    surface_->set_root(std::move(jrv));

    alias_field_ = surface_->host().make_text_field();
    alias_field_->set_placeholder("#room:server.org");
    alias_field_->set_on_changed([this](const std::string& text) {
        if (shared_) shared_->set_alias_text(text);
    });

    surface_->set_on_layout([this] { layout_overlay(); });
}

void JoinRoomDialog::setClient(tesseract::Client* c) {
    client_ = c;
}

void JoinRoomDialog::setAvatarProvider(
    std::function<const tk::Image*(const std::string& mxc_url)> fn) {
    if (shared_) shared_->set_avatar_provider(std::move(fn));
}

void JoinRoomDialog::openDialog() {
    if (shared_) {
        shared_->set_state(tesseract::views::JoinRoomView::State::Idle);
        shared_->set_alias_text("");
    }
    if (alias_field_) alias_field_->set_text("");
    surface_->relayout();
    show();
    raise();
    activateWindow();
    if (alias_field_) alias_field_->set_focused(true);
}

void JoinRoomDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    surface_->relayout();
}

void JoinRoomDialog::resizeEvent(QResizeEvent* e) {
    QDialog::resizeEvent(e);
    layout_overlay();
}

void JoinRoomDialog::layout_overlay() {
    if (!alias_field_ || !shared_) return;
    alias_field_->set_rect(shared_->alias_field_rect());
    alias_field_->set_visible(shared_->alias_field_visible());
}
