#include "tesseract/client.h"
#include "tesseract/event_handler_bridge.h"

// cxx-generated header (produced by corrosion_add_cxxbridge)
#include "ffi_convert.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string_view>

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

void Client::set_data_dir(const std::string& path) {
    impl_->ffi->set_data_dir(path);
}

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

PaginateResult Client::paginate_back_with_status(const std::string& room_id,
                                                  std::uint16_t count) {
    return from_ffi(impl_->ffi->paginate_back_with_status(room_id, count));
}

Result Client::timestamp_to_event(const std::string& room_id,
                                   uint64_t ts_ms,
                                   const std::string& dir) {
    if (!impl_) return { false, "client not initialised" };
    return from_ffi(impl_->ffi->timestamp_to_event(room_id, ts_ms, dir));
}

Result Client::subscribe_room_at(const std::string& room_id,
                                  const std::string& focus_event_id) {
    if (!impl_) return { false, "client not initialised" };
    return from_ffi(impl_->ffi->subscribe_room_at(room_id, focus_event_id));
}

PaginateResult Client::paginate_forward(const std::string& room_id,
                                         std::uint16_t count) {
    if (!impl_) return {};
    return from_ffi(impl_->ffi->paginate_forward(room_id, count));
}

Result Client::start_background_backfill(
    const std::vector<std::string>& room_ids)
{
    return from_ffi(impl_->ffi->start_background_backfill(room_ids));
}

void Client::stop_background_backfill() {
    impl_->ffi->stop_background_backfill();
}

Result Client::send_message(const std::string& room_id, const std::string& body,
                             const std::string& formatted_body) {
    return from_ffi(impl_->ffi->send_message(room_id, body, formatted_body));
}

void Client::send_typing_notice(const std::string& room_id, bool typing) {
    impl_->ffi->send_typing_notice(room_id, typing);
}

Result Client::send_image(const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
                          const std::string& caption,
                          std::uint32_t width,
                          std::uint32_t height,
                          const std::string& reply_event_id) {
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_image(
        room_id, slice, mime_type, filename, caption, width, height, reply_event_id));
}

Result Client::send_file(const std::string& room_id,
                         const std::vector<uint8_t>& bytes,
                         const std::string& mime_type,
                         const std::string& filename,
                         const std::string& caption,
                         const std::string& reply_event_id) {
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_file(
        room_id, slice, mime_type, filename, caption, reply_event_id));
}

std::uint64_t Client::media_upload_limit() {
    return impl_->ffi->media_upload_limit();
}

Result Client::send_read_receipt(const std::string& room_id,
                                  const std::string& event_id) {
    return from_ffi(impl_->ffi->send_read_receipt(room_id, event_id));
}

Result Client::mark_room_as_read(const std::string& room_id) {
    return from_ffi(impl_->ffi->mark_room_as_read(room_id));
}

Result Client::send_reaction(const std::string& room_id,
                             const std::string& event_id,
                             const std::string& key) {
    return from_ffi(impl_->ffi->send_reaction(room_id, event_id, key));
}

Result Client::send_reaction_custom(const std::string& room_id,
                                    const std::string& event_id,
                                    const std::string& key,
                                    const std::string& shortcode) {
    return from_ffi(impl_->ffi->send_reaction_custom(room_id, event_id, key, shortcode));
}

Result Client::redact_event(const std::string& room_id,
                            const std::string& event_id,
                            const std::string& reason) {
    return from_ffi(impl_->ffi->redact_event(room_id, event_id, reason));
}

Result Client::send_reply(const std::string& room_id,
                          const std::string& event_id,
                          const std::string& body,
                          const std::string& formatted_body) {
    return from_ffi(impl_->ffi->send_reply(room_id, event_id, body, formatted_body));
}

Result Client::fetch_reply_details(const std::string& room_id,
                                    const std::string& event_id) {
    return from_ffi(impl_->ffi->fetch_reply_details(room_id, event_id));
}

Result Client::send_edit(const std::string& room_id,
                         const std::string& event_id,
                         const std::string& new_body,
                         const std::string& formatted_body) {
    return from_ffi(impl_->ffi->send_edit(room_id, event_id, new_body, formatted_body));
}

std::string Client::load_prefs_json() {
    return std::string(impl_->ffi->load_prefs());
}

void Client::save_prefs_json(const std::string& json) {
    impl_->ffi->save_prefs(json);
}

std::vector<std::string> Client::recent_emoji_top(std::uint32_t n) {
    // cxx returns rust::Vec<rust::String>; copy each into std::string so
    // callers don't have to know about the cxx types.
    auto raw = impl_->ffi->recent_emoji_top(n);
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (const auto& s : raw) out.emplace_back(std::string(s));
    return out;
}

void Client::recent_emoji_bump(const std::string& glyph) {
    impl_->ffi->recent_emoji_bump(glyph);
}

std::string Client::get_user_id() const {
    return std::string(impl_->ffi->user_id());
}

std::string Client::get_display_name() const {
    return std::string(impl_->ffi->current_user_display_name());
}

std::string Client::get_avatar_url() const {
    return std::string(impl_->ffi->current_user_avatar_url());
}

