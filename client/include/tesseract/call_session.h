#pragma once
#ifdef TESSERACT_CALLS_ENABLED
#include <tesseract/types.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tesseract
{

class Client;

/// Owns the state of one active MatrixRTC call: participant list and the
/// FFI calls that control it. All public methods must be called on the UI thread
/// except participants() which takes a snapshot under its mutex.
///
/// Lifetime: created by ShellBase::start_call(), destroyed by
/// ShellBase::end_call() or ShellBase::handle_rtc_session_ended_ui_().
class CallSession
{
public:
    CallSession(Client* client, std::string room_id, std::string slot_id);
    ~CallSession();

    /// Mute/unmute local audio. No-op when the FFI session is gone.
    void mute_audio(bool muted);
    /// Mute/unmute local video. No-op when the FFI session is gone.
    void mute_video(bool muted);
    /// Gracefully leave and release resources (calls rtc_end_call on the client).
    void hang_up();
    /// Start publishing a screen share track.
    void start_screen_share();
    /// Stop the screen share track.
    void stop_screen_share();

    const std::string& room_id() const { return room_id_; }
    const std::string& slot_id() const { return slot_id_; }
    /// 0 until the first participant callback confirms the session id.
    std::uint64_t session_id() const { return session_id_; }

    /// Thread-safe snapshot of the current participant list.
    std::vector<RtcParticipantInfo> participants() const;

    // Called by EventHandlerBase routing on the UI thread —————————————————
    void set_session_id(std::uint64_t id);
    void on_participant_joined(const RtcParticipantInfo& p);
    void on_participant_left(const std::string& participant_id);
    void on_participant_updated(const RtcParticipantInfo& p);
    void on_session_ended(const std::string& reason);

private:
    Client* client_;   // non-owning
    std::string room_id_;
    std::string slot_id_;
    std::uint64_t session_id_ = 0;
    bool ended_ = false;

    mutable std::mutex mu_;
    std::vector<RtcParticipantInfo> participants_;
};

} // namespace tesseract
#endif // TESSERACT_CALLS_ENABLED
