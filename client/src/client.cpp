#include "tesseract/client.h"
#include "tesseract/event_handler_bridge.h"
#include "tesseract/markdown.h"
#include "tesseract/settings.h"

// cxx-generated header (produced by corrosion_add_cxxbridge)
#include "ffi_convert.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <mutex>
#include <shared_mutex>

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
    // Guards EVERY call that crosses the cxx boundary into ClientFfi, as a
    // reader/writer lock. The receiver of each bridge method decides the mode:
    //
    //   * `&mut ClientFfi` methods  -> MUT_FFI (unique_lock, exclusive).
    //   * `&ClientFfi`     methods  -> SH_FFI  (shared_lock, concurrent).
    //
    // Soundness: cxx maps `&self` to a shared `&` borrow of the boxed ClientFfi;
    // any number of those may overlap because every field a `&self` method
    // touches is `Sync` and mutated only through interior-mutability primitives
    // (parking_lot RwLock/Mutex, atomics, Arc<Mutex<…>>) on the Rust side. cxx
    // maps `&mut self` to a `&mut` borrow, which aliased with ANY other borrow
    // is undefined behaviour — hence those keep the exclusive lock.
    //
    // Why this matters (the bug it fixes): the lock is held across blocking
    // `block_on` calls (e.g. subscribe_room building a timeline on a worker).
    // Previously a single coarse mutex serialised everything, so a worker mid-
    // block_on blocked the UI thread the instant it did a cheap read
    // (list_room_threads / subscribe_room_threads during a room switch) — a
    // visible freeze. Those reads are now `&self`/shared and run concurrently
    // with the worker's `&self`/shared block_on, so they no longer block.
    //
    // Residual: a handful of genuine writers still do a long block_on under the
    // unique lock — restore_session, oauth_begin/await, start_sync, stop_sync,
    // clear_caches, logout. All are rare/one-shot and off the room-switch path,
    // so they are acceptable; do not add new long-blocking `&mut self` methods.
    //
    // `mutable` so the lock can be taken inside `const` reader methods.
    mutable std::shared_mutex ffi_mu;

    explicit Impl()
        : ffi(tesseract_ffi::client_create(
              Settings::instance().sdk_log_level.c_str()))
    {
    }
};

// Exclusive lock for wrappers that call a `&mut ClientFfi` bridge method.
#define MUT_FFI std::unique_lock<std::shared_mutex> _fmu_(impl_->ffi_mu)
// Shared lock for wrappers that call a `&ClientFfi` bridge method — these run
// concurrently with each other (and never block behind a shared block_on).
#define SH_FFI std::shared_lock<std::shared_mutex> _fsu_(impl_->ffi_mu)

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
    SH_FFI;
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

Client::QrGrantBitmap Client::begin_qr_grant()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->qr_grant_start());
}

Result Client::await_qr_scanned()
{
    SH_FFI;
    return from_ffi(impl_->ffi->qr_grant_await_scanned());
}

Result Client::submit_qr_check_code(uint8_t code)
{
    SH_FFI;
    return from_ffi(impl_->ffi->qr_grant_submit_check_code(code));
}

Client::QrGrantAuth Client::await_qr_auth()
{
    SH_FFI;
    return from_ffi(impl_->ffi->qr_grant_await_auth());
}

Result Client::await_qr_complete()
{
    SH_FFI;
    return from_ffi(impl_->ffi->qr_grant_await_complete());
}

void Client::cancel_qr_grant()
{
    SH_FFI;
    impl_->ffi->qr_grant_cancel();
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

Client::MatrixLink Client::parse_matrix_link(const std::string& uri)
{
    auto r = tesseract_ffi::parse_matrix_link(uri);
    return MatrixLink{
        static_cast<MatrixLink::Kind>(r.kind),
        std::string(r.primary),
        std::string(r.event_id),
    };
}

Result Client::restore_session(const std::string& session_json)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->restore_session(session_json));
}

std::string Client::export_session() const
{
    SH_FFI;
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
    SH_FFI;
    return impl_->ffi->in_flight_count();
}

std::vector<InviteInfo> Client::list_invites() const
{
    SH_FFI;
    return ffi_vec<InviteInfo>(impl_->ffi->list_invites());
}

void Client::accept_invite_async(std::uint64_t request_id,
                                  const std::string& room_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->accept_invite_async(request_id, room_id);
}

void Client::decline_invite_async(const std::string& room_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->decline_invite_async(room_id);
}

void Client::block_invite_async(const std::string& room_id,
                                 const std::string& inviter_user_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->block_invite_async(room_id, inviter_user_id);
}

Result Client::subscribe_room(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->subscribe_room(room_id));
}

void Client::unsubscribe_room(const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->unsubscribe_room(room_id);
}

PaginateResult Client::paginate_back_with_status(const std::string& room_id,
                                                 std::uint16_t count)
{
    SH_FFI;
    return from_ffi(impl_->ffi->paginate_back_with_status(room_id, count));
}

void Client::paginate_back_async(std::uint64_t request_id,
                                 const std::string& room_id,
                                 std::uint16_t count)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->paginate_back_async(request_id, room_id, count);
}

void Client::cancel_paginate_back(std::uint64_t request_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->cancel_paginate_back(request_id);
}

void Client::paginate_forward_async(std::uint64_t request_id,
                                    const std::string& room_id,
                                    std::uint16_t count)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->paginate_forward_async(request_id, room_id, count);
}

Result Client::timestamp_to_event(const std::string& room_id, uint64_t ts_ms,
                                  const std::string& dir)
{
    SH_FFI;
    if (!impl_)
    {
        return {false, "client not initialised"};
    }
    return from_ffi(impl_->ffi->timestamp_to_event(room_id, ts_ms, dir));
}

Result Client::subscribe_room_at(const std::string& room_id,
                                 const std::string& focus_event_id)
{
    SH_FFI;
    if (!impl_)
    {
        return {false, "client not initialised"};
    }
    return from_ffi(impl_->ffi->subscribe_room_at(room_id, focus_event_id));
}

Result Client::subscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->subscribe_thread(room_id, root_event_id));
}

void Client::unsubscribe_thread(const std::string& room_id,
                                const std::string& root_event_id)
{
    SH_FFI;
    impl_->ffi->unsubscribe_thread(room_id, root_event_id);
}

