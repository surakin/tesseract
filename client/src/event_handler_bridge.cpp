#include "tesseract/event_handler_bridge.h"
#include "ffi_convert.h"
#include "session_persist_queue.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace
{
// On a 32-bit target std::size_t is narrower than the Rust u64 index; a
// silent truncation would mutate the wrong timeline row. Reject the
// (impossible-in-practice but undefined) out-of-range case explicitly.
inline bool index_fits(std::uint64_t i)
{
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
    {
        return i <= static_cast<std::uint64_t>(SIZE_MAX);
    }
    else
    {
        return true;
    }
}
} // namespace

namespace tesseract_ffi
{

namespace
{
// Every method below is invoked by cxx-generated Rust trampolines. An
// exception unwinding out of a `void` C++ callback into Rust is undefined
// behavior (the Rust unwinding ABI does not support it). `guard` gives a
// defined failure mode instead: the callback is dropped and the exception
// is logged, never propagated across the FFI boundary.
template <class F>
void guard(const char* where, F&& f) noexcept
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[tesseract] %s threw: %s (dropped)\n", where,
                     e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "[tesseract] %s threw non-exception (dropped)\n",
                     where);
    }
}

// Every EventHandlerBridge callback repeats the same preamble: run under
// guard(), snapshot the handler out of the shared slot, and bail if it has
// been detached. Folding it here means a callback can't forget the null-check,
// and each body keeps only its argument conversion. `fn` receives the
// guaranteed-non-null handler. (Index-bearing callbacks still do their own
// !index_fits() early-return inside `fn`.)
template <class F>
void with_handler(const char* where, const std::shared_ptr<HandlerSlot>& slot,
                  F&& fn) noexcept
{
    guard(where,
          [&]
          {
              auto* handler_ = slot->load();
              if (!handler_)
              {
                  return;
              }
              fn(handler_);
          });
}
} // namespace

void EventHandlerBridge::on_timeline_reset(
    rust::Str room_id, const rust::Vec<TimelineEvent>& snapshot) const
{
    with_handler("on_timeline_reset", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> events;
              events.reserve(snapshot.size());
              for (const auto& ev : snapshot)
              {
                  events.push_back(tesseract::make_event(ev));
              }
              handler_->on_timeline_reset(std::string(room_id),
                                          std::move(events));
          });
}

void EventHandlerBridge::on_message_inserted(rust::Str room_id,
                                             std::uint64_t index,
                                             const TimelineEvent& ev) const
{
    with_handler("on_message_inserted", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_message_inserted(std::string(room_id),
                                            static_cast<std::size_t>(index),
                                            tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_message_updated(rust::Str room_id,
                                            std::uint64_t index,
                                            const TimelineEvent& ev) const
{
    with_handler("on_message_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_message_updated(std::string(room_id),
                                           static_cast<std::size_t>(index),
                                           tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_message_removed(rust::Str room_id,
                                            std::uint64_t index) const
{
    with_handler("on_message_removed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_message_removed(std::string(room_id),
                                           static_cast<std::size_t>(index));
          });
}

void EventHandlerBridge::on_thread_reset(
    rust::Str room_id, rust::Str thread_root,
    const rust::Vec<TimelineEvent>& snapshot) const
{
    with_handler("on_thread_reset", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> events;
              events.reserve(snapshot.size());
              for (const auto& ev : snapshot)
              {
                  events.push_back(tesseract::make_event(ev));
              }
              handler_->on_thread_reset(std::string(room_id),
                                        std::string(thread_root),
                                        std::move(events));
          });
}

void EventHandlerBridge::on_thread_inserted(rust::Str room_id,
                                            rust::Str thread_root,
                                            std::uint64_t index,
                                            const TimelineEvent& ev) const
{
    with_handler("on_thread_inserted", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_thread_inserted(
                  std::string(room_id), std::string(thread_root),
                  static_cast<std::size_t>(index), tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_thread_updated(rust::Str room_id,
                                           rust::Str thread_root,
                                           std::uint64_t index,
                                           const TimelineEvent& ev) const
{
    with_handler("on_thread_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_thread_updated(
                  std::string(room_id), std::string(thread_root),
                  static_cast<std::size_t>(index), tesseract::make_event(ev));
          });
}

void EventHandlerBridge::on_thread_removed(rust::Str room_id,
                                           rust::Str thread_root,
                                           std::uint64_t index) const
{
    with_handler("on_thread_removed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              if (!index_fits(index))
              {
                  return;
              }
              handler_->on_thread_removed(std::string(room_id),
                                          std::string(thread_root),
                                          static_cast<std::size_t>(index));
          });
}

void EventHandlerBridge::on_messages_prepended(
    rust::Str room_id, const rust::Vec<TimelineEvent>& events) const
{
    with_handler("on_messages_prepended", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> evs;
              evs.reserve(events.size());
              for (const auto& ev : events)
              {
                  evs.push_back(tesseract::make_event(ev));
              }
              handler_->on_messages_prepended(std::string(room_id),
                                              std::move(evs));
          });
}

void EventHandlerBridge::on_messages_appended(
    rust::Str room_id, const rust::Vec<TimelineEvent>& events) const
{
    with_handler("on_messages_appended", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> evs;
              evs.reserve(events.size());
              for (const auto& ev : events)
              {
                  evs.push_back(tesseract::make_event(ev));
              }
              handler_->on_messages_appended(std::string(room_id),
                                             std::move(evs));
          });
}

