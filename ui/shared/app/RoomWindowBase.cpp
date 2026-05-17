#include "app/RoomWindowBase.h"
#include "app/ShellBase.h"
#include <tesseract/client.h>

namespace tesseract {

RoomWindowBase::RoomWindowBase(ShellBase* shell, std::string room_id)
    : shell_(shell), room_id_(std::move(room_id))
{}

RoomWindowBase::~RoomWindowBase() {
    if (shell_) {
        shell_->unregister_room_window_(this);
        shell_->release_room_subscription_(room_id_);
    }
}

void RoomWindowBase::finish_init_() {
    shell_->register_room_window_(this);
    shell_->acquire_room_subscription_(room_id_);
    for (const auto& r : shell_->rooms_) {
        if (r.id == room_id_) {
            if (room_view_) room_view_->set_room(r);
            break;
        }
    }
}

void RoomWindowBase::on_room_info_updated(const RoomInfo& r) {
    if (room_view_) room_view_->set_room(r);
    update_window_title_(r.name);
    request_relayout();
}

void RoomWindowBase::on_timeline_reset(std::vector<views::MessageRowData> rows) {
    if (room_view_) room_view_->set_messages(std::move(rows));
    request_relayout();
}

void RoomWindowBase::on_message_inserted(std::size_t idx, views::MessageRowData row) {
    if (room_view_) room_view_->insert_message(idx, std::move(row));
    request_relayout();
}

void RoomWindowBase::on_message_updated(std::size_t idx, views::MessageRowData row) {
    if (room_view_) room_view_->update_message(idx, std::move(row));
    request_relayout();
}

void RoomWindowBase::on_message_removed(std::size_t idx) {
    if (room_view_) room_view_->remove_message(idx);
    request_relayout();
}

void RoomWindowBase::on_typing_changed(const std::string& text, bool visible) {
    typing_bar_visible_ = visible;
    if (room_view_) room_view_->set_typing_text(text);
    request_relayout();
}

void RoomWindowBase::schedule_self_close_() {
    if (!shell_) return;
    shell_->post_to_ui_([shell = shell_, w = this] {
        shell->release_owned_window_(w);
    });
}

void RoomWindowBase::send_message_(const std::string& body) {
    if (body.empty() || room_id_.empty() || !shell_->client_) return;
    shell_->client_->send_message(room_id_, body);
}

void RoomWindowBase::send_reply_(const std::string& reply_event_id,
                                  const std::string& body) {
    if (body.empty() || room_id_.empty() || !shell_->client_) return;
    shell_->client_->send_reply(room_id_, reply_event_id, body);
}

void RoomWindowBase::send_edit_(const std::string& event_id,
                                 const std::string& new_body) {
    if (new_body.empty() || room_id_.empty() || !shell_->client_) return;
    shell_->client_->send_edit(room_id_, event_id, new_body);
}

void RoomWindowBase::delete_event_(const std::string& event_id) {
    if (event_id.empty() || room_id_.empty() || !shell_->client_) return;
    shell_->client_->redact_event(room_id_, event_id);
}

void RoomWindowBase::toggle_reaction_(const std::string& event_id,
                                       const std::string& key) {
    if (event_id.empty() || room_id_.empty() || !shell_->client_) return;
    shell_->client_->send_reaction(room_id_, event_id, key);
}

void RoomWindowBase::send_receipt_(const std::string& event_id) {
    shell_->maybe_send_read_receipt_(room_id_, event_id);
}

void RoomWindowBase::request_pagination_back_() {
    if (room_id_.empty() || !shell_->client_) return;
    auto& state = shell_->pagination_[room_id_];
    if (state.in_flight || state.reached_start) return;
    state.in_flight = true;
    shell_->run_async_([shell = shell_, room_id = room_id_] {
        auto pr = shell->client_->paginate_back_with_status(
            room_id, ShellBase::kPaginationBatch);
        shell->post_to_ui_([shell, room_id, pr] {
            shell->push_paginate_result_(room_id, pr.reached_start);
        });
    });
}

} // namespace tesseract
