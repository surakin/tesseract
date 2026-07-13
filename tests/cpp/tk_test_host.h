#pragma once

// Minimal concrete tk::Host for unit tests. Stubs every platform virtual,
// points input_root_ at a supplied root, and re-exposes the protected
// dispatch_pointer_* entry points + tooltip/popup state so tests can drive
// and assert against Host's shared logic directly, without a real platform
// backend. Shared by test_tk_host_pointer.cpp, test_tk_about_section.cpp,
// and test_tk_tooltip.cpp.

#include "tk/audio.h"
#include "tk/audio_capture.h"
#include "tk/host.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class TestHost : public tk::Host
{
public:
    explicit TestHost(tk::Widget* root) : root_(root) {}

    void request_repaint() override { ++repaint_count; }
    void post_to_ui(std::function<void()>) override {}

    // Captures delayed callbacks instead of firing them, so tests can
    // control exactly when a tooltip's show-delay elapses.
    void post_delayed(int ms, std::function<void()> fn) override
    {
        pending_delays_.push_back({ms, std::move(fn)});
    }
    // Runs every captured delayed callback that hasn't fired yet (in the
    // order they were scheduled), then clears the queue. A callback that
    // itself calls post_delayed() is not re-run within the same call.
    void fire_all_delays()
    {
        auto delays = std::move(pending_delays_);
        pending_delays_.clear();
        for (auto& d : delays)
            d.fn();
    }

    std::unique_ptr<tk::NativeTextField> make_text_field() override
    {
        return nullptr;
    }
    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        return nullptr;
    }
    std::unique_ptr<tk::AudioPlayer> make_audio_player() override
    {
        return nullptr;
    }
    std::unique_ptr<tk::AudioCapture> make_audio_capture() override
    {
        return nullptr;
    }
    std::unique_ptr<tk::VideoPlayer> make_video_player() override
    {
        return nullptr;
    }
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<tk::AudioPlayback> make_audio_playback() override
    {
        return nullptr;
    }
#endif
    tk::EncodedImage encode_for_send(const std::uint8_t*, std::size_t,
                                     bool) override
    {
        return {};
    }
    void set_clipboard_text(std::string_view) override {}
    bool set_clipboard_image(std::span<const std::uint8_t>) override
    {
        return false;
    }

    // Re-expose the protected shared dispatch + tracked state for tests.
    using tk::Host::dispatch_drag_hover;
    using tk::Host::dispatch_drag_leave;
    using tk::Host::dispatch_file_drop;
    using tk::Host::dispatch_pointer_down;
    using tk::Host::dispatch_pointer_leave;
    using tk::Host::dispatch_pointer_move;
    using tk::Host::dispatch_pointer_up;
    using tk::Host::drag_hovered_widget_;
    using tk::Host::hovered_widget_;
    using tk::Host::pressed_widget_;
    using tk::Host::paint_tooltip_overlay;
    using tk::Host::tooltip_owner_;
    using tk::Host::tooltip_text_;
    using tk::Host::tooltip_anchor_;
    using tk::Host::tooltip_visible_;
    using tk::Host::kTooltipShowDelayMs;

    // Drive the popup the way a paint pass would: register then promote.
    void set_active_popup(tk::Widget* w)
    {
        register_popup(w);
        popup_ = pending_popup_;
    }
    void clear_active_popup() { popup_ = nullptr; }

    int repaint_count = 0;

    struct Delayed
    {
        int ms;
        std::function<void()> fn;
    };
    std::vector<Delayed> pending_delays_;

protected:
    tk::Widget* input_root_() const override { return root_; }

private:
    tk::Widget* root_;
};