void EventHandlerBridge::on_messages_updated_batch(
    rust::Str room_id,
    const rust::Vec<std::uint64_t>& indices,
    const rust::Vec<TimelineEvent>& events) const
{
    with_handler("on_messages_updated_batch", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              const std::size_t n = indices.size();
              if (n == 0 || n != events.size())
              {
                  return;
              }
              std::vector<std::size_t> idxs;
              idxs.reserve(n);
              for (std::uint64_t i : indices)
              {
                  if (!index_fits(i))
                  {
                      return;
                  }
                  idxs.push_back(static_cast<std::size_t>(i));
              }
              std::vector<std::unique_ptr<tesseract::Event>> evs;
              evs.reserve(n);
              for (const auto& ev : events)
              {
                  evs.push_back(tesseract::make_event(ev));
              }
              handler_->on_messages_updated_batch(std::string(room_id),
                                                  std::move(idxs),
                                                  std::move(evs));
          });
}

void EventHandlerBridge::on_thread_messages_prepended(
    rust::Str room_id, rust::Str thread_root,
    const rust::Vec<TimelineEvent>& events) const
{
    with_handler("on_thread_messages_prepended", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> evs;
              evs.reserve(events.size());
              for (const auto& ev : events)
              {
                  evs.push_back(tesseract::make_event(ev));
              }
              handler_->on_thread_messages_prepended(std::string(room_id),
                                                     std::string(thread_root),
                                                     std::move(evs));
          });
}

void EventHandlerBridge::on_thread_messages_appended(
    rust::Str room_id, rust::Str thread_root,
    const rust::Vec<TimelineEvent>& events) const
{
    with_handler("on_thread_messages_appended", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::unique_ptr<tesseract::Event>> evs;
              evs.reserve(events.size());
              for (const auto& ev : events)
              {
                  evs.push_back(tesseract::make_event(ev));
              }
              handler_->on_thread_messages_appended(std::string(room_id),
                                                    std::string(thread_root),
                                                    std::move(evs));
          });
}

void EventHandlerBridge::on_rooms_updated(
    const rust::Vec<RoomInfo>& rooms) const
{
    with_handler("on_rooms_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<tesseract::RoomInfo> cpp_rooms;
              cpp_rooms.reserve(rooms.size());
              for (const auto& r : rooms)
              {
                  cpp_rooms.push_back(tesseract::from_ffi(r));
              }
              handler_->on_rooms_updated(cpp_rooms);
          });
}

