#pragma once

#include "MediaTransportBar.h"
#include "MediaViewerPage.h"

#include "tk/audio.h"
#include "tk/canvas.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

// Audio page of MediaViewerOverlay: a centred card (music glyph, title, clock,
// waveform) with the shared play / scrub / speed transport bar. Non-widget —
// the overlay owns the scrim, chrome and pointer routing and forwards here.
//
// Bytes arrive through load_bytes() (token-checked by the overlay); until then
// a spinner shows. Without a player (the platform has no audio backend) the
// card shows an explanatory message and the chrome Save button still works.
class AudioViewerPage : public MediaViewerPage
{
public:
    explicit AudioViewerPage(MediaViewerPageHost& host);

    void activate(const MediaViewerItem& item) override;
    // Stops playback and drops per-item state.
    void deactivate() override;
    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override;
    void paint_content(tk::PaintCtx&) override;
    void paint_controls(tk::PaintCtx&) override;
    bool on_content_pointer_down(tk::Point world, tk::Point local) override;
    bool on_content_pointer_up(tk::Point world, tk::Point local, bool inside_self) override;
    void on_pointer_drag(tk::Point local) override;
    // Space / K play-pause, Home restarts. Left/Right/PageUp/PageDown are never
    // consumed, so they navigate the sequence.
    bool on_key(const tk::KeyEvent& e) override;
    bool controls_hovered() const override;

    void set_audio_player(std::unique_ptr<tk::AudioPlayer> player);
    bool has_player() const
    {
        return player_ != nullptr;
    }
    tk::AudioPlayer* player() const
    {
        return player_.get();
    }

    // Called on the UI thread once the byte fetch completes; empty = failed.
    // Starts playback immediately.
    void load_bytes(const std::uint8_t* data, std::size_t size);

    bool is_loading() const
    {
        return is_loading_;
    }
    bool has_error() const
    {
        return has_error_;
    }
    // Bytes of the clip currently handed to the player (an upper bound on what
    // the backend retains).
    std::size_t memory_bytes() const
    {
        return loaded_bytes_;
    }

    tk::Button* play_btn() const
    {
        return bar_.play_btn();
    }
    tk::Button* speed_btn() const
    {
        return bar_.speed_btn();
    }
    tk::Rect card_rect() const
    {
        return card_;
    }

private:
    void toggle_();
    void cycle_speed_();
    void seek_to_fraction_(float frac);
    void on_player_progress_();
    void on_player_error_();
    std::uint64_t duration_() const;
    MediaTransportBar::State transport_state_() const;
    bool controls_available_() const;
    std::string title_() const;

    MediaViewerPageHost& host_;
    MediaTransportBar bar_;
    std::unique_ptr<tk::AudioPlayer> player_;

    bool active_ = false;
    bool is_loading_ = false;
    bool has_error_ = false;
    std::chrono::steady_clock::time_point loading_start_{};
    std::string filename_;
    std::string caption_;
    std::string mime_type_;
    std::uint64_t duration_ms_ = 0;
    std::vector<std::uint16_t> waveform_;
    float rate_ = 1.0f;
    std::size_t loaded_bytes_ = 0;

    tk::Rect card_{};
    tk::Rect wave_{};
    tk::Rect bar_rect_{};
    tk::IconCache glyph_;
};

} // namespace tesseract::views
