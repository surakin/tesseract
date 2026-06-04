#include "tesseract/client.h"
#include "tesseract/event_handler_bridge.h"
#include "tesseract/markdown.h"

// cxx-generated header (produced by corrosion_add_cxxbridge)
#include "ffi_convert.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace tesseract
{

// ---------------------------------------------------------------------------
// Pimpl
// ---------------------------------------------------------------------------
struct Client::Impl
{
    rust::Box<tesseract_ffi::ClientFfi> ffi;
    // Shared, mutex-guarded slot holding the IEventHandler the sync bridge
    // dispatches to. Created on demand in start_sync and detached in stop_sync
    // so a sync-task callback that fires after stop_sync() (tokio abort only
    // *requests* cancellation) observes a null handler and drops, instead of
    // dereferencing the about-to-be-destroyed handler. See HandlerSlot.
    std::shared_ptr<tesseract_ffi::HandlerSlot> handler_slot;
    // Serialises all calls that reach &mut ClientFfi across the cxx boundary.
    // Any method whose Rust signature is `&self` (not `&mut self`) does NOT
    // acquire this lock and may run concurrently: fetch_*, fetch_media_async,
    // fetch_url_async, cancel_media_group, get_url_preview, get_url_preview_async,
    // get_room_summary, get_room_members, space_children,
    // get_room_notification_mode, can_pin_in_room, needs_recovery,
    // backup_state, recovery_state, list_pack_images, list_devices,
    // get_sas_emojis.
    mutable std::mutex ffi_mu;

    explicit Impl() : ffi(tesseract_ffi::client_create())
    {
    }
};

// Acquire ffi_mu for the duration of the current scope. Add to every wrapper
// that calls a &mut ClientFfi bridge method to prevent concurrent mutable
// access from run_async_() worker threads and the UI thread.
#define MUT_FFI std::lock_guard<std::mutex> _fmu_(impl_->ffi_mu)

// ---------------------------------------------------------------------------

Client::Client() : impl_(std::make_unique<Impl>())
{
}

Client::~Client() = default;

Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

// ---------------------------------------------------------------------------

void Client::set_data_dir(const std::string& path)
{
    MUT_FFI;
    impl_->ffi->set_data_dir(path);
}

Client::OAuthFlow Client::begin_oauth(const std::string& homeserver,
                                      bool register_account)
{
    MUT_FFI;
    auto r = impl_->ffi->oauth_begin(homeserver, register_account);
    return OAuthFlow{
        .ok = r.ok,
        .message = std::string(r.message),
        .auth_url = std::string(r.auth_url),
        .redirect_uri = std::string(r.redirect_uri),
    };
}

bool Client::homeserver_supports_registration(const std::string& homeserver)
{
    MUT_FFI;
    return impl_->ffi->homeserver_supports_registration(homeserver);
}

Result Client::await_oauth()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->oauth_await_callback());
}

void Client::cancel_oauth()
{
    MUT_FFI;
    impl_->ffi->oauth_cancel();
}

bool Client::open_in_browser(const std::string& url)
{
#if defined(_WIN32)
    HINSTANCE hi = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(hi) > 32;
#else
    // Double-fork so we never block the (typically UI) caller waiting for
    // the launcher. The middle process exits immediately; the grandchild is
    // reparented to init and exec's the opener, so there is no zombie and no
    // wait on the browser itself. We can only report that the launch was
    // dispatched, not its eventual exit code (which is fine — `xdg-open`
    // returning does not mean the page is up anyway).
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid == 0)
    {
        pid_t inner = fork();
        if (inner == 0)
        {
            execlp(opener, opener, url.c_str(),
                   static_cast<const char*>(nullptr));
            _exit(127);
        }
        _exit(inner < 0 ? 1 : 0);
    }
    int status = 0;
    // Reaps only the fast middle process, not the opener.
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
#endif
}

Result Client::restore_session(const std::string& session_json)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->restore_session(session_json));
}

std::string Client::export_session() const
{
    return std::string(impl_->ffi->export_session());
}

Result Client::logout()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->logout());
}

void Client::start_sync(IEventHandler* handler)
{
    MUT_FFI;
    // Detach any prior slot (e.g. a previous start_sync without an intervening
    // stop_sync) so its bridge stops dispatching, then publish a fresh slot.
    if (impl_->handler_slot)
    {
        impl_->handler_slot->detach();
    }
    impl_->handler_slot =
        std::make_shared<tesseract_ffi::HandlerSlot>(handler);
    impl_->ffi->start_sync(
        std::make_unique<tesseract_ffi::EventHandlerBridge>(
            impl_->handler_slot));
}

void Client::stop_sync()
{
    MUT_FFI;
    // Sever the handler link *before* aborting the Rust tasks: a callback
    // already in flight on the sync-task thread will then observe a null
    // handler (under the slot mutex) and drop, even though tokio's abort()
    // only requests cancellation and cannot wait for that callback to finish.
    if (impl_->handler_slot)
    {
        impl_->handler_slot->detach();
    }
    impl_->ffi->stop_sync();
}

