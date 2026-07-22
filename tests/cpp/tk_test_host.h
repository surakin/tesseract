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

    // Every real backend's request_relayout() also repaints (see
    // host_qt.cpp/host_gtk.cpp/host_win32.cpp/host_macos.mm), so bump
    // repaint_count too — keeps existing repaint_count-only assertions valid
    // for call sites that switched from request_repaint() to this.
    void request_relayout() override
    {
        ++relayout_count;
        ++repaint_count;
    }

    // Captures posted UI-thread tasks instead of running them immediately,
    // so tests can control exactly when a deferred task (e.g.
    // Host::queue_for_deletion()'s drain) actually runs. Mirrors
    // post_delayed()/fire_all_delays() below.
    void post_to_ui(std::function<void()> task) override
    {
        pending_ui_tasks_.push_back(std::move(task));
    }
    // Runs every captured post_to_ui() task (in the order posted), then
    // clears the queue. A task that itself calls post_to_ui() is not
    // re-run within the same call.
    void fire_all_ui_tasks()
    {
        auto tasks = std::move(pending_ui_tasks_);
        pending_ui_tasks_.clear();
        for (auto& t : tasks)
            t();
    }

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
    std::unique_ptr<tk::PopupSurfaceHandle> make_popup_surface() override
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
    std::unique_ptr<tk::AudioPlayback> make_audio_playback() override
    {
        return nullptr;
    }
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
    using tk::Host::dispatch_key_down;
    using tk::Host::dispatch_pointer_down;
    using tk::Host::dispatch_pointer_leave;
    using tk::Host::dispatch_pointer_move;
    using tk::Host::dispatch_pointer_up;
    using tk::Host::drag_hovered_widget_;
    using tk::Host::hovered_widget_;
    using tk::Host::pressed_widget_;
    using tk::Host::paint_tooltip_overlay;
    using tk::Host::paint_focus_overlay;
    using tk::Host::focus_visible_;
    using tk::Host::tooltip_owner_;
    using tk::Host::tooltip_text_;
    using tk::Host::tooltip_anchor_;
    using tk::Host::tooltip_visible_;
    using tk::Host::kTooltipShowDelayMs;
    using tk::Host::paint_toast_overlay;
    using tk::Host::toast_message_;
    using tk::Host::toast_visible_;
    using tk::Host::kToastDurationMs;

    // Drive the popup the way a paint pass would: register then promote.
    void set_active_popup(tk::Widget* w, tk::Widget* trigger = nullptr)
    {
        register_popup(w, trigger);
        popup_ = pending_popup_;
        popup_trigger_ = pending_popup_trigger_;
    }
    void clear_active_popup()
    {
        popup_.reset();
        popup_trigger_.reset();
    }

    // Point input_root_() at a tree built after construction (e.g. a view
    // that itself needed this host passed to *its* constructor) — the
    // constructor-only `root` param can't express that ordering.
    void set_root(tk::Widget* root) { root_ = root; }

    int repaint_count = 0;
    int relayout_count = 0;

    struct Delayed
    {
        int ms;
        std::function<void()> fn;
    };
    std::vector<Delayed> pending_delays_;
    std::vector<std::function<void()>> pending_ui_tasks_;

protected:
    tk::Widget* input_root_() const override { return root_; }

private:
    tk::Widget* root_;
};

// A NativeTextField that actually stores its text (TestHost's own
// make_text_field() returns nullptr — deliberately headless), for tests that
// need a tk::TextField's text()/set_text() to round-trip through a real
// native backend instead of silently no-op'ing against a null field_.
struct StubTextField : public tk::NativeTextField
{
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_focused(bool) override {}
    void set_visible(bool) override {}
    void set_enabled(bool) override {}
    void set_password(bool) override {}
    void set_on_changed(std::function<void(const std::string&)> f) override
    {
        on_changed = std::move(f);
    }
    void set_on_submit(std::function<void()>) override {}

    std::string text_;
    std::function<void(const std::string&)> on_changed;
};

// A NativeTextArea that actually stores its text (TestHost's own
// make_text_area() returns nullptr — deliberately headless), for tests that
// need a tk::TextArea's text()/set_text() to round-trip through a real
// native backend instead of silently no-op'ing against a null area_.
struct StubTextArea : public tk::NativeTextArea
{
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_focused(bool f) override { focused_ = f; }
    void set_visible(bool v) override { visible_ = v; }
    bool visible() const override { return visible_; }
    void set_enabled(bool) override {}
    float natural_height() const override { return natural_height_; }
    void set_on_changed(std::function<void(const std::string&)> f) override
    {
        on_changed = std::move(f);
    }
    void set_on_submit(std::function<void()>) override {}
    void set_on_height_changed(std::function<void(float)> f) override
    {
        on_height_changed = std::move(f);
    }
    void insert_at_cursor(std::string text) override { text_ += text; }
    tk::Rect cursor_rect() const override { return {}; }
    void replace_range(int start, int end, std::string text) override
    {
        text_ = text_.substr(0, start) + text + text_.substr(end);
    }
    void set_on_popup_nav(std::function<bool(tk::NavKey)> f) override
    {
        on_popup_nav = std::move(f);
    }
    void set_on_edit_last(std::function<bool()> f) override
    {
        on_edit_last = std::move(f);
    }
    void set_on_image_paste(ImagePasteHandler f) override
    {
        on_image_paste = std::move(f);
    }

    std::string text_;
    bool visible_ = true;
    bool focused_ = false;
    float natural_height_ = 0.0f;
    std::function<void(const std::string&)> on_changed;
    std::function<void(float)> on_height_changed;
    std::function<bool(tk::NavKey)> on_popup_nav;
    std::function<bool()> on_edit_last;
    ImagePasteHandler on_image_paste;
};

// TestHost that hands out a StubTextField/StubTextArea instead of the
// default nullptr, so a view's internally-owned tk::TextField/tk::TextArea
// members have a native backend to drive (set_text/text()/on_changed) in
// tests that care about round-tripped text, not just layout/paint.
struct StubHost : public TestHost
{
    StubHost() : TestHost(nullptr) {}

    std::unique_ptr<tk::NativeTextField> make_text_field() override
    {
        auto field = std::make_unique<StubTextField>();
        // Borrowed — owned by whichever tk::TextField this backs. Lets a
        // test simulate "user typed X" (fields_created[i]->on_changed("X"))
        // by reaching the backend directly, since tk::TextField itself
        // doesn't expose its private field_ for calling on_changed.
        fields_created.push_back(field.get());
        return field;
    }

    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        auto area = std::make_unique<StubTextArea>();
        // Borrowed — owned by whichever tk::TextArea this backs. Lets a
        // test simulate native-side events (areas_created[i]->on_changed(...),
        // ->on_popup_nav(...)) by reaching the backend directly, since
        // tk::TextArea doesn't expose its private area_ for calling these.
        areas_created.push_back(area.get());
        return area;
    }

    std::vector<StubTextField*> fields_created;
    std::vector<StubTextArea*> areas_created;
};