void EventHandlerBridge::on_invites_updated(
    const rust::Vec<InviteInfo>& invites) const
{
    with_handler("on_invites_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<tesseract::InviteInfo> cpp_invites;
              cpp_invites.reserve(invites.size());
              for (const auto& i : invites)
              {
                  cpp_invites.push_back(tesseract::from_ffi(i));
              }
              handler_->on_invites_updated(cpp_invites);
          });
}

void EventHandlerBridge::on_error(rust::Str context, rust::Str message,
                                  bool soft_logout) const
{
    with_handler("on_error", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_sync_error(std::string(context),
                                      std::string(message), soft_logout);
          });
}

void EventHandlerBridge::on_session_refreshed(rust::Str session_json) const
{
    with_handler("on_session_refreshed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_session_saved(std::string(session_json));
          });
}

void persist_session(rust::Str user_id, rust::Str session_json)
{
    // matrix-sdk invokes its save_session_callback synchronously on a runtime
    // worker/sync thread. The actual write (SessionStore::save_account) does a
    // credential-store write — which can be slow or even *prompt* — plus an
    // fsync'd file write, so running it inline can stall Matrix sync. Instead we
    // copy the (Rust-owned, call-scoped) buffers into owning std::strings and
    // hand them to a dedicated single-thread writer, returning promptly. The
    // writer coalesces per user_id (last-write-wins), so the newest token wins
    // and rapid refreshes can't grow the queue without bound.
    //
    // guard() keeps any exception from unwinding across the FFI boundary; the
    // enqueue itself shouldn't throw, but the singleton's first-use
    // construction (which spawns the thread) could, and that must not escape.
    guard("persist_session",
          [&]
          {
              tesseract::session_persist_queue().enqueue(
                  std::string(user_id), std::string(session_json));
          });
}

void EventHandlerBridge::on_backup_progress(
    const BackupProgress& progress) const
{
    with_handler("on_backup_progress", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_backup_progress(tesseract::from_ffi(progress));
          });
}

void EventHandlerBridge::on_enable_recovery_progress(std::uint8_t step,
                                                     rust::Str recovery_key,
                                                     std::uint32_t backed_up,
                                                     std::uint32_t total) const
{
    with_handler("on_enable_recovery_progress", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_enable_recovery_progress(
                  step, std::string(recovery_key), backed_up, total);
          });
}

void EventHandlerBridge::on_crypto_reset_result(bool ok, rust::Str message) const
{
    with_handler("on_crypto_reset_result", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_crypto_reset_result(ok, std::string(message));
          });
}

void EventHandlerBridge::on_room_list_state(std::uint8_t state) const
{
    with_handler("on_room_list_state", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              // Clamp unknown codes back to Init so the C++ side never sees an
              // out-of-range enum value if the Rust protocol ever adds a state we
              // don't know yet. The exhaustive Rust mapper makes this a defensive
              // belt-and-braces — there's no path that produces an invalid code today.
              tesseract::RoomListState rls =
                  (state <= static_cast<std::uint8_t>(
                                tesseract::RoomListState::Terminated))
                      ? static_cast<tesseract::RoomListState>(state)
                      : tesseract::RoomListState::Init;
              handler_->on_room_list_state(rls);
          });
}

void EventHandlerBridge::on_inflight_changed(std::uint32_t count) const
{
    with_handler("on_inflight_changed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_inflight_changed(count);
          });
}

void EventHandlerBridge::on_inflight_changed_debug(std::uint32_t count,
                                                    rust::Str urls) const
{
#ifndef NDEBUG
    with_handler("on_inflight_changed_debug", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_inflight_changed_debug(count, std::string(urls));
          });
#else
    (void)count; (void)urls;
#endif
}

void EventHandlerBridge::on_image_packs_updated() const
{
    with_handler("on_image_packs_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_image_packs_updated();
          });
}

void EventHandlerBridge::on_threads_updated(rust::Str room_id) const
{
    with_handler("on_threads_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_threads_updated(std::string(room_id));
          });
}