tesseract::Result Client::clear_caches()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->clear_caches());
}

std::uint32_t Client::in_flight_count() const
{
    return impl_->ffi->in_flight_count();
}

std::vector<RoomInfo> Client::list_rooms() const
{
    return ffi_vec<RoomInfo>(impl_->ffi->list_rooms());
}

std::vector<InviteInfo> Client::list_invites() const
{
    return ffi_vec<InviteInfo>(impl_->ffi->list_invites());
}

Result Client::accept_invite(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->accept_invite(room_id));
}

Result Client::decline_invite(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->decline_invite(room_id));
}

Result Client::block_invite(const std::string& room_id,
                            const std::string& inviter_user_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->block_invite(room_id, inviter_user_id));
}

Result Client::subscribe_room(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->subscribe_room(room_id));
}

void Client::unsubscribe_room(const std::string& room_id)
{
    MUT_FFI;
    impl_->ffi->unsubscribe_room(room_id);
}

Result Client::paginate_back(const std::string& room_id, std::uint16_t count)
{
    return from_ffi(impl_->ffi->paginate_back(room_id, count));
}

PaginateResult Client::paginate_back_with_status(const std::string& room_id,
                                                 std::uint16_t count)
{
    return from_ffi(impl_->ffi->paginate_back_with_status(room_id, count));
}

Result Client::timestamp_to_event(const std::string& room_id, uint64_t ts_ms,
                                  const std::string& dir)
{
    MUT_FFI;
    if (!impl_)
    {
        return {false, "client not initialised"};
    }
    return from_ffi(impl_->ffi->timestamp_to_event(room_id, ts_ms, dir));
}

Result Client::subscribe_room_at(const std::string& room_id,
                                 const std::string& focus_event_id)
{
    MUT_FFI;
    if (!impl_)
    {
        return {false, "client not initialised"};
    }
    return from_ffi(impl_->ffi->subscribe_room_at(room_id, focus_event_id));
}

PaginateResult Client::paginate_forward(const std::string& room_id,
                                        std::uint16_t count)
{
    MUT_FFI;
    if (!impl_)
    {
        return {};
    }
    return from_ffi(impl_->ffi->paginate_forward(room_id, count));
}

Result Client::subscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->subscribe_thread(room_id, root_event_id));
}

void Client::unsubscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    MUT_FFI;
    impl_->ffi->unsubscribe_thread(room_id, root_event_id);
}

PaginateResult Client::paginate_thread_back(const std::string& room_id,
                                            const std::string& root_event_id,
                                            std::uint16_t count)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->paginate_thread_back(room_id, root_event_id, count));
}

Result Client::subscribe_room_threads(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->subscribe_room_threads(room_id));
}

void Client::unsubscribe_room_threads(const std::string& room_id)
{
    MUT_FFI;
    impl_->ffi->unsubscribe_room_threads(room_id);
}

std::vector<ThreadInfo> Client::list_room_threads(const std::string& room_id)
{
    return ffi_vec<ThreadInfo>(impl_->ffi->list_room_threads(room_id));
}

PaginateResult Client::paginate_room_threads(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->paginate_room_threads(room_id));
}

Result
Client::start_background_backfill(const std::vector<std::string>& room_ids)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->start_background_backfill(room_ids));
}

Result
Client::start_background_backfill_all_uncached()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->start_background_backfill_all_uncached());
}

void Client::stop_background_backfill()
{
    MUT_FFI;
    impl_->ffi->stop_background_backfill();
}

// Single markdown chokepoint for every outgoing text send (all four shells'
// main windows + RoomWindowBase pop-outs). When the caller did not supply an
// explicit `formatted_body`, derive it from `body` via markdown_to_html
// (returns "" when there are no markdown markers — plain text stays plain).
// An explicitly supplied non-empty formatted_body is passed through
// verbatim, preserving the future custom-HTML send paths (e.g. emoticons).
static std::string derive_formatted(const std::string& body,
                                    const std::string& formatted_body)
{
    if (!formatted_body.empty())
    {
        return formatted_body;
    }
    return markdown_to_html(body).formatted_body;
}

Result Client::send_message(const std::string& room_id, const std::string& body,
                            const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_message(
        room_id, body, derive_formatted(body, formatted_body)));
}

Result Client::send_emote(const std::string& room_id, const std::string& body,
                          const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_emote(
        room_id, body, derive_formatted(body, formatted_body)));
}

Result Client::retry_send(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->retry_send(room_id));
}

Result Client::abort_send(const std::string& room_id, const std::string& txn_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->abort_send(room_id, txn_id));
}

void Client::send_typing_notice(const std::string& room_id, bool typing)
{
    MUT_FFI;
    impl_->ffi->send_typing_notice(room_id, typing);
}