PaginateResult Client::paginate_thread_back(const std::string& room_id,
                                            const std::string& root_event_id,
                                            std::uint16_t count)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->paginate_thread_back(room_id, root_event_id, count));
}

Result Client::subscribe_room_threads(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->subscribe_room_threads(room_id));
}

void Client::unsubscribe_room_threads(const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->unsubscribe_room_threads(room_id);
}

std::vector<ThreadInfo> Client::list_room_threads(const std::string& room_id)
{
    // Reads the plain (un-synchronised) self.thread_lists HashMap on the Rust
    // side, which subscribe_room_threads / unsubscribe_room_threads / drop
    // mutate under &mut self. Take ffi_mu so this read serialises with those
    // writers (no data race on the HashMap). Cheap: the underlying items()
    // call is an in-memory snapshot, not a network round-trip.
    SH_FFI;
    return ffi_vec<ThreadInfo>(impl_->ffi->list_room_threads(room_id));
}

PaginateResult Client::paginate_room_threads(const std::string& room_id)
{
    SH_FFI;
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

Result
Client::start_bridge_status_check(const std::vector<std::string>& room_ids)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->start_bridge_status_check(room_ids));
}

Result
Client::start_unread_prefetch(const std::vector<std::string>& room_ids)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->start_unread_prefetch(room_ids));
}

void Client::stop_unread_prefetch()
{
    MUT_FFI;
    impl_->ffi->stop_unread_prefetch();
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
    SH_FFI;
    return from_ffi(impl_->ffi->send_message(
        room_id, body, derive_formatted(body, formatted_body)));
}

Result Client::send_emote(const std::string& room_id, const std::string& body,
                          const std::string& formatted_body)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_emote(
        room_id, body, derive_formatted(body, formatted_body)));
}

Result Client::retry_send(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->retry_send(room_id));
}

Result Client::abort_send(const std::string& room_id, const std::string& txn_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->abort_send(room_id, txn_id));
}

void Client::send_typing_notice(const std::string& room_id, bool typing)
{
    SH_FFI;
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
    SH_FFI;
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
    SH_FFI;
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
    SH_FFI;
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
    SH_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->send_file(room_id, slice, mime_type, filename,
                                          caption, reply_event_id, thread_root));
}

void Client::send_image_async(std::uint64_t request_id,
                               const std::string& room_id,
                               const std::vector<uint8_t>& bytes,
                               const std::string& mime_type,
                               const std::string& filename,
                               const std::string& caption,
                               std::uint32_t width, std::uint32_t height,
                               bool is_animated,
                               const std::string& reply_event_id,
                               const std::string& thread_root)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    impl_->ffi->send_image_async(request_id, room_id, slice, mime_type,
                                  filename, caption, width, height, is_animated,
                                  reply_event_id, thread_root);
}

void Client::send_video_async(std::uint64_t request_id,
                               const std::string& room_id,
                               const std::vector<uint8_t>& bytes,
                               const std::string& mime_type,
                               const std::string& filename,
                               const std::string& caption,
                               std::uint32_t width, std::uint32_t height,
                               const std::vector<uint8_t>& thumb_bytes,
                               std::uint32_t thumb_width,
                               std::uint32_t thumb_height,
                               std::uint64_t duration_ms,
                               const std::string& reply_event_id,
                               const std::string& thread_root)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    rust::Slice<const std::uint8_t> thumb_slice{thumb_bytes.data(),
                                                thumb_bytes.size()};
    impl_->ffi->send_video_async(request_id, room_id, slice, mime_type,
                                  filename, caption, width, height, thumb_slice,
                                  thumb_width, thumb_height, duration_ms,
                                  reply_event_id, thread_root);
}

void Client::send_audio_async(std::uint64_t request_id,
                               const std::string& room_id,
                               const std::vector<uint8_t>& bytes,
                               const std::string& mime_type,
                               const std::string& filename,
                               const std::string& caption,
                               std::uint64_t duration_ms,
                               const std::string& reply_event_id,
                               const std::string& thread_root)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    impl_->ffi->send_audio_async(request_id, room_id, slice, mime_type,
                                  filename, caption, duration_ms,
                                  reply_event_id, thread_root);
}

void Client::send_file_async(std::uint64_t request_id,
                              const std::string& room_id,
                              const std::vector<uint8_t>& bytes,
                              const std::string& mime_type,
                              const std::string& filename,
                              const std::string& caption,
                              const std::string& reply_event_id,
                              const std::string& thread_root)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    impl_->ffi->send_file_async(request_id, room_id, slice, mime_type,
                                 filename, caption, reply_event_id, thread_root);
}

Result Client::send_voice(const std::string& room_id,
                          const std::uint8_t* pcm, std::size_t pcm_size,
                          std::uint64_t duration_ms,
                          const std::vector<std::uint16_t>& waveform,
                          const std::string& caption,
                          const std::string& reply_event_id,
                          const std::string& thread_root)
{
    SH_FFI;
    rust::Slice<const std::uint8_t> pcm_slice{pcm, pcm_size};
    rust::Slice<const std::uint16_t> wf_slice{waveform.data(), waveform.size()};
    return from_ffi(impl_->ffi->send_voice(room_id, pcm_slice, duration_ms,
                                            wf_slice, caption,
                                            reply_event_id, thread_root));
}

std::uint64_t Client::media_upload_limit()
{
    SH_FFI;
    return impl_->ffi->media_upload_limit();
}

Result Client::send_read_receipt(const std::string& room_id,
                                 const std::string& event_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_read_receipt(room_id, event_id));
}

Result Client::mark_room_as_read(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->mark_room_as_read(room_id));
}

Result Client::send_reaction(const std::string& room_id,
                             const std::string& event_id,
                             const std::string& key)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_reaction(room_id, event_id, key));
}

Result Client::send_reaction_custom(const std::string& room_id,
                                    const std::string& event_id,
                                    const std::string& key,
                                    const std::string& shortcode)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->send_reaction_custom(room_id, event_id, key, shortcode));
}

Result Client::redact_event(const std::string& room_id,
                            const std::string& event_id,
                            const std::string& reason)
{
    SH_FFI;
    return from_ffi(impl_->ffi->redact_event(room_id, event_id, reason));
}

