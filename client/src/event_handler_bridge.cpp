#include "tesseract/event_handler_bridge.h"
#include "ffi_convert.h"
#include "tesseract/session_store.h"

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
} // namespace

void EventHandlerBridge::on_timeline_reset(
    rust::Str room_id, const rust::Vec<TimelineEvent>& snapshot) const
{
    guard("on_timeline_reset",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_message_inserted",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
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
    guard("on_message_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
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
    guard("on_message_removed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
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
    guard("on_thread_reset",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_thread_inserted",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
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
    guard("on_thread_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
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
    guard("on_thread_removed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_ || !index_fits(index))
              {
                  return;
              }
              handler_->on_thread_removed(std::string(room_id),
                                          std::string(thread_root),
                                          static_cast<std::size_t>(index));
          });
}

void EventHandlerBridge::on_rooms_updated(
    const rust::Vec<RoomInfo>& rooms) const
{
    guard("on_rooms_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_invites_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_error",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_sync_error(std::string(context),
                                      std::string(message), soft_logout);
          });
}

void EventHandlerBridge::on_session_refreshed(rust::Str session_json) const
{
    guard("on_session_refreshed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_session_saved(std::string(session_json));
          });
}

void persist_session(rust::Str user_id, rust::Str session_json)
{
    // Routes matrix-sdk's synchronous save_session_callback to the same
    // authoritative store load_account() reads on the next launch. Best-effort
    // and idempotent with the async on_session_refreshed path: whichever runs
    // last simply rewrites the same blob. guard() keeps any exception from
    // unwinding across the FFI boundary.
    guard("persist_session",
          [&]
          {
              tesseract::SessionStore::save_account(std::string(user_id),
                                                    std::string(session_json));
          });
}

void EventHandlerBridge::on_backup_progress(
    const BackupProgress& progress) const
{
    guard("on_backup_progress",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_backup_progress(tesseract::from_ffi(progress));
          });
}

void EventHandlerBridge::on_enable_recovery_progress(std::uint8_t step,
                                                     rust::Str recovery_key,
                                                     std::uint32_t backed_up,
                                                     std::uint32_t total) const
{
    guard("on_enable_recovery_progress",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_enable_recovery_progress(
                  step, std::string(recovery_key), backed_up, total);
          });
}

void EventHandlerBridge::on_room_list_state(std::uint8_t state) const
{
    guard("on_room_list_state",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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

void EventHandlerBridge::on_image_packs_updated() const
{
    guard("on_image_packs_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_image_packs_updated();
          });
}

void EventHandlerBridge::on_threads_updated(rust::Str room_id) const
{
    guard("on_threads_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_threads_updated(std::string(room_id));
          });
}

void EventHandlerBridge::on_account_prefs_updated(rust::Str json) const
{
    guard("on_account_prefs_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_account_prefs_updated(std::string(json));
          });
}

void EventHandlerBridge::on_media_preview_config_updated(rust::Str json) const
{
    guard("on_media_preview_config_updated",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_media_preview_config_updated(std::string(json));
          });
}

void EventHandlerBridge::on_notification(
    rust::Str room_id, rust::Str room_name, rust::Str sender, rust::Str body,
    bool is_mention, rust::Slice<const uint8_t> avatar_bytes,
    rust::Slice<const uint8_t> image_bytes) const
{
    guard("on_notification",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_verification_request",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_verification_request(
                  std::string(flow_id), std::string(user_id),
                  std::string(device_id), incoming);
          });
}

void EventHandlerBridge::on_sas_ready(
    rust::Str flow_id, const rust::Vec<VerificationEmoji>& emojis) const
{
    guard("on_sas_ready",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_verification_done",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_verification_done(std::string(flow_id));
          });
}

void EventHandlerBridge::on_verification_cancelled(rust::Str flow_id,
                                                   rust::Str reason) const
{
    guard("on_verification_cancelled",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_verification_cancelled(std::string(flow_id),
                                                  std::string(reason));
          });
}

void EventHandlerBridge::on_verification_state_changed(bool verified) const
{
    guard("on_verification_state_changed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
              handler_->on_verification_state_changed(verified);
          });
}

void EventHandlerBridge::on_typing_changed(
    rust::Str room_id, const rust::Vec<rust::String>& user_ids) const
{
    guard("on_typing_changed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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
    guard("on_presence_changed",
          [&]
          {
              auto* handler_ = slot_->load();
              if (!handler_)
              {
                  return;
              }
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

} // namespace tesseract_ffi