void EventHandlerBridge::on_media_ready(std::uint64_t request_id,
                                        rust::Slice<const uint8_t> bytes) const
{
    with_handler("on_media_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<uint8_t> b(bytes.data(), bytes.data() + bytes.size());
              handler_->on_media_ready(request_id, b);
          });
}

void EventHandlerBridge::on_url_preview_ready(std::uint64_t request_id,
                                              rust::Str preview_json) const
{
    with_handler("on_url_preview_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_url_preview_ready(request_id,
                                             std::string(preview_json));
          });
}

void EventHandlerBridge::on_gif_results(std::uint64_t request_id,
                                        const rust::Vec<GifResult>& results) const
{
    with_handler("on_gif_results", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<tesseract::GifResult> cpp_results;
              cpp_results.reserve(results.size());
              for (const auto& r : results)
              {
                  tesseract::GifResult g;
                  g.id = std::string(r.id);
                  g.preview_url = std::string(r.preview_url);
                  g.preview_w = r.preview_w;
                  g.preview_h = r.preview_h;
                  g.image_url = std::string(r.image_url);
                  g.image_w = r.image_w;
                  g.image_h = r.image_h;
                  g.image_mime = std::string(r.image_mime);
                  g.strip_url = std::string(r.strip_url);
                  g.strip_mime = std::string(r.strip_mime);
                  cpp_results.push_back(std::move(g));
              }
              handler_->on_gif_results(request_id, cpp_results);
          });
}

void EventHandlerBridge::on_gif_search_failed(std::uint64_t request_id,
                                              rust::Str message) const
{
    with_handler("on_gif_search_failed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_gif_search_failed(request_id, std::string(message));
          });
}

void EventHandlerBridge::on_forward_done(std::uint64_t request_id) const
{
    with_handler("on_forward_done", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_forward_done(request_id);
          });
}

void EventHandlerBridge::on_forward_failed(std::uint64_t request_id,
                                           rust::Str message) const
{
    with_handler("on_forward_failed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_forward_failed(request_id, std::string(message));
          });
}

void EventHandlerBridge::on_space_child_summary_ready(std::uint64_t request_id,
                                                      rust::Str summary_json) const
{
    with_handler("on_space_child_summary_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_space_child_summary_ready(request_id,
                                                     std::string(summary_json));
          });
}

void EventHandlerBridge::on_server_info_ready(std::uint64_t request_id,
                                              rust::Str info_json) const
{
    with_handler("on_server_info_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_server_info_ready(request_id, std::string(info_json));
          });
}

void EventHandlerBridge::on_media_preview_config_ready(std::uint64_t request_id,
                                                       rust::Str config_json) const
{
    with_handler("on_media_preview_config_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_media_preview_config_ready(request_id,
                                                      std::string(config_json));
          });
}

void EventHandlerBridge::on_room_preview_override_ready(std::uint64_t request_id,
                                                        rust::Str override_json) const
{
    with_handler("on_room_preview_override_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_room_preview_override_ready(request_id,
                                                       std::string(override_json));
          });
}

void EventHandlerBridge::on_search_results(std::uint64_t request_id,
                                           const rust::Vec<SearchHit>& results) const
{
    with_handler("on_search_results", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<tesseract::SearchHit> cpp_results;
              cpp_results.reserve(results.size());
              for (const auto& r : results)
              {
                  tesseract::SearchHit h;
                  h.event_id = std::string(r.event_id);
                  h.room_id = std::string(r.room_id);
                  h.room_name = std::string(r.room_name);
                  h.sender = std::string(r.sender);
                  h.sender_name = std::string(r.sender_name);
                  h.body = std::string(r.body);
                  h.timestamp_ms = r.timestamp_ms;
                  cpp_results.push_back(std::move(h));
              }
              handler_->on_search_results(request_id, cpp_results);
          });
}

void EventHandlerBridge::on_search_failed(std::uint64_t request_id,
                                          rust::Str message) const
{
    with_handler("on_search_failed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_search_failed(request_id, std::string(message));
          });
}