Result Client::send_image(const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
                          const std::string& caption, std::uint32_t width,
                          std::uint32_t height,
                          bool is_animated,
                          const std::string& reply_event_id,
                          const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_image(room_id, slice, mime_type, filename,
                                           caption, width, height, is_animated,
                                           reply_event_id, thread_root));
}

Result Client::send_video(const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
                          const std::string& caption,
                          std::uint32_t width, std::uint32_t height,
                          const std::vector<uint8_t>& thumb_bytes,
                          std::uint32_t thumb_width, std::uint32_t thumb_height,
                          std::uint64_t duration_ms,
                          const std::string& reply_event_id,
                          const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    rust::Slice<const std::uint8_t> thumb_slice{thumb_bytes.data(),
                                                thumb_bytes.size()};
    return from_ffi(impl_->ffi->send_video(room_id, slice, mime_type, filename,
                                           caption, width, height, thumb_slice,
                                           thumb_width, thumb_height,
                                           duration_ms, reply_event_id,
                                           thread_root));
}

Result Client::send_audio(const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
                          const std::string& caption,
                          std::uint64_t duration_ms,
                          const std::string& reply_event_id,
                          const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_audio(room_id, slice, mime_type, filename,
                                           caption, duration_ms,
                                           reply_event_id, thread_root));
}

Result
Client::send_file(const std::string& room_id, const std::vector<uint8_t>& bytes,
                  const std::string& mime_type, const std::string& filename,
                  const std::string& caption, const std::string& reply_event_id,
                  const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_file(room_id, slice, mime_type, filename,
                                          caption, reply_event_id, thread_root));
}

Result Client::send_voice(const std::string& room_id,
                          const std::uint8_t* pcm, std::size_t pcm_size,
                          std::uint64_t duration_ms,
                          const std::vector<std::uint16_t>& waveform,
                          const std::string& caption,
                          const std::string& reply_event_id,
                          const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> pcm_slice{pcm, pcm_size};
    rust::Slice<const std::uint16_t> wf_slice{waveform.data(), waveform.size()};
    return from_ffi(impl_->ffi->send_voice(room_id, pcm_slice, duration_ms,
                                            wf_slice, caption,
                                            reply_event_id, thread_root));
}

std::uint64_t Client::media_upload_limit()
{
    MUT_FFI;
    return impl_->ffi->media_upload_limit();
}

Result Client::send_read_receipt(const std::string& room_id,
                                 const std::string& event_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_read_receipt(room_id, event_id));
}

Result Client::mark_room_as_read(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->mark_room_as_read(room_id));
}

Result Client::send_reaction(const std::string& room_id,
                             const std::string& event_id,
                             const std::string& key)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_reaction(room_id, event_id, key));
}

Result Client::send_reaction_custom(const std::string& room_id,
                                    const std::string& event_id,
                                    const std::string& key,
                                    const std::string& shortcode)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->send_reaction_custom(room_id, event_id, key, shortcode));
}

Result Client::redact_event(const std::string& room_id,
                            const std::string& event_id,
                            const std::string& reason)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->redact_event(room_id, event_id, reason));
}

Result Client::send_reply(const std::string& room_id,
                          const std::string& event_id, const std::string& body,
                          const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_reply(
        room_id, event_id, body, derive_formatted(body, formatted_body)));
}

Result Client::send_thread_message(const std::string& room_id,
                                   const std::string& thread_root,
                                   const std::string& body,
                                   const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_thread_message(
        room_id, thread_root, body, derive_formatted(body, formatted_body)));
}

Result Client::send_thread_reply(const std::string& room_id,
                                 const std::string& thread_root,
                                 const std::string& in_reply_to_event_id,
                                 const std::string& body,
                                 const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->send_thread_reply(
        room_id, thread_root, in_reply_to_event_id, body,
        derive_formatted(body, formatted_body)));
}

Result Client::fetch_reply_details(const std::string& room_id,
                                   const std::string& event_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->fetch_reply_details(room_id, event_id));
}

Result Client::send_edit(const std::string& room_id,
                         const std::string& event_id,
                         const std::string& new_body,
                         const std::string& formatted_body)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->send_edit(room_id, event_id, new_body,
                              derive_formatted(new_body, formatted_body)));
}

std::string Client::load_prefs_json()
{
    MUT_FFI;
    return std::string(impl_->ffi->load_prefs());
}

void Client::save_prefs_json(const std::string& json)
{
    MUT_FFI;
    impl_->ffi->save_prefs(json);
}

MediaPreviewConfig Client::media_preview_config()
{
    MUT_FFI;
    auto raw = impl_->ffi->media_preview_config();
    MediaPreviewConfig cfg;
    cfg.media_previews =
        static_cast<MediaPreviewConfig::Mode>(raw.media_previews);
    cfg.invite_avatars = raw.invite_avatars;
    return cfg;
}

