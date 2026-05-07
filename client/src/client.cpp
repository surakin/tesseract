#include "tesseract/client.h"
#include "tesseract/event_handler_bridge.h"

// cxx-generated header (produced by corrosion_add_cxxbridge)
#include "ffi_convert.h"

#include <cassert>
#include <cstdlib>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

namespace tesseract {

// ---------------------------------------------------------------------------
// Pimpl
// ---------------------------------------------------------------------------
struct Client::Impl {
    rust::Box<tesseract_ffi::ClientFfi> ffi;

    explicit Impl()
        : ffi(tesseract_ffi::client_create()) {}
};

// ---------------------------------------------------------------------------

Client::Client()
    : impl_(std::make_unique<Impl>()) {}

Client::~Client() = default;

Client::Client(Client&&) noexcept            = default;
Client& Client::operator=(Client&&) noexcept = default;

// ---------------------------------------------------------------------------

Client::OAuthFlow Client::begin_oauth(const std::string& homeserver) {
    auto r = impl_->ffi->oauth_begin(homeserver);
    return OAuthFlow{
        .ok           = r.ok,
        .message      = std::string(r.message),
        .auth_url     = std::string(r.auth_url),
        .redirect_uri = std::string(r.redirect_uri),
    };
}

Result Client::await_oauth() {
    return from_ffi(impl_->ffi->oauth_await_callback());
}

void Client::cancel_oauth() {
    impl_->ffi->oauth_cancel();
}

bool Client::open_in_browser(const std::string& url) {
#if defined(_WIN32)
    HINSTANCE hi = ShellExecuteA(nullptr, "open", url.c_str(),
                                 nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(hi) > 32;
#elif defined(__APPLE__)
    pid_t pid = fork();
    if (pid == 0) {
        execlp("open", "open", url.c_str(), static_cast<const char*>(nullptr));
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<const char*>(nullptr));
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    return waitpid(pid, &status, 0) == pid
        && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

Result Client::restore_session(const std::string& session_json) {
    return from_ffi(impl_->ffi->restore_session(session_json));
}

std::string Client::export_session() const {
    return std::string(impl_->ffi->export_session());
}

Result Client::logout() {
    return from_ffi(impl_->ffi->logout());
}

void Client::start_sync(IEventHandler* handler) {
    impl_->ffi->start_sync(
        std::make_unique<tesseract_ffi::EventHandlerBridge>(handler));
}

void Client::stop_sync() {
    impl_->ffi->stop_sync();
}

std::vector<RoomInfo> Client::list_rooms() const {
    auto ffi_rooms = impl_->ffi->list_rooms();
    std::vector<RoomInfo> result;
    result.reserve(ffi_rooms.size());
    for (const auto& r : ffi_rooms)
        result.push_back(from_ffi(r));
    return result;
}

Result Client::subscribe_room(const std::string& room_id) {
    return from_ffi(impl_->ffi->subscribe_room(room_id));
}

void Client::unsubscribe_room(const std::string& room_id) {
    impl_->ffi->unsubscribe_room(room_id);
}

Result Client::paginate_back(const std::string& room_id, std::uint16_t count) {
    return from_ffi(impl_->ffi->paginate_back(room_id, count));
}

Result Client::send_message(const std::string& room_id, const std::string& body) {
    return from_ffi(impl_->ffi->send_message(room_id, body));
}

} // namespace tesseract