void EventHandlerBridge::on_paginate_result(std::uint64_t request_id,
                                            bool ok, bool reached_start,
                                            bool reached_end,
                                            rust::Str message) const
{
    with_handler("on_paginate_result", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_paginate_result(request_id, ok, reached_start,
                                           reached_end, std::string(message));
          });
}

void EventHandlerBridge::on_room_action_complete(std::uint64_t request_id,
                                                  bool ok,
                                                  rust::Str joined_room_id,
                                                  rust::Str message) const
{
    with_handler("on_room_action_complete", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_room_action_complete(request_id, ok,
                                                std::string(joined_room_id),
                                                std::string(message));
          });
}

void EventHandlerBridge::on_upload_complete(std::uint64_t request_id, bool ok,
                                             rust::Str message) const
{
    with_handler("on_upload_complete", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_upload_complete(request_id, ok, std::string(message));
          });
}

void EventHandlerBridge::on_profile_field_result(std::uint64_t request_id,
                                                  rust::Str key, bool ok,
                                                  rust::Str message) const
{
    with_handler("on_profile_field_result", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_profile_field_result(request_id, std::string(key),
                                                ok, std::string(message));
          });
}

void EventHandlerBridge::on_extended_profile_ready(std::uint64_t request_id,
                                                    rust::Str profile_json) const
{
    with_handler("on_extended_profile_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_extended_profile_ready(request_id,
                                                   std::string(profile_json));
          });
}

void EventHandlerBridge::on_account_prefs_updated(rust::Str json) const
{
    with_handler("on_account_prefs_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_account_prefs_updated(std::string(json));
          });
}

void EventHandlerBridge::on_media_preview_config_updated(rust::Str json) const
{
    with_handler("on_media_preview_config_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_media_preview_config_updated(std::string(json));
          });
}

void EventHandlerBridge::on_notification(
    rust::Str room_id, rust::Str room_name, rust::Str sender, rust::Str body,
    bool is_mention, rust::Slice<const uint8_t> avatar_bytes,
    rust::Slice<const uint8_t> image_bytes) const
{
    with_handler("on_notification", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<uint8_t> av(avatar_bytes.data(),
                                      avatar_bytes.data() +
                                          avatar_bytes.size());
              std::vector<uint8_t> img(image_bytes.data(),
                                       image_bytes.data() + image_bytes.size());
              handler_->on_notification(
                  std::string(room_id), std::string(room_name),
                  std::string(sender), std::string(body), is_mention, av, img);
          });
}

void EventHandlerBridge::on_verification_request(rust::Str flow_id,
                                                 rust::Str user_id,
                                                 rust::Str device_id,
                                                 bool incoming) const
{
    with_handler("on_verification_request", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_verification_request(
                  std::string(flow_id), std::string(user_id),
                  std::string(device_id), incoming);
          });
}

void EventHandlerBridge::on_sas_ready(
    rust::Str flow_id, const rust::Vec<VerificationEmoji>& emojis) const
{
    with_handler("on_sas_ready", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<tesseract::VerificationEmoji> cpp_emojis;
              cpp_emojis.reserve(emojis.size());
              for (const auto& e : emojis)
              {
                  cpp_emojis.push_back(
                      {std::string(e.symbol), std::string(e.description)});
              }
              handler_->on_sas_ready(std::string(flow_id),
                                     std::move(cpp_emojis));
          });
}

void EventHandlerBridge::on_verification_done(rust::Str flow_id) const
{
    with_handler("on_verification_done", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_verification_done(std::string(flow_id));
          });
}

void EventHandlerBridge::on_verification_cancelled(rust::Str flow_id,
                                                   rust::Str reason) const
{
    with_handler("on_verification_cancelled", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_verification_cancelled(std::string(flow_id),
                                                  std::string(reason));
          });
}

void EventHandlerBridge::on_verification_state_changed(bool verified) const
{
    with_handler("on_verification_state_changed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_verification_state_changed(verified);
          });
}