MediaPreviewOverride
Client::room_media_preview_override(const std::string& room_id)
{
    MUT_FFI;
    auto raw = impl_->ffi->room_media_preview_override(room_id);
    MediaPreviewOverride ov;
    ov.has_media_previews = raw.has_media_previews;
    ov.media_previews =
        static_cast<MediaPreviewConfig::Mode>(raw.media_previews);
    ov.join_rule = std::string(raw.join_rule);
    return ov;
}

void Client::save_media_preview_config(MediaPreviewConfig::Mode media_previews,
                                       bool invite_avatars)
{
    MUT_FFI;
    impl_->ffi->set_media_preview_config(
        static_cast<std::uint8_t>(media_previews), invite_avatars);
}

std::vector<std::string> Client::recent_emoji_top(std::uint32_t n)
{
    MUT_FFI;
    // cxx returns rust::Vec<rust::String>; copy each into std::string so
    // callers don't have to know about the cxx types.
    auto raw = impl_->ffi->recent_emoji_top(n);
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (const auto& s : raw)
    {
        out.emplace_back(std::string(s));
    }
    return out;
}

void Client::recent_emoji_bump(const std::string& glyph)
{
    MUT_FFI;
    impl_->ffi->recent_emoji_bump(glyph);
}

std::string Client::get_user_id() const
{
    return std::string(impl_->ffi->user_id());
}

std::string Client::get_display_name() const
{
    return std::string(impl_->ffi->current_user_display_name());
}

std::string Client::get_avatar_url() const
{
    return std::string(impl_->ffi->current_user_avatar_url());
}

Result Client::set_display_name(const std::string& name)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->set_display_name(name));
}

Result Client::upload_avatar(const std::vector<uint8_t>& bytes,
                              const std::string& mime_type)
{
    MUT_FFI;
    auto slice = rust::Slice<const uint8_t>{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->upload_avatar(slice, mime_type));
}

Result Client::upload_media(const std::vector<uint8_t>& bytes,
                             const std::string& mime_type)
{
    MUT_FFI;
    auto slice = rust::Slice<const uint8_t>{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->upload_media(slice, mime_type));
}

Result Client::remove_avatar()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->remove_avatar());
}

std::string Client::get_device_id() const
{
    return std::string(impl_->ffi->device_id());
}

std::vector<Client::Device> Client::list_devices() const
{
    auto ffi_devices = impl_->ffi->list_devices();
    std::vector<Device> out;
    out.reserve(ffi_devices.size());
    for (const auto& d : ffi_devices)
    {
        Device dev;
        dev.id = std::string(d.device_id);
        dev.display_name = std::string(d.display_name);
        dev.last_seen_ip = std::string(d.last_seen_ip);
        dev.last_seen_ts = d.last_seen_ts;
        dev.verification = (d.verification_state == 2)
                               ? DeviceVerification::Verified
                               : (d.verification_state == 1)
                                     ? DeviceVerification::Unverified
                                     : DeviceVerification::Unknown;
        dev.is_current = d.is_current;
        out.push_back(std::move(dev));
    }
    return out;
}

Result Client::set_device_display_name(const std::string& device_id,
                                       const std::string& name)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->set_device_display_name(device_id, name));
}

Client::DeleteDeviceBegin
Client::begin_delete_device(const std::string& device_id)
{
    MUT_FFI;
    auto r = impl_->ffi->begin_delete_device(device_id);
    DeleteDeviceBegin out;
    out.ok = r.ok;
    out.message = std::string(r.message);
    out.needs_uia = r.needs_uia;
    out.fallback_url = std::string(r.fallback_url);
    out.session = std::string(r.session);
    return out;
}

Result Client::complete_delete_device(const std::string& device_id,
                                       const std::string& session)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->complete_delete_device(device_id, session));
}

Result Client::set_presence(PresenceState state)
{
    MUT_FFI;
    // Map the C++ enum (Online=0, Unavailable=1, Offline=2) to the 1/2/3 wire
    // encoding the Rust FFI accepts (matches event_handler_bridge's inverse).
    std::uint8_t byte = 3;
    switch (state)
    {
        case PresenceState::Online:      byte = 1; break;
        case PresenceState::Unavailable: byte = 2; break;
        case PresenceState::Offline:     byte = 3; break;
    }
    return from_ffi(impl_->ffi->set_presence(byte));
}

