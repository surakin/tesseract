#pragma once
#include <tesseract/types.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tesseract
{

class Client;
struct Result;

/// Owns the state of one active MatrixRTC call: participant list and the
/// FFI calls that control it. All public methods must be called on the UI thread
/// except participants() which takes a snapshot under its mutex.
///
/// Lifetime: created by ShellBase::start_call(), destroyed by
/// ShellBase::end_call() or ShellBase::handle_rtc_session_ended_ui_().
class CallSession
{
public:
    /// `session_id` is the id `Client::rtc_start_call` returned: every event of
    /// this call carries it, and an event with another id (or none) is not ours.
    CallSession(Client* client, std::string room_id, std::string slot_id,
                std::uint64_t session_id);
    ~CallSession();

    /// Mute/unmute local audio. No-op when the FFI session is gone.
    void mute_audio(bool muted);
    /// Mute/unmute local video. No-op when the FFI session is gone.
    void mute_video(bool muted);
    /// Gracefully leave and release resources. Returns at once; the leave
    /// finishes in the background (rtc_end_call on the client).
    void hang_up();
    /// As hang_up(), but returns once the leave has gone out. Blocks for up to a
    /// few seconds: for sign-out and quit, where the client goes away right after.
    void hang_up_and_wait();
    /// Start publishing a screen share track. Returns the publish result so
    /// the caller can avoid arming a capture whose frames would otherwise be
    /// silently dropped (push_screen_frame_i420 is a no-op until this
    /// succeeds).
    Result start_screen_share();
    /// Stop the screen share track.
    void stop_screen_share();

    const std::string& room_id() const { return room_id_; }
    const std::string& slot_id() const { return slot_id_; }
    /// The id the SDK gave this call when it was started.
    std::uint64_t session_id() const { return session_id_; }
    /// Whether an SDK event carrying `id` belongs to this call.
    bool owns_event(std::uint64_t id) const { return id != 0 && id == session_id_; }

    /// Thread-safe snapshot of the current participant list.
    std::vector<RtcParticipantInfo> participants() const;

    // Called by EventHandlerBase routing on the UI thread —————————————————
    void on_participant_joined(const RtcParticipantInfo& p);
    void on_participant_left(const std::string& participant_id);
    void on_participant_updated(const RtcParticipantInfo& p);
    void on_session_ended(const std::string& reason);

private:
    Client* client_;   // non-owning
    std::string room_id_;
    std::string slot_id_;
    std::uint64_t session_id_;
    bool ended_ = false;

    mutable std::mutex mu_;
    std::vector<RtcParticipantInfo> participants_;
};

} // namespace tesseract