Result Client::send_reply(const std::string& room_id,
                          const std::string& event_id, const std::string& body,
                          const std::string& formatted_body)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_reply(
        room_id, event_id, body, derive_formatted(body, formatted_body)));
}

Result Client::send_thread_message(const std::string& room_id,
                                   const std::string& thread_root,
                                   const std::string& body,
                                   const std::string& formatted_body)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_thread_message(
        room_id, thread_root, body, derive_formatted(body, formatted_body)));
}

Result Client::send_thread_reply(const std::string& room_id,
                                 const std::string& thread_root,
                                 const std::string& in_reply_to_event_id,
                                 const std::string& body,
                                 const std::string& formatted_body)
{
    SH_FFI;
    return from_ffi(impl_->ffi->send_thread_reply(
        room_id, thread_root, in_reply_to_event_id, body,
        derive_formatted(body, formatted_body)));
}

Result Client::fetch_reply_details(const std::string& room_id,
                                   const std::string& event_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->fetch_reply_details(room_id, event_id));
}

Result Client::send_edit(const std::string& room_id,
                         const std::string& event_id,
                         const std::string& new_body,
                         const std::string& formatted_body)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->send_edit(room_id, event_id, new_body,
                              derive_formatted(new_body, formatted_body)));
}

void Client::forward_event(std::uint64_t      request_id,
                            const std::string& source_room_id,
                            const std::string& event_id,
                            const std::string& target_room_id)
{
    SH_FFI;
    impl_->ffi->forward_event(request_id, source_room_id, event_id, target_room_id);
}

std::string Client::load_prefs_json()
{
    SH_FFI;
    return std::string(impl_->ffi->load_prefs());
}

void Client::save_prefs_json(const std::string& json)
{
    SH_FFI;
    impl_->ffi->save_prefs(json);
}

void Client::save_media_preview_config(MediaPreviewConfig::Mode media_previews,
                                       bool invite_avatars)
{
    SH_FFI;
    impl_->ffi->set_media_preview_config(
        static_cast<std::uint8_t>(media_previews), invite_avatars);
}

void Client::save_room_media_preview_override(const std::string& room_id,
                                               bool has_override,
                                               MediaPreviewConfig::Mode media_previews)
{
    SH_FFI;
    impl_->ffi->set_room_media_preview_override(
        room_id, has_override, static_cast<std::uint8_t>(media_previews));
}

std::vector<std::string> Client::recent_emoji_top(std::uint32_t n)
{
    SH_FFI;
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
    SH_FFI;
    impl_->ffi->recent_emoji_bump(glyph);
}

std::string Client::get_user_id() const
{
    SH_FFI;
    return std::string(impl_->ffi->user_id());
}

std::string Client::get_display_name() const
{
    SH_FFI;
    return std::string(impl_->ffi->current_user_display_name());
}

std::string Client::get_avatar_url() const
{
    SH_FFI;
    return std::string(impl_->ffi->current_user_avatar_url());
}

Result Client::set_display_name(const std::string& name)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_display_name(name));
}

Result Client::upload_avatar(const std::vector<uint8_t>& bytes,
                              const std::string& mime_type)
{
    SH_FFI;
    auto slice = rust::Slice<const uint8_t>{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->upload_avatar(slice, mime_type));
}

Result Client::upload_media(const std::vector<uint8_t>& bytes,
                             const std::string& mime_type)
{
    SH_FFI;
    auto slice = rust::Slice<const uint8_t>{bytes.data(), bytes.size()};
    return from_ffi(impl_->ffi->upload_media(slice, mime_type));
}

Result Client::remove_avatar()
{
    SH_FFI;
    return from_ffi(impl_->ffi->remove_avatar());
}

std::string Client::get_device_id() const
{
    SH_FFI;
    return std::string(impl_->ffi->device_id());
}

std::vector<Client::Device> Client::list_devices() const
{
    SH_FFI;
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
    SH_FFI;
    return from_ffi(impl_->ffi->set_device_display_name(device_id, name));
}

Client::DeleteDeviceBegin
Client::begin_delete_device(const std::string& device_id)
{
    SH_FFI;
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
    SH_FFI;
    return from_ffi(impl_->ffi->complete_delete_device(device_id, session));
}

Result Client::set_presence(PresenceState state)
{
    SH_FFI;
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

void Client::set_presence_async(PresenceState state)
{
    SH_FFI;
    std::uint8_t byte = 3;
    switch (state)
    {
        case PresenceState::Online:      byte = 1; break;
        case PresenceState::Unavailable: byte = 2; break;
        case PresenceState::Offline:     byte = 3; break;
    }
    impl_->ffi->set_presence_async(byte);
}

std::vector<uint8_t> Client::fetch_source_bytes(const std::string& source)
{
    SH_FFI;
    auto v = impl_->ffi->fetch_source_bytes(source);
    return std::vector<uint8_t>(v.begin(), v.end());
}

void Client::fetch_source_bytes_async(std::uint64_t request_id,
                                       const std::string& source_json)
{
    SH_FFI;
    impl_->ffi->fetch_source_bytes_async(request_id, source_json);
}

// ---------------------------------------------------------------------------
// Async media downloads (dispatch returns immediately; the actual download
// runs on a Rust worker pool). The dispatch call reaches `&ClientFfi`, so it
// takes the shared lock (SH_FFI) and never blocks concurrent readers.
// ---------------------------------------------------------------------------

void Client::fetch_media_async(std::uint64_t request_id, std::uint64_t group_id,
                               MediaReqKind kind, const std::string& source,
                               std::uint32_t w, std::uint32_t h, bool animated,
                               MediaPriority priority)
{
    SH_FFI;
    impl_->ffi->fetch_media_async(request_id, group_id,
                                  static_cast<std::uint8_t>(priority),
                                  static_cast<std::uint8_t>(kind), source, w, h,
                                  animated);
}

void Client::prioritize_media(std::uint64_t group_id,
                              const std::vector<std::uint64_t>& request_ids)
{
    if (request_ids.empty())
        return;
    SH_FFI;
    rust::Slice<const std::uint64_t> ids{request_ids.data(), request_ids.size()};
    impl_->ffi->prioritize_media(group_id, ids);
}

void Client::cancel_media_group(std::uint64_t group_id)
{
    SH_FFI;
    impl_->ffi->cancel_media_group(group_id);
}

void Client::cancel_space_summaries(const std::string& space_id)
{
    SH_FFI;
    impl_->ffi->cancel_space_summaries(space_id);
}

void Client::get_url_preview_async(std::uint64_t request_id,
                                   std::uint64_t group_id,
                                   const std::string& url)
{
    SH_FFI;
    impl_->ffi->get_url_preview_async(request_id, group_id, url);
}

void Client::fetch_url_async(std::uint64_t request_id, std::uint64_t group_id,
                             const std::string& url)
{
    SH_FFI;
    impl_->ffi->fetch_url_async(request_id, group_id, url);
}

Client::UpdateResult Client::check_for_update(const std::string& repo,
                                               const std::string& current_version)
{
    SH_FFI;
    auto r = impl_->ffi->check_for_update(repo, current_version);
    return { r.has_update, std::string(r.version), std::string(r.url) };
}

void Client::gif_search(std::uint64_t request_id, const std::string& query,
                        const std::string& api_key,
                        const std::string& client_key, std::uint32_t limit)
{
    SH_FFI;
    impl_->ffi->gif_search_async(request_id, query, api_key, client_key, limit);
}

void Client::set_search_indexing_enabled(bool enabled)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->set_search_indexing_enabled(enabled);
}