std::vector<uint8_t> Client::fetch_avatar_bytes(const std::string& room_id)
{
    auto v = impl_->ffi->fetch_avatar_bytes(room_id);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_media_bytes(const std::string& mxc_url)
{
    auto v = impl_->ffi->fetch_media_bytes(mxc_url);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_source_bytes(const std::string& source)
{
    auto v = impl_->ffi->fetch_source_bytes(source);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t>
Client::fetch_avatar_thumbnail_bytes(const std::string& room_id, uint32_t size)
{
    auto v = impl_->ffi->fetch_avatar_thumbnail_bytes(room_id, size);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_media_thumbnail_bytes(
    const std::string& mxc_url, uint32_t w, uint32_t h, bool animated)
{
    auto v = impl_->ffi->fetch_media_thumbnail_bytes(mxc_url, w, h, animated);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_source_thumbnail_bytes(
    const std::string& source, uint32_t w, uint32_t h, bool animated)
{
    auto v = impl_->ffi->fetch_source_thumbnail_bytes(source, w, h, animated);
    return std::vector<uint8_t>(v.begin(), v.end());
}

std::vector<uint8_t> Client::fetch_url_bytes(const std::string& url)
{
    auto v = impl_->ffi->fetch_url_bytes(url);
    return std::vector<uint8_t>(v.begin(), v.end());
}

// ---------------------------------------------------------------------------
// Async media downloads (non-blocking — &self FFI, no MUT_FFI)
// ---------------------------------------------------------------------------

void Client::fetch_media_async(std::uint64_t request_id, std::uint64_t group_id,
                               MediaReqKind kind, const std::string& source,
                               std::uint32_t w, std::uint32_t h, bool animated)
{
    impl_->ffi->fetch_media_async(request_id, group_id,
                                  static_cast<std::uint8_t>(kind), source, w, h,
                                  animated);
}

void Client::cancel_media_group(std::uint64_t group_id)
{
    impl_->ffi->cancel_media_group(group_id);
}

void Client::get_url_preview_async(std::uint64_t request_id,
                                   std::uint64_t group_id,
                                   const std::string& url)
{
    impl_->ffi->get_url_preview_async(request_id, group_id, url);
}

void Client::fetch_url_async(std::uint64_t request_id, std::uint64_t group_id,
                             const std::string& url)
{
    impl_->ffi->fetch_url_async(request_id, group_id, url);
}

void Client::gif_search(std::uint64_t request_id, const std::string& query,
                        const std::string& api_key,
                        const std::string& client_key, std::uint32_t limit)
{
    impl_->ffi->gif_search_async(request_id, query, api_key, client_key, limit);
}

Result Client::send_gif_video(
    const std::string& room_id, const std::vector<uint8_t>& mp4_bytes,
    const std::string& mime_type, const std::string& body, std::uint32_t width,
    std::uint32_t height, std::uint64_t duration_ms,
    const std::vector<uint8_t>& thumb_bytes, const std::string& thumb_mime,
    std::uint32_t thumb_width, std::uint32_t thumb_height,
    const std::string& reply_event_id, const std::string& thread_root)
{
    MUT_FFI;
    rust::Slice<const std::uint8_t> mp4{mp4_bytes.data(), mp4_bytes.size()};
    rust::Slice<const std::uint8_t> thumb{thumb_bytes.data(),
                                          thumb_bytes.size()};
    return from_ffi(impl_->ffi->send_gif_video(
        room_id, mp4, mime_type, body, width, height, duration_ms, thumb,
        thumb_mime, thumb_width, thumb_height, reply_event_id, thread_root));
}

// ---------------------------------------------------------------------------
// URL preview
// ---------------------------------------------------------------------------

namespace
{

// Encode a Unicode scalar value as UTF-8 into `out`.
void append_utf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parse_hex4(std::string_view json, size_t pos, unsigned& code)
{
    if (pos + 4 > json.size())
    {
        return false;
    }
    code = 0;
    for (int i = 0; i < 4; ++i)
    {
        char h = json[pos + static_cast<size_t>(i)];
        code <<= 4;
        if (h >= '0' && h <= '9')
        {
            code |= static_cast<unsigned>(h - '0');
        }
        else if (h >= 'a' && h <= 'f')
        {
            code |= static_cast<unsigned>(h - 'a' + 10);
        }
        else if (h >= 'A' && h <= 'F')
        {
            code |= static_cast<unsigned>(h - 'A' + 10);
        }
        else
        {
            return false;
        }
    }
    return true;
}

// Locate `key` as an *object key* and return the index of its value (first
// non-whitespace char after the `:`), or npos. Only accepts a match whose
// quoted key is at a structural key position (preceded by `{` or `,`, then
// followed by `:`), so the key text appearing inside a string *value* no
// longer produces a false match. The JSON we parse here is flat output from
// serde_json, so this structural check is sufficient.
size_t find_value(std::string_view json, std::string_view key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t idx = 0;
    while (true)
    {
        auto hit = json.find(needle, idx);
        if (hit == std::string_view::npos)
        {
            return std::string_view::npos;
        }

        // Preceding non-whitespace char must be `{` or `,`.
        size_t b = hit;
        while (b > 0 && (json[b - 1] == ' ' || json[b - 1] == '\t' ||
                         json[b - 1] == '\n' || json[b - 1] == '\r'))
        {
            --b;
        }
        bool key_pos = (b == 0) || json[b - 1] == '{' || json[b - 1] == ',';

        if (key_pos)
        {
            size_t p = hit + needle.size();
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                                       json[p] == '\n' || json[p] == '\r'))
            {
                ++p;
            }
            if (p < json.size() && json[p] == ':')
            {
                ++p;
                while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                                           json[p] == '\n' || json[p] == '\r'))
                {
                    ++p;
                }
                return p;
            }
        }
        idx = hit + 1;
    }
}

std::string json_string_field(std::string_view json, std::string_view key)
{
    size_t pos = find_value(json, key);
    if (pos == std::string_view::npos)
    {
        return {};
    }
    if (pos >= json.size() || json[pos] != '"')
    {
        return {};
    }
    ++pos;
    std::string out;
    for (; pos < json.size() && json[pos] != '"'; ++pos)
    {
        if (json[pos] == '\\' && pos + 1 < json.size())
        {
            ++pos;
            switch (json[pos])
            {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!parse_hex4(json, pos + 1, code))
                {
                    break;
                }
                pos += 4;
                // Combine a UTF-16 surrogate pair into one scalar so
                // supplementary-plane chars (emoji in og:title, …) are
                // valid UTF-8 instead of ill-formed surrogate halves.
                if (code >= 0xD800 && code <= 0xDBFF)
                {
                    unsigned lo = 0;
                    if (pos + 6 < json.size() && json[pos + 1] == '\\' &&
                        json[pos + 2] == 'u' && parse_hex4(json, pos + 3, lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        code =
                            0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                        pos += 6;
                    }
                    else
                    {
                        code = 0xFFFD; // lone high surrogate
                    }
                }
                else if (code >= 0xDC00 && code <= 0xDFFF)
                {
                    code = 0xFFFD; // lone low surrogate
                }
                append_utf8(out, code);
                break;
            }
            default:
                out += json[pos];
                break;
            }
        }
        else
        {
            out += json[pos];
        }
    }
    return out;
}

uint32_t json_int_field(std::string_view json, std::string_view key)
{
    size_t pos = find_value(json, key);
    if (pos == std::string_view::npos)
    {
        return 0;
    }
    uint64_t v = 0;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos])))
    {
        v = v * 10 + static_cast<uint64_t>(json[pos++] - '0');
        if (v >= 0xFFFFFFFFull)
        {
            return 0xFFFFFFFFu; // saturate, never wrap
        }
    }
    return static_cast<uint32_t>(v);
}

