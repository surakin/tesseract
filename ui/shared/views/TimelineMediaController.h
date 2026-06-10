#pragma once

// TimelineMediaController — voice + audio message playback subsystem extracted
// from MessageListView. Owns the single tk::AudioPlayer (at most one clip plays
// at a time), the byte-cache provider, the play/pause/scrub/speed playback
// state, and the per-paint hit-test geometry for the voice and audio cards.
//
// MessageListView holds one of these by value and forwards its public
// set_audio_player / set_voice_bytes_provider wiring here, routes the
// pointer-down/up voice+audio dispatch to the handle_* methods, and lets the
// Adapter paint the cards through paint_voice_card / paint_audio_card. The
// controller is repaint-driven: it calls the injected repaint callback whenever
// playback state changes, exactly as the original code did.

#include "tk/audio.h"
#include "tk/canvas.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tk
{
struct PaintCtx;
class IconCache;
} // namespace tk

namespace tesseract::views
{

struct MessageRowData;

// Same shape as MessageListView's historical nested alias: given the opaque
// audio source token (JSON), return the decoded clip bytes (empty on cache
// miss, which kicks off a background fetch in the provider).
using VoiceBytesProvider =
    std::function<std::vector<std::uint8_t>(const std::string& source_json)>;

class TimelineMediaController
{
public:
    // Hit-test geometry, rebuilt every paint pass (world coords, keyed by
    // event_id) so the pointer handlers can locate the play button, waveform
    // strip / progress track, and speed pill without touching the painter.
    struct VoiceCardGeom
    {
        std::string event_id;
        tk::Rect    play_button{};    // play/pause hit rect
        tk::Rect    waveform_strip{}; // scrub hit rect
        tk::Rect    speed_pill{};     // playback-rate cycle hit rect
        tk::Rect    card_bounds{};
    };

    struct AudioCardGeom
    {
        std::string event_id;
        tk::Rect    play_button{};    // play/pause hit rect
        tk::Rect    progress_track{}; // linear scrub hit rect
        tk::Rect    card_bounds{};
    };

    // --- wiring (forwarded from MessageListView's public API) ---
    void set_player(std::unique_ptr<tk::AudioPlayer> player);
    void set_bytes_provider(VoiceBytesProvider provider);
    void set_repaint(std::function<void()> request_repaint);

    bool has_player() const { return static_cast<bool>(audio_player_); }
    bool has_bytes_provider() const
    {
        return static_cast<bool>(voice_bytes_provider_);
    }

    // --- handlers (dispatched from on_pointer_down / on_pointer_up / drag) ---
    void handle_voice_play_click(const MessageRowData& row);
    void handle_voice_scrub_at(const MessageRowData& row, float world_x);
    void handle_voice_speed_click();
    void handle_audio_play_click(const MessageRowData& row);
    void handle_audio_scrub_at(const MessageRowData& row, float world_x);
    // Wired to AudioPlayer::on_progress; snapshots position / playing state.
    void on_audio_progress();

    // A play click on a not-yet-cached clip arms a pending play instead of
    // failing silently; retry_pending_voice_play() is called from
    // MessageListView::arrange() on the relayout that the arriving bytes
    // trigger, and starts playback once the warm cache holds the bytes.
    void retry_pending_voice_play();
    // Clears any armed pending play (room switch / timeline reset).
    void reset_pending_play();

    // --- geometry: written by paint, read by the pointer hit-test ---
    void clear_geometry()
    {
        voice_card_geom_.clear();
        audio_card_geom_.clear();
    }
    const std::unordered_map<std::string, VoiceCardGeom>& voice_geom() const
    {
        return voice_card_geom_;
    }
    const std::unordered_map<std::string, AudioCardGeom>& audio_geom() const
    {
        return audio_card_geom_;
    }
    const VoiceCardGeom* voice_geom_at(const std::string& event_id) const
    {
        auto it = voice_card_geom_.find(event_id);
        return it == voice_card_geom_.end() ? nullptr : &it->second;
    }
    const AudioCardGeom* audio_geom_at(const std::string& event_id) const
    {
        auto it = audio_card_geom_.find(event_id);
        return it == audio_card_geom_.end() ? nullptr : &it->second;
    }

    // --- card paint (Adapter delegates here; icon cache stays Adapter-local) ---
    void paint_voice_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst, tk::IconCache& play_icon);
    void paint_audio_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst, tk::IconCache& play_icon);

    // --- accessors used by the hit-test / debugging ---
    const std::string& playing_event_id() const { return playing_event_id_; }

private:
    std::unique_ptr<tk::AudioPlayer> audio_player_;
    VoiceBytesProvider               voice_bytes_provider_;
    std::function<void()>            request_repaint_;

    // The event_id of the currently-loaded clip in `audio_player_`. Empty
    // when nothing is loaded.
    std::string playing_event_id_;
    // Mirror of `audio_player_->position_ms()` — refreshed from the
    // AudioPlayer's on_progress callback so paint doesn't have to call
    // back into the player.
    std::uint64_t playing_position_ms_ = 0;
    bool          playing_is_active_   = false;
    // Set to true once on_audio_progress() first observes is_playing()==true
    // for the current clip. Guards against clearing playing_event_id_ during
    // the async-load window when Qt reports is_playing()==false at position 0.
    bool playing_ever_active_ = false;
    // Global playback rate. Cycles 1.0 -> 1.5 -> 2.0 -> 1.0 via the per-row
    // speed pill. Applied to every play()/resume()/seek().
    float playback_rate_ = 1.0f;

    // Armed when a play click hits a cold cache (bytes still fetching). The
    // next relayout (the fetch's on_ready repaints/relayouts) re-runs
    // retry_pending_voice_play(), which re-pulls the now-warm bytes and
    // starts playback — so a single click suffices.
    std::string pending_play_event_id_;
    std::string pending_play_token_;
    std::string pending_play_mime_;
    bool        pending_play_is_voice_ = false; // voice → playback_rate_; audio → 1.0f

    mutable std::unordered_map<std::string, VoiceCardGeom> voice_card_geom_;
    mutable std::unordered_map<std::string, AudioCardGeom> audio_card_geom_;
};

} // namespace tesseract::views