void Client::search_messages(std::uint64_t request_id, const std::string& query,
                             const std::string& room_id, std::uint32_t limit)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->search_messages_async(request_id, query, room_id, limit);
}

SearchIndexStats Client::search_index_stats() const
{
    if (!impl_)
    {
        return {};
    }
    SH_FFI;
    return from_ffi(impl_->ffi->search_index_stats());
}

std::uint64_t Client::search_index_size_bytes() const
{
    if (!impl_)
    {
        return 0;
    }
    SH_FFI;
    return impl_->ffi->search_index_size_bytes();
}

std::vector<MediaBackoffEntry> Client::load_media_backoff() const
{
    if (!impl_)
    {
        return {};
    }
    SH_FFI;
    auto ffi_vec = impl_->ffi->load_media_backoff();
    std::vector<MediaBackoffEntry> out;
    out.reserve(ffi_vec.size());
    for (const auto& e : ffi_vec)
        out.push_back(from_ffi(e));
    return out;
}

void Client::note_media_backoff_failed(const std::string& url,
                                        std::uint32_t       attempts,
                                        std::int64_t        deadline_secs)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->note_media_backoff_failed(url, attempts, deadline_secs);
}

void Client::note_media_backoff_ok(const std::string& url)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->note_media_backoff_ok(url);
}

void Client::clear_media_backoff_db()
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->clear_media_backoff_db();
}

std::vector<RoomSummaryBackoffEntry> Client::load_room_summary_backoff() const
{
    if (!impl_)
    {
        return {};
    }
    SH_FFI;
    auto ffi_vec = impl_->ffi->load_room_summary_backoff();
    std::vector<RoomSummaryBackoffEntry> out;
    out.reserve(ffi_vec.size());
    for (const auto& e : ffi_vec)
        out.push_back(from_ffi(e));
    return out;
}

void Client::note_room_summary_backoff_failed(const std::string& room_id,
                                               std::uint32_t       attempts,
                                               std::int64_t        deadline_secs)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->note_room_summary_backoff_failed(room_id, attempts, deadline_secs);
}

void Client::note_room_summary_backoff_ok(const std::string& room_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->note_room_summary_backoff_ok(room_id);
}

Result Client::send_gif_video(
    const std::string& room_id, const std::vector<uint8_t>& mp4_bytes,
    const std::string& mime_type, const std::string& body, std::uint32_t width,
    std::uint32_t height, std::uint64_t duration_ms,
    const std::vector<uint8_t>& thumb_bytes, const std::string& thumb_mime,
    std::uint32_t thumb_width, std::uint32_t thumb_height,
    const std::string& reply_event_id, const std::string& thread_root)
{
    SH_FFI;
    rust::Slice<const std::uint8_t> mp4{mp4_bytes.data(), mp4_bytes.size()};
    rust::Slice<const std::uint8_t> thumb{thumb_bytes.data(),
                                          thumb_bytes.size()};
    return from_ffi(impl_->ffi->send_gif_video(
        room_id, mp4, mime_type, body, width, height, duration_ms, thumb,
        thumb_mime, thumb_width, thumb_height, reply_event_id, thread_root));
}

void Client::send_gif_from_urls_async(std::uint64_t request_id,
                                       const std::string& room_id,
                                       const std::string& image_url,
                                       const std::string& image_mime,
                                       const std::string& body,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       const std::string& preview_url,
                                       std::uint32_t preview_w,
                                       std::uint32_t preview_h,
                                       const std::string& reply_event_id,
                                       const std::string& thread_root)
{
    SH_FFI;
    impl_->ffi->send_gif_from_urls_async(
        request_id, room_id, image_url, image_mime, body,
        width, height, preview_url, preview_w, preview_h,
        reply_event_id, thread_root);
}

// ---------------------------------------------------------------------------
// URL preview
// ---------------------------------------------------------------------------