bool json_bool_field(std::string_view json, std::string_view key)
{
    size_t pos = find_value(json, key);
    if (pos == std::string_view::npos)
    {
        return false;
    }
    return json.substr(pos, 4) == "true";
}

// ---------------------------------------------------------------------------
// ServerInfo JSON helpers
// ---------------------------------------------------------------------------

std::vector<tesseract::SpecVersion>
si_parse_spec_versions(std::string_view json)
{
    std::vector<tesseract::SpecVersion> out;
    const auto pos0 = find_value(json, "spec_versions");
    if (pos0 == std::string_view::npos)
        return out;
    auto pos = pos0;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != '[')
        return out;
    ++pos;
    while (pos < json.size())
    {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ',' ||
                json[pos] == '\n' || json[pos] == '\r'))
            ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break;
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) break;
        auto raw = json.substr(pos, end - pos); // e.g. "v1.6"
        pos = end + 1;
        if (raw.size() >= 2 && raw[0] == 'v') {
            int major = 0, minor = 0;
            const char* p = raw.data() + 1;
            while (*p >= '0' && *p <= '9') major = major * 10 + (*p++ - '0');
            if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') minor = minor * 10 + (*p++ - '0'); }
            out.push_back({major, minor});
        }
    }
    return out;
}

bool si_extract_bool(std::string_view json, std::string_view key,
                     bool default_val)
{
    const auto pos = find_value(json, key);
    if (pos == std::string_view::npos)
        return default_val;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")
        return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false")
        return false;
    return default_val;
}

} // namespace

// ---------------------------------------------------------------------------
// MSC3266 room summary / join
// ---------------------------------------------------------------------------

RoomSummary Client::get_room_summary(const std::string& room_id_or_alias)
{
    std::string json =
        std::string(impl_->ffi->get_room_summary(room_id_or_alias));
    if (json.empty())
    {
        return {};
    }
    RoomSummary s;
    s.room_id = json_string_field(json, "room_id");
    s.canonical_alias = json_string_field(json, "canonical_alias");
    s.name = json_string_field(json, "name");
    s.topic = json_string_field(json, "topic");
    s.avatar_url = json_string_field(json, "avatar_url");
    s.num_joined_members = json_int_field(json, "num_joined_members");
    s.join_rule = json_string_field(json, "join_rule");
    s.world_readable = json_bool_field(json, "world_readable");
    s.guest_can_join = json_bool_field(json, "guest_can_join");
    s.encryption = json_string_field(json, "encryption");
    s.is_space = json_bool_field(json, "is_space");
    s.membership = json_string_field(json, "membership");
    return s;
}

std::string Client::join_room(const std::string& room_id_or_alias)
{
    MUT_FFI;
    return std::string(impl_->ffi->join_room(room_id_or_alias));
}

Result Client::leave_room(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->leave_room(room_id));
}

Result Client::invite_user(const std::string& room_id, const std::string& user_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->invite_user(room_id, user_id));
}

