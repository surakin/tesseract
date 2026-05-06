#include "matrix/client.hpp"
#include "matrix/event_handler_bridge.hpp"

// cxx-generated header
#include "matrix-sdk-ffi/src/lib.rs.h"

#include <cassert>

namespace matrix {

// ---------------------------------------------------------------------------
// Pimpl that holds the rust::Box<MatrixClientFfi> with its real cxx type.
// ---------------------------------------------------------------------------
struct MatrixClient::Impl {
    rust::Box<matrix_ffi::MatrixClientFfi> ffi;

    explicit Impl()
        : ffi(matrix_ffi::matrix_client_create()) {}
};

// ---------------------------------------------------------------------------

static Result from_ffi(const matrix_ffi::OpResult& r) {
    return Result{ r.ok, std::string(r.message) };
}

static RoomInfo from_ffi(const matrix_ffi::RoomInfo& r) {
    return RoomInfo{
        .id           = std::string(r.id),
        .name         = std::string(r.name),
        .topic        = std::string(r.topic),
        .unread_count = r.unread_count,
        .is_direct    = r.is_direct,
    };
}

static Message from_ffi(const matrix_ffi::TimelineEvent& e) {
    return Message{
        .event_id  = std::string(e.event_id),
        .room_id   = std::string(e.room_id),
        .sender    = std::string(e.sender),
        .body      = std::string(e.body),
        .timestamp = e.timestamp,
        .msg_type  = std::string(e.msg_type),
    };
}

// ---------------------------------------------------------------------------

MatrixClient::MatrixClient()
    : impl_(std::make_unique<Impl>()) {}

MatrixClient::~MatrixClient() = default;

MatrixClient::MatrixClient(MatrixClient&&) noexcept            = default;
MatrixClient& MatrixClient::operator=(MatrixClient&&) noexcept = default;

// ---------------------------------------------------------------------------

Result MatrixClient::login(
    const std::string& homeserver,
    const std::string& username,
    const std::string& password)
{
    return from_ffi(impl_->ffi->login_password(homeserver, username, password));
}

Result MatrixClient::restore_session(const std::string& session_json) {
    return from_ffi(impl_->ffi->restore_session(session_json));
}

std::string MatrixClient::export_session() const {
    return std::string(impl_->ffi->export_session());
}

Result MatrixClient::logout() {
    stop_sync();
    return from_ffi(impl_->ffi->logout());
}

// ---------------------------------------------------------------------------

void MatrixClient::start_sync(IEventHandler* handler) {
    assert(handler && "handler must not be null");
    auto bridge = std::make_unique<matrix_ffi::EventHandlerBridge>(handler);
    impl_->ffi->start_sync(std::move(bridge));
}

void MatrixClient::stop_sync() {
    impl_->ffi->stop_sync();
}

// ---------------------------------------------------------------------------

std::vector<RoomInfo> MatrixClient::list_rooms() const {
    auto raw = impl_->ffi->list_rooms();
    std::vector<RoomInfo> out;
    out.reserve(raw.size());
    for (const auto& r : raw) out.push_back(from_ffi(r));
    return out;
}

std::vector<Message> MatrixClient::room_messages(
    const std::string& room_id, uint64_t limit) const
{
    auto raw = impl_->ffi->room_messages(room_id, limit);
    std::vector<Message> out;
    out.reserve(raw.size());
    for (const auto& e : raw) out.push_back(from_ffi(e));
    return out;
}

Result MatrixClient::send_message(
    const std::string& room_id,
    const std::string& body)
{
    return from_ffi(impl_->ffi->send_message(room_id, body));
}

} // namespace matrix