namespace
{

// Parse `json` into an nlohmann object, returning an empty object on any parse
// error or when the top level is not an object. Callers then read fields with
// the typed accessors below, which all degrade to their defaults on a missing
// or wrong-typed key — so a malformed blob yields the same all-defaults result
// the previous hand-rolled scanners produced. nlohmann handles UTF-8 escapes
// and UTF-16 surrogate pairs internally, so the bespoke decoder is gone.
nlohmann::json parse_json_obj(const std::string& json)
{
    try
    {
        auto j = nlohmann::json::parse(json);
        if (j.is_object())
        {
            return j;
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }
    return nlohmann::json::object();
}

// String field, or "" if absent/non-string.
std::string js_str(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it != j.end() && it->is_string())
    {
        return it->get<std::string>();
    }
    return {};
}

// Unsigned field saturated to 32 bits. Accepts either a JSON number or a
// numeric string (some OpenGraph servers quote og:image:width); a negative or
// out-of-range value saturates at 0xFFFFFFFF, mirroring the old digit scanner.
uint32_t js_uint(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
    {
        return 0;
    }
    uint64_t v = 0;
    if (it->is_number_unsigned())
    {
        v = it->get<uint64_t>();
    }
    else if (it->is_number_integer())
    {
        auto s = it->get<int64_t>();
        v = s < 0 ? 0 : static_cast<uint64_t>(s);
    }
    else if (it->is_number_float())
    {
        double d = it->get<double>();
        v = d < 0 ? 0 : static_cast<uint64_t>(d);
    }
    else if (it->is_string())
    {
        for (char c : it->get_ref<const std::string&>())
        {
            if (c < '0' || c > '9')
                break;
            v = v * 10 + static_cast<uint64_t>(c - '0');
            if (v >= 0xFFFFFFFFull)
                return 0xFFFFFFFFu;
        }
    }
    else
    {
        return 0;
    }
    return v >= 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

// Bool field, or `default_val` if absent/non-bool.
bool js_bool(const nlohmann::json& j, const char* key, bool default_val)
{
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean())
    {
        return it->get<bool>();
    }
    return default_val;
}

// Parse the `spec_versions` array of "vMAJOR.MINOR" strings.
std::vector<tesseract::SpecVersion>
si_parse_spec_versions(const nlohmann::json& j)
{
    std::vector<tesseract::SpecVersion> out;
    auto it = j.find("spec_versions");
    if (it == j.end() || !it->is_array())
    {
        return out;
    }
    for (const auto& elem : *it)
    {
        if (!elem.is_string())
            continue;
        const std::string& raw = elem.get_ref<const std::string&>();
        if (raw.size() >= 2 && raw[0] == 'v')
        {
            int major = 0, minor = 0;
            const char* p = raw.data() + 1;
            const char* end = raw.data() + raw.size();
            while (p < end && *p >= '0' && *p <= '9')
                major = major * 10 + (*p++ - '0');
            if (p < end && *p == '.')
            {
                ++p;
                while (p < end && *p >= '0' && *p <= '9')
                    minor = minor * 10 + (*p++ - '0');
            }
            out.push_back({major, minor});
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// MSC3266 room summary / join
// ---------------------------------------------------------------------------

static RoomSummary parse_room_summary_json(const std::string& json)
{
    if (json.empty())
        return {};
    nlohmann::json j = parse_json_obj(json);
    RoomSummary s;
    s.room_id            = js_str(j, "room_id");
    s.canonical_alias    = js_str(j, "canonical_alias");
    s.name               = js_str(j, "name");
    s.topic              = js_str(j, "topic");
    s.avatar_url         = js_str(j, "avatar_url");
    s.num_joined_members = js_uint(j, "num_joined_members");
    s.join_rule          = js_str(j, "join_rule");
    s.world_readable     = js_bool(j, "world_readable", false);
    s.guest_can_join     = js_bool(j, "guest_can_join", false);
    s.encryption         = js_str(j, "encryption");
    s.is_space           = js_bool(j, "is_space", false);
    s.membership         = js_str(j, "membership");
    return s;
}

RoomSummary Client::get_room_summary(const std::string& room_id_or_alias)
{
    SH_FFI;
    return parse_room_summary_json(
        std::string(impl_->ffi->get_room_summary(room_id_or_alias)));
}

std::optional<tesseract::RoomSummary>
Client::get_space_child_summary(const std::string& space_id,
                                const std::string& room_id)
{
    SH_FFI;
    const std::string json = std::string(
        impl_->ffi->get_space_child_summary(space_id, room_id));
    if (json.empty())
        return std::nullopt;
    try
    {
        return parse_room_summary_json(json);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

void Client::get_space_child_summary_async(std::uint64_t request_id,
                                           const std::string& space_id,
                                           const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->get_space_child_summary_async(request_id, space_id, room_id);
}

void Client::get_server_info_async(std::uint64_t request_id)
{
    SH_FFI;
    impl_->ffi->get_server_info_async(request_id);
}

void Client::media_preview_config_async(std::uint64_t request_id)
{
    SH_FFI;
    impl_->ffi->media_preview_config_async(request_id);
}

void Client::room_media_preview_override_async(std::uint64_t request_id,
                                               const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->room_media_preview_override_async(request_id, room_id);
}

void Client::fetch_room_security_state_async(std::uint64_t request_id,
                                              const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->fetch_room_security_state_async(request_id, room_id);
}

std::optional<tesseract::RoomSummary>
Client::get_cached_room_summary(const std::string& room_id) const
{
    if (!impl_)
    {
        return std::nullopt;
    }
    SH_FFI;
    const std::string json =
        std::string(impl_->ffi->get_cached_room_summary(room_id));
    if (json.empty()) return std::nullopt;
    try { return parse_room_summary_json(json); }
    catch (...) { return std::nullopt; }
}

std::string Client::join_room(const std::string& room_id_or_alias)
{
    SH_FFI;
    return std::string(impl_->ffi->join_room(room_id_or_alias));
}

void Client::join_room_async(std::uint64_t request_id,
                              const std::string& room_id_or_alias)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->join_room_async(request_id, room_id_or_alias);
}

Result Client::leave_room(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->leave_room(room_id));
}

void Client::leave_room_async(std::uint64_t request_id, const std::string& room_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->leave_room_async(request_id, room_id);
}

void Client::invite_user_async(const std::string& room_id,
                                const std::string& user_id)
{
    if (!impl_)
    {
        return;
    }
    SH_FFI;
    impl_->ffi->invite_user_async(room_id, user_id);
}

std::vector<RoomMember> Client::get_room_members(const std::string& room_id)
{
    SH_FFI;
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
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_topic(room_id, topic));
}

Result Client::set_user_room_display_name(const std::string& room_id,
                                          const std::string& name)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_user_room_display_name(room_id, name));
}

Result Client::set_user_room_avatar(const std::string& room_id,
                                    const std::string& mxc_uri)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_user_room_avatar(room_id, mxc_uri));
}

Result Client::set_room_display_name(const std::string& room_id,
                                      const std::string& name)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_display_name(room_id, name));
}

Result Client::set_room_avatar(const std::string& room_id,
                               const std::string& mxc_uri)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_avatar(room_id, mxc_uri));
}

Result Client::set_room_encryption(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_encryption(room_id));
}

Result Client::set_room_join_rule(const std::string& room_id,
                                  const std::string& join_rule)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_join_rule(room_id, join_rule));
}

Result Client::set_room_guest_access(const std::string& room_id, bool allow)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_guest_access(room_id, allow));
}

Result Client::set_room_history_visibility(const std::string& room_id,
                                           const std::string& visibility)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_history_visibility(room_id, visibility));
}