std::vector<uint8_t> Client::fetch_avatar_bytes(const std::string& room_id) {
    auto v = impl_->ffi->fetch_avatar_bytes(room_id);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_media_bytes(const std::string& mxc_url) {
    auto v = impl_->ffi->fetch_media_bytes(mxc_url);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_source_bytes(const std::string& source) {
    auto v = impl_->ffi->fetch_source_bytes(source);
    return std::vector<uint8_t>(v.begin(), v.end());
}

// ---------------------------------------------------------------------------
// URL preview
// ---------------------------------------------------------------------------

namespace {

std::string json_string_field(std::string_view json, std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    std::string out;
    for (; pos < json.size() && json[pos] != '"'; ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += json[pos]; break;
            }
        } else {
            out += json[pos];
        }
    }
    return out;
}

int json_int_field(std::string_view json, std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return 0;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    int v = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
        v = v * 10 + (json[pos++] - '0');
    return v;
}

} // namespace

Client::UrlPreview Client::get_url_preview(const std::string& url) {
    std::string json = std::string(impl_->ffi->get_url_preview(url));
    if (json.empty()) { UrlPreview p; p.failed = true; return p; }
    UrlPreview p;
    p.title       = json_string_field(json, "og:title");
    p.description = json_string_field(json, "og:description");
    p.image_mxc   = json_string_field(json, "og:image");
    p.image_w     = json_int_field(json, "og:image:width");
    p.image_h     = json_int_field(json, "og:image:height");
    if (!p.has_content()) { p.failed = true; }
    return p;
}

// ---------------------------------------------------------------------------
// MSC2545 image packs
// ---------------------------------------------------------------------------

std::vector<ImagePack> Client::list_image_packs() const {
    auto raw = impl_->ffi->list_image_packs();
    std::vector<ImagePack> out;
    out.reserve(raw.size());
    for (const auto& p : raw)
        out.push_back(from_ffi(p));
    return out;
}

std::vector<ImagePackImage> Client::list_pack_images(const std::string& pack_id,
                                                     PackUsageFilter filter) const {
    auto raw = impl_->ffi->list_pack_images(pack_id, pack_usage_filter_to_str(filter));
    std::vector<ImagePackImage> out;
    out.reserve(raw.size());
    for (const auto& e : raw)
        out.push_back(from_ffi(e));
    return out;
}

std::vector<ImagePackImage> Client::list_favorite_stickers() const {
    auto raw = impl_->ffi->list_favorite_stickers();
    std::vector<ImagePackImage> out;
    out.reserve(raw.size());
    for (const auto& e : raw)
        out.push_back(from_ffi(e));
    return out;
}

Result Client::send_sticker(const std::string& room_id,
                            const std::string& body,
                            const std::string& image_url,
                            const std::string& info_json) {
    return from_ffi(impl_->ffi->send_sticker(room_id, body, image_url, info_json));
}

Result Client::save_sticker_to_user_pack(const std::string& shortcode,
                                         const std::string& body,
                                         const std::string& image_url,
                                         const std::string& info_json) {
    return from_ffi(impl_->ffi->save_sticker_to_user_pack(
        shortcode, body, image_url, info_json));
}

bool Client::user_pack_has_sticker(const std::string& image_url) const {
    return impl_->ffi->user_pack_has_sticker(image_url);
}

Result Client::toggle_favorite_sticker(const std::string& image_url) {
    return from_ffi(impl_->ffi->toggle_favorite_sticker(image_url));
}

std::vector<std::string> Client::space_children(const std::string& space_id) const {
    auto raw = impl_->ffi->space_children(space_id);
    std::vector<std::string> result;
    result.reserve(raw.size());
    for (const auto& s : raw)
        result.push_back(std::string(s));
    return result;
}

bool Client::needs_recovery() const {
    return impl_->ffi->needs_recovery();
}

Result Client::recover(const std::string& key_or_passphrase) {
    return from_ffi(impl_->ffi->recover(key_or_passphrase));
}

BackupProgress Client::backup_state() const {
    return from_ffi(impl_->ffi->backup_state());
}

Result Client::request_self_verification() {
    return from_ffi(impl_->ffi->request_self_verification());
}

Result Client::accept_verification(const std::string& flow_id) {
    return from_ffi(impl_->ffi->accept_verification(flow_id));
}

Result Client::start_sas(const std::string& flow_id) {
    return from_ffi(impl_->ffi->start_sas(flow_id));
}

Result Client::confirm_sas(const std::string& flow_id) {
    return from_ffi(impl_->ffi->confirm_sas(flow_id));
}

Result Client::cancel_verification(const std::string& flow_id) {
    return from_ffi(impl_->ffi->cancel_verification(flow_id));
}

std::vector<VerificationEmoji> Client::get_sas_emojis(const std::string& flow_id) const {
    auto ffi_vec = impl_->ffi->get_sas_emojis(flow_id);
    std::vector<VerificationEmoji> result;
    result.reserve(ffi_vec.size());
    for (const auto& e : ffi_vec)
        result.push_back({std::string(e.symbol), std::string(e.description)});
    return result;
}

Result Client::register_pusher(const std::string& pushkey,
                                const std::string& app_id,
                                const std::string& app_display_name,
                                const std::string& device_display_name,
                                const std::string& endpoint_url,
                                const std::string& lang) {
    return from_ffi(impl_->ffi->register_pusher(
        pushkey, app_id, app_display_name, device_display_name, endpoint_url, lang));
}

Result Client::remove_pusher(const std::string& pushkey, const std::string& app_id) {
    return from_ffi(impl_->ffi->remove_pusher(pushkey, app_id));
}

} // namespace tesseract