void EventHandlerBridge::on_typing_changed(
    rust::Str room_id, const rust::Vec<rust::String>& user_ids) const
{
    with_handler("on_typing_changed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              std::vector<std::string> ids;
              ids.reserve(user_ids.size());
              for (const auto& uid : user_ids)
              {
                  ids.push_back(std::string(uid));
              }
              handler_->on_typing_changed(std::string(room_id), std::move(ids));
          });
}

void EventHandlerBridge::on_presence_changed(rust::Str user_id,
                                             std::uint8_t state) const
{
    with_handler("on_presence_changed", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              tesseract::PresenceState ps;
              switch (state)
              {
              case 1:  ps = tesseract::PresenceState::Online;      break;
              case 2:  ps = tesseract::PresenceState::Unavailable; break;
              default: ps = tesseract::PresenceState::Offline;     break;
              }
              handler_->on_presence_changed(std::string(user_id), ps);
          });
}

// ---------------------------------------------------------------------------
// MatrixRTC callbacks
// ---------------------------------------------------------------------------

void EventHandlerBridge::on_rtc_invitation(rust::Str room_id,
                                           rust::Str slot_id,
                                           rust::Str caller_user_id,
                                           rust::Str call_intent,
                                           std::uint64_t lifetime_ms,
                                           rust::Str notification_event_id) const
{
    with_handler("on_rtc_invitation", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_invitation(std::string(room_id),
                                           std::string(slot_id),
                                           std::string(caller_user_id),
                                           std::string(call_intent),
                                           lifetime_ms,
                                           std::string(notification_event_id));
          });
}

void EventHandlerBridge::on_rtc_participant_joined(
    std::uint64_t session_id, const RtcParticipantInfo& info) const
{
    with_handler("on_rtc_participant_joined", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_participant_joined(session_id,
                                                   tesseract::from_ffi(info));
          });
}

void EventHandlerBridge::on_rtc_participant_left(std::uint64_t session_id,
                                                 rust::Str participant_id) const
{
    with_handler("on_rtc_participant_left", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_participant_left(session_id,
                                                  std::string(participant_id));
          });
}

void EventHandlerBridge::on_rtc_participant_updated(
    std::uint64_t session_id, const RtcParticipantInfo& info) const
{
    with_handler("on_rtc_participant_updated", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_participant_updated(session_id,
                                                    tesseract::from_ffi(info));
          });
}

void EventHandlerBridge::on_rtc_session_ended(std::uint64_t session_id,
                                              rust::Str reason) const
{
    with_handler("on_rtc_session_ended", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_ended(session_id, std::string(reason));
          });
}

void EventHandlerBridge::on_rtc_video_frame(std::uint64_t session_id,
                                            rust::Str participant_id,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            rust::Slice<const uint8_t> rgba) const
{
    with_handler("on_rtc_video_frame", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_video_frame(session_id,
                                             std::string(participant_id),
                                             width, height,
                                             rgba.data(), rgba.size());
          });
}

void EventHandlerBridge::on_rtc_screen_frame(std::uint64_t session_id,
                                             rust::Str participant_id,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             rust::Slice<const uint8_t> rgba) const
{
    with_handler("on_rtc_screen_frame", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_screen_frame(session_id,
                                              std::string(participant_id),
                                              width, height,
                                              rgba.data(), rgba.size());
          });
}

void EventHandlerBridge::on_rtc_audio_frame(std::uint64_t session_id,
                                            rust::Str participant_id,
                                            rust::Slice<const int16_t> samples,
                                            std::uint32_t sample_rate,
                                            std::uint32_t num_channels) const
{
    with_handler("on_rtc_audio_frame", slot_,
          [&](tesseract::IEventHandler* handler_)
          {
              handler_->on_call_audio_frame(session_id,
                                             std::string(participant_id),
                                             samples.data(), samples.size(),
                                             sample_rate, num_channels);
          });
}

} // namespace tesseract_ffi