Result Client::pin_event(const std::string& room_id, const std::string& event_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->pin_event(room_id, event_id));
}

Result Client::unpin_event(const std::string& room_id, const std::string& event_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->unpin_event(room_id, event_id));
}

bool Client::can_pin_in_room(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_pin_in_room(room_id);
}

bool Client::can_set_room_name(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_name(room_id);
}

bool Client::can_set_room_topic(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_topic(room_id);
}

bool Client::can_set_room_avatar(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_avatar(room_id);
}

bool Client::can_set_room_encryption(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_encryption(room_id);
}

bool Client::can_set_room_join_rules(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_join_rules(room_id);
}

bool Client::can_set_room_guest_access(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_guest_access(room_id);
}

bool Client::can_set_room_history_visibility(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_history_visibility(room_id);
}

bool Client::can_set_room_power_levels(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_power_levels(room_id);
}

bool Client::can_set_room_image_packs(const std::string& room_id)
{
    SH_FFI;
    return impl_->ffi->can_set_room_image_packs(room_id);
}

RoomPermissions Client::room_power_levels(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->room_power_levels(room_id));
}

Result Client::set_room_power_levels(const std::string& room_id,
                                     const RoomPermissions& levels)
{
    SH_FFI;
    return from_ffi(impl_->ffi->set_room_power_levels(room_id, to_ffi(levels)));
}

RoomOwnPowerLevel Client::room_own_power_level(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->room_own_power_level(room_id));
}

std::string Client::get_room_notification_mode(std::string room_id) const
{
    SH_FFI;
    return std::string(impl_->ffi->get_room_notification_mode(room_id));
}

void Client::set_room_notification_mode(std::string room_id, std::string mode)
{
    SH_FFI;
    impl_->ffi->set_room_notification_mode(room_id, mode);
}

void Client::set_room_favourite(std::string room_id, bool value)
{
    SH_FFI;
    impl_->ffi->set_room_favourite(room_id, value);
}

void Client::set_room_low_priority(std::string room_id, bool value)
{
    SH_FFI;
    impl_->ffi->set_room_low_priority(room_id, value);
}

void Client::ignore_user_async(const std::string& user_id)
{
    if (!impl_) { return; }
    SH_FFI;
    impl_->ffi->ignore_user_async(user_id);
}

void Client::unignore_user_async(const std::string& user_id)
{
    if (!impl_) { return; }
    SH_FFI;
    impl_->ffi->unignore_user_async(user_id);
}

std::string Client::get_or_create_dm(const std::string& user_id)
{
    SH_FFI;
    return std::string(impl_->ffi->get_or_create_dm(user_id));
}

void Client::set_or_delete_profile_field_async(std::uint64_t request_id,
                                               const std::string& key,
                                               const std::string& value_json)
{
    SH_FFI;
    impl_->ffi->set_or_delete_profile_field_async(request_id, key, value_json);
}

void Client::get_extended_profile_async(std::uint64_t request_id,
                                        const std::string& user_id)
{
    SH_FFI;
    impl_->ffi->get_extended_profile_async(request_id, user_id);
}

void Client::resolve_user_profile_async(std::uint64_t request_id,
                                        const std::string& user_id)
{
    SH_FFI;
    impl_->ffi->resolve_user_profile_async(request_id, user_id);
}

Client::DiscoveryResult
Client::discover_homeserver(const std::string& server_name_or_mxid)
{
    SH_FFI;
    std::string json =
        std::string(impl_->ffi->discover_homeserver(server_name_or_mxid));
    nlohmann::json j = parse_json_obj(json);
    std::string base_url = js_str(j, "base_url");
    std::string error = js_str(j, "error");
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
    nlohmann::json j = parse_json_obj(json);
    ServerInfo info;
    info.homeserver_url       = js_str(j, "homeserver");
    info.spec_versions        = si_parse_spec_versions(j);
    info.supports_msc3030     = js_bool(j, "supports_msc3030", false);
    // MSC3030 (Jump to Date) was stabilised in spec v1.6.
    for (const auto& sv : info.spec_versions)
        if (sv.at_least(1, 6)) { info.supports_msc3030 = true; break; }
    info.can_change_password       = js_bool(j, "can_change_password", true);
    info.can_set_displayname       = js_bool(j, "can_set_displayname", true);
    info.can_set_avatar            = js_bool(j, "can_set_avatar", true);
    info.supports_profile_fields   = js_bool(j, "supports_profile_fields", false);
    info.profile_fields_enabled    = js_bool(j, "profile_fields_enabled", true);
    info.supports_qr_grant         = js_bool(j, "supports_qr_grant", false);
    info.supports_calls            = js_bool(j, "supports_calls", false);
    info.default_room_version      = js_str(j, "default_room_version");
    return info;
}

ServerInfo Client::get_server_info() const
{
    SH_FFI;
    return ServerInfo::from_json(std::string(impl_->ffi->get_server_info()));
}

// RoomSummary
// ---------------------------------------------------------------------------

RoomSummary RoomSummary::from_json(const std::string& json)
{
    if (json.empty())
        return {};
    nlohmann::json j = parse_json_obj(json);
    RoomSummary s;
    s.room_id            = js_str(j, "room_id");
    if (s.room_id.empty())
        return {};
    s.canonical_alias    = js_str(j, "canonical_alias");
    s.name               = js_str(j, "name");
    s.topic              = js_str(j, "topic");
    s.avatar_url         = js_str(j, "avatar_url");
    s.num_joined_members = js_uint(j, "num_joined_members");
    s.join_rule          = js_str(j, "join_rule");
    s.world_readable     = js_bool(j, "world_readable", false);
    s.is_space           = js_bool(j, "is_space", false);
    s.membership         = js_str(j, "membership");
    return s;
}

UserProfile UserProfile::from_json(const std::string& json)
{
    UserProfile p;
    if (json.empty())
        return p;
    auto j = parse_json_obj(json);
    p.exists       = j.value("exists", false);
    p.user_id      = js_str(j, "user_id");
    p.display_name = js_str(j, "display_name");
    p.avatar_url   = js_str(j, "avatar_url");
    p.pronouns     = js_str(j, "pronouns");
    p.tz           = js_str(j, "tz");
    p.biography    = js_str(j, "biography");
    return p;
}