std::vector<RoomMember> Client::get_room_members(const std::string& room_id)
{
    auto raw = impl_->ffi->get_room_members(room_id);
    std::vector<RoomMember> out;
    out.reserve(raw.size());
    for (const auto& m : raw)
    {
        out.push_back({std::string(m.user_id), std::string(m.display_name),
                       std::string(m.avatar_url)});
    }
    return out;
}

Result Client::set_room_topic(const std::string& room_id, const std::string& topic)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->set_room_topic(room_id, topic));
}

Result Client::set_room_display_name(const std::string& room_id,
                                      const std::string& name)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->set_room_display_name(room_id, name));
}

Result Client::set_room_avatar(const std::string& room_id,
                               const std::string& mxc_uri)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->set_room_avatar(room_id, mxc_uri));
}

Result Client::pin_event(const std::string& room_id, const std::string& event_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->pin_event(room_id, event_id));
}

Result Client::unpin_event(const std::string& room_id, const std::string& event_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->unpin_event(room_id, event_id));
}

bool Client::can_pin_in_room(const std::string& room_id)
{
    return impl_->ffi->can_pin_in_room(room_id);
}

std::string Client::get_room_notification_mode(std::string room_id) const
{
    return std::string(impl_->ffi->get_room_notification_mode(room_id));
}

void Client::set_room_notification_mode(std::string room_id, std::string mode)
{
    MUT_FFI;
    impl_->ffi->set_room_notification_mode(room_id, mode);
}

void Client::set_room_favourite(std::string room_id, bool value)
{
    MUT_FFI;
    impl_->ffi->set_room_favourite(room_id, value);
}

void Client::set_room_low_priority(std::string room_id, bool value)
{
    MUT_FFI;
    impl_->ffi->set_room_low_priority(room_id, value);
}

Result Client::ignore_user(const std::string& user_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->ignore_user(user_id));
}

Result Client::unignore_user(const std::string& user_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->unignore_user(user_id));
}

std::string Client::get_or_create_dm(const std::string& user_id)
{
    MUT_FFI;
    return std::string(impl_->ffi->get_or_create_dm(user_id));
}

Client::DiscoveryResult
Client::discover_homeserver(const std::string& server_name_or_mxid)
{
    MUT_FFI;
    std::string json =
        std::string(impl_->ffi->discover_homeserver(server_name_or_mxid));
    std::string base_url = json_string_field(json, "base_url");
    std::string error = json_string_field(json, "error");
    return DiscoveryResult{error.empty() && !base_url.empty(),
                           std::move(base_url), std::move(error)};
}

// ---------------------------------------------------------------------------
// ServerInfo
// ---------------------------------------------------------------------------

tesseract::ServerInfo tesseract::ServerInfo::from_json(const std::string& json)
{
    if (json.empty())
        return {};
    ServerInfo info;
    info.homeserver_url       = json_string_field(json, "homeserver");
    info.spec_versions        = si_parse_spec_versions(json);
    info.supports_msc3030     = si_extract_bool(json, "supports_msc3030", false);
    // MSC3030 (Jump to Date) was stabilised in spec v1.6.
    for (const auto& sv : info.spec_versions)
        if (sv.at_least(1, 6)) { info.supports_msc3030 = true; break; }
    info.can_change_password  = si_extract_bool(json, "can_change_password", true);
    info.can_set_displayname  = si_extract_bool(json, "can_set_displayname", true);
    info.can_set_avatar       = si_extract_bool(json, "can_set_avatar", true);
    info.default_room_version = json_string_field(json, "default_room_version");
    return info;
}

ServerInfo Client::get_server_info() const
{
    return ServerInfo::from_json(std::string(impl_->ffi->get_server_info()));
}

Client::UrlPreview Client::parse_url_preview(const std::string& json)
{
    if (json.empty())
    {
        UrlPreview p;
        p.failed = true;
        return p;
    }
    UrlPreview p;
    p.title = json_string_field(json, "og:title");
    p.description = json_string_field(json, "og:description");
    p.image_mxc = json_string_field(json, "og:image");
    p.image_w = json_int_field(json, "og:image:width");
    p.image_h = json_int_field(json, "og:image:height");
    if (!p.has_content())
    {
        p.failed = true;
    }
    return p;
}

Client::UrlPreview Client::get_url_preview(const std::string& url)
{
    return parse_url_preview(std::string(impl_->ffi->get_url_preview(url)));
}

// ---------------------------------------------------------------------------
// MSC2545 image packs
// ---------------------------------------------------------------------------

std::vector<ImagePack> Client::list_image_packs() const
{
    return ffi_vec<ImagePack>(impl_->ffi->list_image_packs());
}

std::vector<ImagePackImage>
Client::list_pack_images(const std::string& pack_id,
                         PackUsageFilter filter) const
{
    return ffi_vec<ImagePackImage>(
        impl_->ffi->list_pack_images(pack_id, pack_usage_filter_to_str(filter)));
}

