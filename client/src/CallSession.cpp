#include <tesseract/call_session.h>
#include <tesseract/client.h>
#include <algorithm>

namespace tesseract
{

CallSession::CallSession(Client* client, std::string room_id, std::string slot_id)
    : client_(client), room_id_(std::move(room_id)), slot_id_(std::move(slot_id))
{
}

CallSession::~CallSession()
{
    if (!ended_)
        hang_up();
}

void CallSession::mute_audio(bool muted)
{
    if (!ended_)
        client_->rtc_set_audio_muted(muted);
}

void CallSession::mute_video(bool muted)
{
    if (!ended_)
        client_->rtc_set_video_muted(muted);
}

void CallSession::hang_up()
{
    if (ended_)
        return;
    ended_ = true;
    client_->rtc_end_call();
}

Result CallSession::start_screen_share()
{
    if (ended_)
        return Result{false, "call has ended"};
    return client_->rtc_start_screen_share();
}

void CallSession::stop_screen_share()
{
    if (!ended_)
        client_->rtc_stop_screen_share();
}

std::vector<RtcParticipantInfo> CallSession::participants() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return participants_;
}

void CallSession::set_session_id(std::uint64_t id)
{
    if (session_id_ == 0)
        session_id_ = id;
}

void CallSession::on_participant_joined(const RtcParticipantInfo& p)
{
    std::lock_guard<std::mutex> lk(mu_);
    participants_.push_back(p);
}

void CallSession::on_participant_left(const std::string& participant_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(participants_.begin(), participants_.end(),
        [&](const RtcParticipantInfo& p) {
            return p.participant_id == participant_id;
        });
    if (it != participants_.end())
        participants_.erase(it);
}

void CallSession::on_participant_updated(const RtcParticipantInfo& p)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& existing : participants_)
    {
        if (existing.participant_id == p.participant_id)
        {
            existing = p;
            return;
        }
    }
    // Participant not yet tracked — add it.
    participants_.push_back(p);
}

void CallSession::on_session_ended(const std::string& /*reason*/)
{
    ended_ = true;
    std::lock_guard<std::mutex> lk(mu_);
    participants_.clear();
}

} // namespace tesseract