MediaPreviewConfig MediaPreviewConfig::from_json(const std::string& json)
{
    MediaPreviewConfig cfg;
    if (json.empty())
        return cfg;
    auto j = parse_json_obj(json);
    // Use nlohmann .value() so missing keys yield the MSC defaults (On / true).
    auto raw = j.value("media_previews", uint32_t(2));
    cfg.media_previews = raw <= 2
        ? static_cast<MediaPreviewConfig::Mode>(raw)
        : MediaPreviewConfig::Mode::On;
    cfg.invite_avatars = j.value("invite_avatars", true);
    return cfg;
}

MediaPreviewOverride MediaPreviewOverride::from_json(const std::string& json)
{
    MediaPreviewOverride ov;
    if (json.empty())
        return ov;
    auto j = parse_json_obj(json);
    ov.has_media_previews = j.value("has_media_previews", false);
    auto raw = j.value("media_previews", uint32_t(2));
    ov.media_previews = raw <= 2
        ? static_cast<MediaPreviewConfig::Mode>(raw)
        : MediaPreviewConfig::Mode::On;
    ov.join_rule = js_str(j, "join_rule");
    return ov;
}

Client::UrlPreview Client::parse_url_preview(const std::string& json)
{
    if (json.empty())
    {
        UrlPreview p;
        p.failed = true;
        return p;
    }
    nlohmann::json j = parse_json_obj(json);
    UrlPreview p;
    p.title = js_str(j, "og:title");
    p.description = js_str(j, "og:description");
    p.image_mxc = js_str(j, "og:image");
    p.image_w = static_cast<int>(js_uint(j, "og:image:width"));
    p.image_h = static_cast<int>(js_uint(j, "og:image:height"));
    if (!p.has_content())
    {
        p.failed = true;
    }
    return p;
}

// ---------------------------------------------------------------------------
// MSC2545 image packs
// ---------------------------------------------------------------------------

std::vector<ImagePack> Client::list_image_packs() const
{
    SH_FFI;
    return ffi_vec<ImagePack>(impl_->ffi->list_image_packs());
}

std::vector<ImagePack> Client::list_known_room_packs() const
{
    SH_FFI;
    return ffi_vec<ImagePack>(impl_->ffi->list_known_room_packs());
}

void Client::set_active_room(const std::string& room_id)
{
    SH_FFI;
    impl_->ffi->set_active_room(room_id);
}

std::vector<ImagePackImage>
Client::list_pack_images(const std::string& pack_id,
                         PackUsageFilter filter) const
{
    SH_FFI;
    return ffi_vec<ImagePackImage>(
        impl_->ffi->list_pack_images(pack_id, pack_usage_filter_to_str(filter)));
}

std::vector<ImagePackImage> Client::list_favorite_stickers() const
{
    SH_FFI;
    return ffi_vec<ImagePackImage>(impl_->ffi->list_favorite_stickers());
}

Result Client::send_sticker(const std::string& room_id, const std::string& body,
                            const std::string& image_url,
                            const std::string& info_json)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->send_sticker(room_id, body, image_url, info_json));
}

Result Client::send_thread_sticker(const std::string& room_id,
                                   const std::string& thread_root,
                                   const std::string& body,
                                   const std::string& image_url,
                                   const std::string& info_json)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->send_thread_sticker(room_id, thread_root, body, image_url,
                                        info_json));
}

Result Client::save_sticker_to_user_pack(const std::string& shortcode,
                                         const std::string& body,
                                         const std::string& image_url,
                                         const std::string& info_json)
{
    SH_FFI;
    return from_ffi(impl_->ffi->save_sticker_to_user_pack(
        shortcode, body, image_url, info_json));
}

bool Client::user_pack_has_sticker(const std::string& image_url,
                                   const std::string& info_json) const
{
    SH_FFI;
    return impl_->ffi->user_pack_has_sticker(image_url, info_json);
}

Result Client::toggle_favorite_sticker(const std::string& image_url)
{
    SH_FFI;
    return from_ffi(impl_->ffi->toggle_favorite_sticker(image_url));
}

Result Client::remove_user_pack_image(const std::string& shortcode)
{
    SH_FFI;
    return from_ffi(impl_->ffi->remove_user_pack_image(shortcode));
}

Result Client::rename_user_pack_image(const std::string& old_shortcode,
                                      const std::string& new_shortcode)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->rename_user_pack_image(old_shortcode, new_shortcode));
}

Result Client::set_pack_room_subscribed(const std::string& room_id,
                                        const std::string& state_key,
                                        bool subscribed)
{
    SH_FFI;
    return from_ffi(
        impl_->ffi->set_pack_room_subscribed(room_id, state_key, subscribed));
}

Result Client::save_room_pack(const std::string& room_id,
                              const std::string& state_key, bool is_new,
                              const std::string& display_name,
                              std::uint8_t usage_mask,
                              const std::vector<PackImageInput>& images)
{
    SH_FFI;
    rust::Vec<tesseract_ffi::PackImageInputFfi> ffi_images;
    for (const auto& img : images)
    {
        ffi_images.push_back(tesseract_ffi::PackImageInputFfi{
            .shortcode  = img.shortcode,
            .url        = img.url,
            .body       = img.body,
            .info_json  = img.info_json,
        });
    }
    return from_ffi(impl_->ffi->save_room_pack(room_id, state_key, is_new,
                                               display_name, usage_mask,
                                               std::move(ffi_images)));
}

Result Client::remove_room_pack(const std::string& room_id,
                                const std::string& state_key)
{
    SH_FFI;
    return from_ffi(impl_->ffi->remove_room_pack(room_id, state_key));
}

std::vector<std::string>
Client::space_children(const std::string& space_id) const
{
    SH_FFI;
    auto raw = impl_->ffi->space_children(space_id);
    std::vector<std::string> result;
    result.reserve(raw.size());
    for (const auto& s : raw)
    {
        result.push_back(std::string(s));
    }
    return result;
}

std::vector<std::string>
Client::space_children_all(const std::string& space_id) const
{
    SH_FFI;
    auto raw = impl_->ffi->space_children_all(space_id);
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
    SH_FFI;
    return impl_->ffi->needs_recovery();
}

Result Client::recover(const std::string& key_or_passphrase)
{
    SH_FFI;
    return from_ffi(impl_->ffi->recover(key_or_passphrase));
}