std::vector<ImagePackImage> Client::list_favorite_stickers() const
{
    return ffi_vec<ImagePackImage>(impl_->ffi->list_favorite_stickers());
}

Result Client::send_sticker(const std::string& room_id, const std::string& body,
                            const std::string& image_url,
                            const std::string& info_json)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->send_sticker(room_id, body, image_url, info_json));
}

Result Client::send_thread_sticker(const std::string& room_id,
                                   const std::string& thread_root,
                                   const std::string& body,
                                   const std::string& image_url,
                                   const std::string& info_json)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->send_thread_sticker(room_id, thread_root, body, image_url,
                                        info_json));
}

Result Client::save_sticker_to_user_pack(const std::string& shortcode,
                                         const std::string& body,
                                         const std::string& image_url,
                                         const std::string& info_json)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->save_sticker_to_user_pack(
        shortcode, body, image_url, info_json));
}

bool Client::user_pack_has_sticker(const std::string& image_url,
                                   const std::string& info_json) const
{
    return impl_->ffi->user_pack_has_sticker(image_url, info_json);
}

Result Client::toggle_favorite_sticker(const std::string& image_url)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->toggle_favorite_sticker(image_url));
}

std::vector<std::string>
Client::space_children(const std::string& space_id) const
{
    auto raw = impl_->ffi->space_children(space_id);
    std::vector<std::string> result;
    result.reserve(raw.size());
    for (const auto& s : raw)
    {
        result.push_back(std::string(s));
    }
    return result;
}

bool Client::needs_recovery() const
{
    return impl_->ffi->needs_recovery();
}

Result Client::recover(const std::string& key_or_passphrase)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->recover(key_or_passphrase));
}

BackupProgress Client::backup_state() const
{
    return from_ffi(impl_->ffi->backup_state());
}

uint8_t Client::recovery_state() const
{
    if (!impl_)
        return 0;
    return impl_->ffi->recovery_state();
}

bool Client::own_identity_exists() const
{
    if (!impl_)
        return false;
    return impl_->ffi->own_identity_exists();
}

bool Client::device_verified() const
{
    if (!impl_)
        return false;
    return impl_->ffi->device_verified();
}

Result Client::enable_recovery(const std::string& passphrase)
{
    if (!impl_)
        return {false, "not logged in"};
    MUT_FFI;
    return from_ffi(impl_->ffi->enable_recovery(passphrase));
}

Result Client::export_room_keys(const std::string& path,
                                const std::string& passphrase)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->export_room_keys(path, passphrase));
}

Result Client::import_room_keys(const std::string& path,
                                const std::string& passphrase)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->import_room_keys(path, passphrase));
}

Client::CryptoResetBegin Client::begin_reset_crypto_identity()
{
    MUT_FFI;
    auto r = impl_->ffi->begin_reset_crypto_identity();
    CryptoResetBegin out;
    out.ok = r.ok;
    out.message = std::string(r.message);
    out.needs_approval = r.needs_approval;
    out.approval_url = std::string(r.approval_url);
    return out;
}

void Client::cancel_reset_crypto_identity()
{
    MUT_FFI;
    impl_->ffi->cancel_reset_crypto_identity();
}

void Client::set_presence_polling_enabled(bool enabled)
{
    MUT_FFI;
    impl_->ffi->set_presence_polling_enabled(enabled);
}

void Client::poll_presence_now()
{
    MUT_FFI;
    impl_->ffi->poll_presence_now();
}

Result Client::request_self_verification()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->request_self_verification());
}

Result Client::accept_verification(const std::string& flow_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->accept_verification(flow_id));
}

Result Client::start_sas(const std::string& flow_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->start_sas(flow_id));
}

Result Client::confirm_sas(const std::string& flow_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->confirm_sas(flow_id));
}

Result Client::cancel_verification(const std::string& flow_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->cancel_verification(flow_id));
}

std::vector<VerificationEmoji>
Client::get_sas_emojis(const std::string& flow_id) const
{
    auto ffi_vec = impl_->ffi->get_sas_emojis(flow_id);
    std::vector<VerificationEmoji> result;
    result.reserve(ffi_vec.size());
    for (const auto& e : ffi_vec)
    {
        result.push_back({std::string(e.symbol), std::string(e.description)});
    }
    return result;
}

Result Client::register_pusher(const std::string& pushkey,
                               const std::string& app_id,
                               const std::string& app_display_name,
                               const std::string& device_display_name,
                               const std::string& endpoint_url,
                               const std::string& lang)
{
    MUT_FFI;
    return from_ffi(
        impl_->ffi->register_pusher(pushkey, app_id, app_display_name,
                                    device_display_name, endpoint_url, lang));
}

Result Client::remove_pusher(const std::string& pushkey,
                             const std::string& app_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->remove_pusher(pushkey, app_id));
}

Result Client::hint_push_room(const std::string& room_id)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->hint_push_room(room_id));
}

} // namespace tesseract