BackupProgress Client::backup_state() const
{
    SH_FFI;
    return from_ffi(impl_->ffi->backup_state());
}

uint8_t Client::recovery_state() const
{
    if (!impl_)
    {
        return 0;
    }
    SH_FFI;
    return impl_->ffi->recovery_state();
}

bool Client::own_identity_exists() const
{
    if (!impl_)
    {
        return false;
    }
    SH_FFI;
    return impl_->ffi->own_identity_exists();
}

bool Client::device_verified() const
{
    if (!impl_)
    {
        return false;
    }
    SH_FFI;
    return impl_->ffi->device_verified();
}

bool Client::have_cross_signing_keys() const
{
    if (!impl_)
    {
        return false;
    }
    SH_FFI;
    return impl_->ffi->have_cross_signing_keys();
}

Result Client::enable_recovery(const std::string& passphrase)
{
    if (!impl_)
    {
        return {false, "not logged in"};
    }
    SH_FFI;
    return from_ffi(impl_->ffi->enable_recovery(passphrase));
}

Result Client::export_room_keys(const std::string& path,
                                const std::string& passphrase)
{
    SH_FFI;
    return from_ffi(impl_->ffi->export_room_keys(path, passphrase));
}

Result Client::import_room_keys(const std::string& path,
                                const std::string& passphrase)
{
    SH_FFI;
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
    SH_FFI;
    impl_->ffi->set_presence_polling_enabled(enabled);
}

void Client::set_show_membership_events(bool enabled)
{
    SH_FFI;
    impl_->ffi->set_show_membership_events(enabled);
}

void Client::poll_presence_now()
{
    MUT_FFI;
    impl_->ffi->poll_presence_now();
}

Result Client::request_self_verification()
{
    SH_FFI;
    return from_ffi(impl_->ffi->request_self_verification());
}

Result Client::accept_verification(const std::string& flow_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->accept_verification(flow_id));
}

Result Client::start_sas(const std::string& flow_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->start_sas(flow_id));
}

Result Client::confirm_sas(const std::string& flow_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->confirm_sas(flow_id));
}

Result Client::cancel_verification(const std::string& flow_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->cancel_verification(flow_id));
}

std::vector<VerificationEmoji>
Client::get_sas_emojis(const std::string& flow_id) const
{
    SH_FFI;
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
    SH_FFI;
    return from_ffi(
        impl_->ffi->register_pusher(pushkey, app_id, app_display_name,
                                    device_display_name, endpoint_url, lang));
}

Result Client::remove_pusher(const std::string& pushkey,
                             const std::string& app_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->remove_pusher(pushkey, app_id));
}

Result Client::hint_push_room(const std::string& room_id)
{
    SH_FFI;
    return from_ffi(impl_->ffi->hint_push_room(room_id));
}

#ifdef TESSERACT_CALLS_ENABLED
Result Client::rtc_start_call(const std::string& room_id,
                               const std::string& slot_id,
                               bool audio_only)
{
    MUT_FFI;
    return from_ffi(impl_->ffi->rtc_start_call(room_id, slot_id, audio_only));
}

void Client::rtc_end_call()
{
    MUT_FFI;
    impl_->ffi->rtc_end_call();
}

void Client::rtc_set_audio_muted(bool muted)
{
    MUT_FFI;
    impl_->ffi->rtc_set_audio_muted(muted);
}

void Client::rtc_set_video_muted(bool muted)
{
    MUT_FFI;
    impl_->ffi->rtc_set_video_muted(muted);
}

void Client::rtc_push_video_frame_i420(const std::uint8_t* y,
                                        const std::uint8_t* u,
                                        const std::uint8_t* v,
                                        std::uint32_t width, std::uint32_t height,
                                        std::uint32_t stride_y,
                                        std::uint32_t stride_u,
                                        std::uint32_t stride_v)
{
    // Basic sanity: reject degenerate or implausibly large frames.
    // 8192 cap exceeds any real camera resolution and bounds the slice sizes.
    if (width == 0 || height == 0 || stride_y < width ||
        stride_u < (width + 1) / 2 || stride_v < (width + 1) / 2 ||
        width > 8192 || height > 8192)
        return;

    MUT_FFI;
    const std::size_t h_uv   = (static_cast<std::size_t>(height) + 1) / 2;
    const std::size_t y_size = static_cast<std::size_t>(stride_y) * height;
    const std::size_t u_size = static_cast<std::size_t>(stride_u) * h_uv;
    const std::size_t v_size = static_cast<std::size_t>(stride_v) * h_uv;
    impl_->ffi->rtc_push_video_frame_i420(
        rust::Slice<const std::uint8_t>{y, y_size},
        rust::Slice<const std::uint8_t>{u, u_size},
        rust::Slice<const std::uint8_t>{v, v_size},
        width, height, stride_y, stride_u, stride_v);
}

Result Client::rtc_start_screen_share()
{
    MUT_FFI;
    return from_ffi(impl_->ffi->rtc_start_screen_share());
}

void Client::rtc_stop_screen_share()
{
    MUT_FFI;
    impl_->ffi->rtc_stop_screen_share();
}

void Client::rtc_push_screen_frame_i420(const std::uint8_t* y,
                                         const std::uint8_t* u,
                                         const std::uint8_t* v,
                                         std::uint32_t width, std::uint32_t height,
                                         std::uint32_t stride_y,
                                         std::uint32_t stride_u,
                                         std::uint32_t stride_v)
{
    if (!y || !u || !v || width == 0 || height == 0) return;
    const std::uint32_t h_uv   = (height + 1) / 2;
    const std::size_t   y_size = static_cast<std::size_t>(stride_y) * height;
    const std::size_t   u_size = static_cast<std::size_t>(stride_u) * h_uv;
    const std::size_t   v_size = static_cast<std::size_t>(stride_v) * h_uv;
    MUT_FFI;
    impl_->ffi->rtc_push_screen_frame_i420(
        rust::Slice<const std::uint8_t>{y, y_size},
        rust::Slice<const std::uint8_t>{u, u_size},
        rust::Slice<const std::uint8_t>{v, v_size},
        width, height, stride_y, stride_u, stride_v);
}
#endif // TESSERACT_CALLS_ENABLED

#undef MUT_FFI
#undef SH_FFI

} // namespace tesseract
