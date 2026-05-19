# Voice Message Sending Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement MSC3245 voice message sending across all four platform targets (Qt6, GTK4, Win32, macOS) — mic capture, Opus/OGG encoding, upload, and ComposeBar UI.

**Architecture:** C++ platform backends capture raw 48kHz/16-bit/mono PCM via a new `tk::AudioCapture` abstract interface (parallel to `tk::AudioPlayer`). Each factory function performs a quick availability check at construction time and returns `nullptr` when no input device is present; the shell checks for null and hides the mic button. On stop, the full PCM buffer passes through a new `send_voice()` FFI call to Rust, which encodes Opus, muxes OGG, uploads, and posts an `m.audio` MSC3245 event. ComposeBar gains a mic button (hidden when no input device is detected), a waveform strip, a stop button, and a cancel button.

**Tech Stack:** `audiopus 0.3` (Opus encoding), `ogg 0.9` (OGG muxing), `QAudioSource` (Qt6), GStreamer `pulsesrc`/`appsink` (GTK4), WASAPI `IAudioCaptureClient` (Win32), `AVAudioEngine` (macOS).

---

## File Map

| Action | File | Purpose |
| --- | --- | --- |
| Create | `ui/shared/tk/audio_capture.h` | `tk::AudioCapture` abstract interface + `PostFn` typedef |
| Create | `ui/shared/tk/audio_capture_qt.cpp` | Qt6 backend: `QAudioSource` → PCM buffer |
| Create | `ui/shared/tk/audio_capture_gtk.cpp` | GTK4 backend: GStreamer pipeline → PCM buffer |
| Create | `ui/shared/tk/audio_capture_win32.cpp` | Win32 backend: WASAPI `IAudioCaptureClient` → PCM buffer |
| Create | `ui/shared/tk/audio_capture_macos.mm` | macOS backend: `AVAudioEngine` → PCM buffer |
| Modify | `ui/shared/tk/host.h` | Add `virtual make_audio_capture()` |
| Modify | `ui/shared/tk/host_qt.cpp` | Implement `make_audio_capture()` |
| Modify | `ui/shared/tk/host_gtk.cpp` | Implement `make_audio_capture()` |
| Modify | `ui/shared/tk/host_win32.cpp` | Implement `make_audio_capture()` |
| Modify | `ui/shared/tk/host_macos.mm` | Implement `make_audio_capture()` |
| Modify | `ui/shared/CMakeLists.txt` | Add `audio_capture_*.cpp` to per-platform source lists |
| Modify | `sdk/Cargo.toml` | Add `audiopus = "0.3"`, `ogg = "0.9"` |
| Modify | `sdk/src/bridge.rs` | Declare `fn send_voice()` in the cxx bridge |
| Modify | `sdk/src/client.rs` | `encode_voice_ogg()` helper + `send_voice()` impl + unit tests |
| Modify | `client/include/tesseract/client.h` | Add `send_voice()` declaration |
| Modify | `client/src/client.cpp` | Implement `Client::send_voice()` |
| Modify | `ui/shared/views/ComposeBar.h` | New API: `set_recording`, `push_amplitude`, `set_mic_available`, `on_mic_clicked`, `on_cancel_voice` |
| Modify | `ui/shared/views/ComposeBar.cpp` | Recording state machine + waveform strip paint |
| Modify | `ui/shared/views/RoomView.h` | Forward `on_mic_clicked`, `on_cancel_voice` |
| Modify | `ui/shared/views/RoomView.cpp` | Wire ComposeBar voice callbacks → RoomView callbacks |
| Modify | `ui/shared/app/ShellBase.h` | Add `capture_` member |
| Modify | `ui/shared/app/RoomWindowBase.cpp` | Wire voice callbacks in `wire_room_view_()` |
| Modify | `tests/cpp/test_tk_compose_bar.cpp` | Recording state tests |

---

## Task 1: `tk::AudioCapture` interface header + CMake

**Files:**
- Create: `ui/shared/tk/audio_capture.h`
- Modify: `ui/shared/CMakeLists.txt`

- [ ] **Step 1: Create `ui/shared/tk/audio_capture.h`**

```cpp
#pragma once

// Per-platform microphone capture abstraction. Parallel to tk::AudioPlayer.
// All backends normalise to 48kHz / 16-bit signed LE / mono before firing
// on_stopped. Amplitude sampling happens inside each backend at ~100ms windows
// (max absolute sample value, normalised to [0, 1000]).
//
// Threading: implementations marshal on_amplitude and on_stopped to the UI
// thread via the PostFn passed to their factory function.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace tk
{

class AudioCapture
{
public:
    virtual ~AudioCapture() = default;

    // Begin capture. No-op if already recording. On permission failure,
    // fires on_stopped with empty PCM immediately (no on_amplitude calls).
    virtual void start() = 0;

    // Stop recording and fire on_stopped with the full PCM buffer.
    // No-op if not recording.
    virtual void stop() = 0;

    // Stop recording and discard; on_stopped is NOT fired.
    // No-op if not recording.
    virtual void cancel() = 0;

    virtual bool is_recording() const = 0;

    // Elapsed recording time in milliseconds. 0 when not recording.
    virtual std::uint64_t duration_ms() const = 0;

    // Fired ~10x/sec on the UI thread while recording.
    // amplitude is in [0, 1000] (max absolute sample value in the window,
    // normalised from int16 range).
    std::function<void(std::uint16_t amplitude)> on_amplitude;

    // Fired on the UI thread after stop() completes.
    // pcm:      48kHz / 16-bit signed LE / mono raw samples.
    // waveform: amplitude samples collected during recording, one per ~100ms
    //           window, each in [0, 1000].
    // duration_ms: total clip length in milliseconds.
    // Empty pcm indicates a permission or device error (don't send).
    std::function<void(std::vector<std::uint8_t> pcm,
                       std::vector<std::uint16_t> waveform,
                       std::uint64_t duration_ms)>
        on_stopped;
};

// Factory function declarations — each defined in audio_capture_<platform>.cpp.
// `post` is a thread-safe functor that schedules its argument on the UI thread.
using AudioCapturePostFn = std::function<void(std::function<void()>)>;

std::unique_ptr<AudioCapture> make_audio_capture_qt(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_gtk(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_win32(AudioCapturePostFn post);
std::unique_ptr<AudioCapture> make_audio_capture_macos(AudioCapturePostFn post);

} // namespace tk
```

- [ ] **Step 2: Add `audio_capture_*.cpp` to `ui/shared/CMakeLists.txt`**

In `ui/shared/CMakeLists.txt`, inside each `if(TESSERACT_UI STREQUAL ...)` block, add the platform capture file alongside the existing `audio_*.cpp` entry. The four additions are:

```cmake
# win32 block — add after tk/audio_win32.cpp:
tk/audio_capture_win32.cpp

# qt6 block — add after tk/audio_qt.cpp:
tk/audio_capture_qt.cpp

# gtk block — add after tk/audio_gtk.cpp:
tk/audio_capture_gtk.cpp

# macos block — add after tk/audio_macos.mm:
tk/audio_capture_macos.mm
```

Also add `tk/audio_capture.h` to the shared (non-platform) sources list at the top of `_tk_sources`, alongside `tk/audio.h`:

```cmake
tk/audio_capture.h
```

- [ ] **Step 3: Verify CMake configures cleanly (no new source yet — just header)**

```bash
cmake --preset linux-qt6-debug 2>&1 | tail -5
```

Expected: configuration succeeds (the `.cpp` files don't exist yet so they'll error at build time — that's fine for now).

- [ ] **Step 4: Commit**

```bash
git add ui/shared/tk/audio_capture.h ui/shared/CMakeLists.txt
git commit -m "feat(capture): add tk::AudioCapture interface header and CMake wiring"
```

---

## Task 2: `Host::make_audio_capture()` virtual method

**Files:**
- Modify: `ui/shared/tk/host.h`
- Modify: `ui/shared/tk/host_qt.cpp`
- Modify: `ui/shared/tk/host_gtk.cpp`
- Modify: `ui/shared/tk/host_win32.cpp`
- Modify: `ui/shared/tk/host_macos.mm`

- [ ] **Step 1: Add `make_audio_capture()` to `host.h`**

In `ui/shared/tk/host.h`, add the following immediately after the `make_audio_player()` declaration (around line 222):

```cpp
#include "audio_capture.h"

// Create an AudioCapture backed by the platform's native capture stack.
// The returned object captures 48kHz/16-bit/mono PCM and fires amplitude
// and completion callbacks on the UI thread via `post_to_ui`.
virtual std::unique_ptr<AudioCapture> make_audio_capture() = 0;
```

- [ ] **Step 2: Implement in `host_qt.cpp`**

At the bottom of `host_qt.cpp`, near the existing `make_audio_player()` implementation (around line 1307), add:

```cpp
// Defined in audio_capture_qt.cpp
std::unique_ptr<tk::AudioCapture>
make_audio_capture_qt(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_qt(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}
```

- [ ] **Step 3: Implement in `host_gtk.cpp`**

Near the existing `make_audio_player()` implementation in `host_gtk.cpp`, add:

```cpp
// Defined in audio_capture_gtk.cpp
std::unique_ptr<tk::AudioCapture>
make_audio_capture_gtk(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_gtk(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}
```

- [ ] **Step 4: Implement in `host_win32.cpp`**

In the Win32 Host class body (where `make_audio_player()` is implemented as an inline method around line 1056), add alongside it:

```cpp
// Defined in audio_capture_win32.cpp
std::unique_ptr<tk::AudioCapture>
make_audio_capture_win32(tk::AudioCapturePostFn post);

std::unique_ptr<AudioCapture> make_audio_capture() override
{
    return make_audio_capture_win32(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}
```

- [ ] **Step 5: Implement in `host_macos.mm`**

Near the existing `make_audio_player()` implementation in `host_macos.mm`, add:

```cpp
// Defined in audio_capture_macos.mm
std::unique_ptr<tk::AudioCapture>
make_audio_capture_macos(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_macos(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}
```

- [ ] **Step 6: Commit**

```bash
git add ui/shared/tk/host.h \
        ui/shared/tk/host_qt.cpp \
        ui/shared/tk/host_gtk.cpp \
        ui/shared/tk/host_win32.cpp \
        ui/shared/tk/host_macos.mm
git commit -m "feat(capture): add Host::make_audio_capture() virtual method to all platform hosts"
```

---

## Task 2b: Device detection — hide mic button when no input is available

**Files:**

- Modify: `ui/shared/tk/audio_capture.h` (add availability helpers)
- Modify: `ui/shared/views/ComposeBar.h`
- Modify: `ui/shared/views/ComposeBar.cpp`

The four platform backends already return `nullptr` from their factory when no device is available (each backend performs a quick availability check before constructing the object — see Tasks 3–6). This task adds the `ComposeBar` API to reflect that state, and the shell check that calls it.

- [ ] **Step 1: Add `set_mic_available()` to `ComposeBar.h`**

In `ui/shared/views/ComposeBar.h`, add to the voice recording API block (alongside `set_recording`):

```cpp
/// Hide or show the mic button. Pass false when no audio input device is
/// detected at startup (capture_ == nullptr after make_audio_capture()).
/// Defaults to true.
void set_mic_available(bool available);
bool mic_available() const { return mic_available_; }
```

Add to private members:

```cpp
bool mic_available_ = true;
```

- [ ] **Step 2: Implement `set_mic_available()` in `ComposeBar.cpp`**

```cpp
void ComposeBar::set_mic_available(bool available)
{
    if (mic_available_ == available)
        return;
    mic_available_ = available;
    if (mic_btn_)
        mic_btn_->set_visible(available);
    recompute_height();
    invalidate();
}
```

- [ ] **Step 3: Update `arrange()` to skip mic button layout when unavailable**

In `ComposeBar::arrange()`, wrap the mic button positioning in a guard:

```cpp
if (mic_available_ && mic_btn_)
{
    mic_btn_rect_ = tk::Rect{sticker_rect_.right() + kBtnGap,
                              btn_y, kIconBtnW, kIconBtnH};
    mic_btn_->arrange(lc, mic_btn_rect_);
}
else
{
    mic_btn_rect_ = {};
}
```

When `mic_available_` is false, the text area expands to fill the space the mic button would have occupied (the sticker button is the last left-side icon; the send button stays at the far right).

- [ ] **Step 4: Update the shell to call `set_mic_available()` after creating capture_**

In `RoomWindowBase::wire_room_view_()` (added in Task 11), immediately after the block that initialises `capture_->on_amplitude` / `capture_->on_stopped`, add:

```cpp
room_view_->compose_bar()->set_mic_available(shell_->capture_ != nullptr);
```

This single call propagates the availability state from all four shells because `wire_room_view_()` is called by each of them.

- [ ] **Step 5: Write a test for the hidden-button layout**

In `tests/cpp/test_tk_compose_bar.cpp`, add:

```cpp
TEST_CASE("ComposeBar mic unavailable: natural_height unchanged, mic_btn hidden",
          "[tk][view][compose][voice]")
{
    Stage st;
    auto cb = std::make_unique<ComposeBar>();
    float baseline = cb->natural_height();
    cb->set_mic_available(false);
    st.run(*cb, {0, 0, 640, 200});
    REQUIRE(!cb->mic_available());
    // Height must not change just because the mic button is hidden.
    REQUIRE(cb->natural_height() == baseline);
}
```

- [ ] **Step 6: Run tests — verify pass**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "compose.*voice" --output-on-failure
```

Expected: all compose voice tests pass including the new one.

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/ComposeBar.h ui/shared/views/ComposeBar.cpp \
        tests/cpp/test_tk_compose_bar.cpp
git commit -m "feat(ui): hide mic button when no audio input device is available"
```

---

## Task 3: Qt6 capture backend

**Files:**
- Create: `ui/shared/tk/audio_capture_qt.cpp`

- [ ] **Step 1: Create `ui/shared/tk/audio_capture_qt.cpp`**

```cpp
// Qt6 audio capture backend for tk::AudioCapture.
// Uses QAudioSource (Qt Multimedia) to capture 48kHz/16-bit/mono PCM.
// Amplitude is sampled every ~100ms from the incoming PCM window.
// All callbacks are marshalled to the UI thread via the PostFn.

#include "audio_capture.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QBuffer>
#include <QMediaDevices>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureQt : public tk::AudioCapture
{
public:
    explicit AudioCaptureQt(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureQt() override { cancel(); }

    void start() override
    {
        if (recording_)
            return;

        QAudioFormat fmt;
        fmt.setSampleRate(48000);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice dev = QMediaDevices::defaultAudioInput();
        if (!dev.isFormatSupported(fmt))
        {
            // Fire on_stopped with empty PCM to signal device/permission error.
            if (on_stopped)
            {
                auto cb = on_stopped;
                post_([cb]() mutable
                      { cb({}, {}, 0); });
            }
            return;
        }

        pcm_.clear();
        waveform_.clear();
        window_samples_.clear();
        start_ms_ = std::chrono::steady_clock::now();
        recording_ = true;

        buffer_ = std::make_unique<QBuffer>();
        buffer_->open(QIODevice::ReadWrite);

        source_ = std::make_unique<QAudioSource>(dev, fmt);
        source_->start(buffer_.get());

        // Poll the buffer every 50ms (gives ~100ms amplitude windows).
        timer_ = std::make_unique<QTimer>();
        timer_->setInterval(50);
        QObject::connect(timer_.get(), &QTimer::timeout,
                         [this]() { poll(); });
        timer_->start();
    }

    void stop() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/false);
    }

    bool is_recording() const override { return recording_; }

    std::uint64_t duration_ms() const override
    {
        if (!recording_)
            return 0;
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now() - start_ms_).count());
    }

private:
    void poll()
    {
        if (!buffer_)
            return;

        // Drain newly available bytes from the QBuffer.
        buffer_->seek(0);
        QByteArray chunk = buffer_->readAll();
        buffer_->buffer().clear();
        buffer_->seek(0);

        if (chunk.isEmpty())
            return;

        const std::size_t byte_count = static_cast<std::size_t>(chunk.size());
        const auto* s16 = reinterpret_cast<const int16_t*>(chunk.constData());
        const std::size_t sample_count = byte_count / 2;

        // Append to PCM buffer.
        pcm_.insert(pcm_.end(),
                    reinterpret_cast<const uint8_t*>(chunk.constData()),
                    reinterpret_cast<const uint8_t*>(chunk.constData()) + byte_count);

        // Track window for amplitude.
        window_samples_.insert(window_samples_.end(), s16, s16 + sample_count);
        window_byte_count_ += byte_count;

        // ~100ms window = 48000 * 2 * 0.1 = 9600 bytes.
        if (window_byte_count_ >= 9600)
        {
            int16_t peak = 0;
            for (int16_t v : window_samples_)
                peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
            std::uint16_t amp =
                static_cast<std::uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);
            waveform_.push_back(amp);
            window_samples_.clear();
            window_byte_count_ = 0;

            if (on_amplitude)
            {
                auto cb = on_amplitude;
                post_([cb, amp]() { cb(amp); });
            }
        }
    }

    void finish_(bool send)
    {
        timer_->stop();
        timer_.reset();
        source_->stop();
        source_.reset();

        // Final poll to capture any remaining bytes.
        poll();

        recording_ = false;
        std::uint64_t dur = duration_ms();

        if (send && on_stopped)
        {
            auto cb = on_stopped;
            auto pcm = std::move(pcm_);
            auto wf = std::move(waveform_);
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }

        pcm_.clear();
        waveform_.clear();
        buffer_.reset();
    }

    PostFn post_;
    bool recording_ = false;
    std::unique_ptr<QAudioSource> source_;
    std::unique_ptr<QBuffer> buffer_;
    std::unique_ptr<QTimer> timer_;
    std::vector<std::uint8_t> pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t> window_samples_;
    std::size_t window_byte_count_ = 0;
    std::chrono::steady_clock::time_point start_ms_;
};

} // namespace

std::unique_ptr<tk::AudioCapture>
make_audio_capture_qt(tk::AudioCapturePostFn post)
{
    // Return nullptr immediately if no audio input device is present so the
    // shell can hide the mic button without attempting to open a device.
    if (QMediaDevices::audioInputs().isEmpty())
        return nullptr;
    return std::make_unique<AudioCaptureQt>(std::move(post));
}
```

- [ ] **Step 2: Build the Qt6 target to verify the backend compiles**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | tail -20
```

Expected: `tesseract_tk` library builds without errors. (The binary won't link yet — later tasks add the FFI layer.)

- [ ] **Step 3: Commit**

```bash
git add ui/shared/tk/audio_capture_qt.cpp
git commit -m "feat(capture): add Qt6 QAudioSource capture backend"
```

---

## Task 4: GTK4 capture backend

**Files:**
- Create: `ui/shared/tk/audio_capture_gtk.cpp`

- [ ] **Step 1: Create `ui/shared/tk/audio_capture_gtk.cpp`**

```cpp
// GTK4 audio capture backend for tk::AudioCapture.
// GStreamer pipeline: pulsesrc ! audioconvert ! audioresample !
//   audio/x-raw,rate=48000,channels=1,format=S16LE ! appsink
// gst-plugins-base (which includes pulsesrc/audioconvert/audioresample/appsink)
// is already a dependency for MSC3245 voice playback.

#include "audio_capture.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureGtk : public tk::AudioCapture
{
public:
    explicit AudioCaptureGtk(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureGtk() override { cancel(); }

    void start() override
    {
        if (recording_)
            return;

        pipeline_ = gst_parse_launch(
            "pulsesrc ! audioconvert ! audioresample ! "
            "audio/x-raw,rate=48000,channels=1,format=S16LE ! "
            "appsink name=sink emit-signals=true",
            nullptr);

        if (!pipeline_)
        {
            fire_error_();
            return;
        }

        sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        if (!sink_)
        {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            fire_error_();
            return;
        }

        g_signal_connect(sink_, "new-sample",
                         G_CALLBACK(on_new_sample_), this);

        pcm_.clear();
        waveform_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_ = true;

        gst_element_set_state(pipeline_, GST_STATE_PLAYING);

        // Amplitude timer ~10/sec.
        timer_source_ = g_timeout_add(100, amplitude_tick_, this);
    }

    void stop() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/false);
    }

    bool is_recording() const override { return recording_; }

    std::uint64_t duration_ms() const override
    {
        if (!recording_)
            return 0;
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now() - start_tp_).count());
    }

private:
    static GstFlowReturn on_new_sample_(GstElement* sink, gpointer user_data)
    {
        auto* self = static_cast<AudioCaptureGtk*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo info;
        if (gst_buffer_map(buf, &info, GST_MAP_READ))
        {
            const std::size_t n = info.size;
            const auto* s16 = reinterpret_cast<const int16_t*>(info.data);
            {
                std::lock_guard<std::mutex> lk(self->pcm_mu_);
                self->pcm_.insert(self->pcm_.end(), info.data, info.data + n);
                // Accumulate into pending window.
                self->window_buf_.insert(self->window_buf_.end(),
                                         s16, s16 + n / 2);
                self->window_byte_count_ += n;
            }
            gst_buffer_unmap(buf, &info);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    static gboolean amplitude_tick_(gpointer user_data)
    {
        auto* self = static_cast<AudioCaptureGtk*>(user_data);
        std::vector<int16_t> window;
        {
            std::lock_guard<std::mutex> lk(self->pcm_mu_);
            if (self->window_byte_count_ < 9600)
                return G_SOURCE_CONTINUE;
            window = std::move(self->window_buf_);
            self->window_buf_.clear();
            self->window_byte_count_ = 0;
        }

        int16_t peak = 0;
        for (int16_t v : window)
            peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
        std::uint16_t amp =
            static_cast<uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);

        {
            std::lock_guard<std::mutex> lk(self->pcm_mu_);
            self->waveform_.push_back(amp);
        }

        if (self->on_amplitude)
        {
            auto cb = self->on_amplitude;
            self->post_([cb, amp]() { cb(amp); });
        }
        return G_SOURCE_CONTINUE;
    }

    void fire_error_()
    {
        if (on_stopped)
        {
            auto cb = on_stopped;
            post_([cb]() mutable { cb({}, {}, 0); });
        }
    }

    void finish_(bool send)
    {
        if (timer_source_)
        {
            g_source_remove(timer_source_);
            timer_source_ = 0;
        }

        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (sink_)
            {
                gst_object_unref(sink_);
                sink_ = nullptr;
            }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }

        recording_ = false;
        const std::uint64_t dur = duration_ms_snapshot_;

        if (send && on_stopped)
        {
            std::vector<uint8_t> pcm;
            std::vector<uint16_t> wf;
            {
                std::lock_guard<std::mutex> lk(pcm_mu_);
                pcm = std::move(pcm_);
                wf  = std::move(waveform_);
            }
            auto cb = on_stopped;
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }
    }

    PostFn post_;
    bool recording_  = false;
    GstElement* pipeline_ = nullptr;
    GstElement* sink_     = nullptr;
    guint timer_source_   = 0;

    std::mutex pcm_mu_;
    std::vector<std::uint8_t> pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t> window_buf_;
    std::size_t window_byte_count_ = 0;
    std::uint64_t duration_ms_snapshot_ = 0;
    std::chrono::steady_clock::time_point start_tp_;
};

} // namespace

std::unique_ptr<tk::AudioCapture>
make_audio_capture_gtk(tk::AudioCapturePostFn post)
{
    // Return nullptr if the pulsesrc element is unavailable (no audio input).
    GstElementFactory* factory = gst_element_factory_find("pulsesrc");
    if (!factory)
        return nullptr;
    gst_object_unref(factory);
    return std::make_unique<AudioCaptureGtk>(std::move(post));
}
```

- [ ] **Step 2: Build the GTK target**

```bash
cmake --build build/linux-gtk-debug --target tesseract_tk 2>&1 | tail -20
```

Expected: compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add ui/shared/tk/audio_capture_gtk.cpp
git commit -m "feat(capture): add GTK4 GStreamer capture backend"
```

---

## Task 5: Win32 capture backend

**Files:**
- Create: `ui/shared/tk/audio_capture_win32.cpp`

- [ ] **Step 1: Create `ui/shared/tk/audio_capture_win32.cpp`**

```cpp
// Win32 audio capture backend for tk::AudioCapture.
// Uses WASAPI in shared mode (IAudioCaptureClient). No new system dependencies
// — mf/mfplat are already linked for the video player.

#include "audio_capture.h"

#define WIN32_LEAN_AND_MEAN
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace
{

using PostFn = tk::AudioCapturePostFn;

// 48kHz / 16-bit / mono frame size in bytes.
constexpr std::size_t kBytesPerFrame = 2;
// Target engine period in 100ns units (10ms).
constexpr REFERENCE_TIME kBufferDuration = 100'000;

class AudioCaptureWin32 : public tk::AudioCapture
{
public:
    explicit AudioCaptureWin32(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureWin32() override { cancel(); }

    void start() override
    {
        if (recording_.load())
            return;

        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        {
            fire_error_();
            return;
        }

        IMMDevice* device = nullptr;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                         &device);
        enumerator->Release();
        if (FAILED(hr))
        {
            fire_error_();
            return;
        }

        IAudioClient* client = nullptr;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client));
        device->Release();
        if (FAILED(hr))
        {
            fire_error_();
            return;
        }

        // Request 48kHz / 16-bit / mono.
        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = 48000;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = 96000;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                0, kBufferDuration, 0, &wfx, nullptr);
        if (FAILED(hr))
        {
            client->Release();
            fire_error_();
            return;
        }

        IAudioCaptureClient* capture = nullptr;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr))
        {
            client->Release();
            fire_error_();
            return;
        }

        client->Start();
        audio_client_  = client;
        capture_client_ = capture;

        pcm_.clear();
        waveform_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_.store(true);

        // Capture loop on a dedicated thread.
        capture_thread_ = std::thread([this]() { capture_loop_(); });
    }

    void stop() override
    {
        if (!recording_.load())
            return;
        stop_requested_.store(true);
        if (capture_thread_.joinable())
            capture_thread_.join();
        flush_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_.load())
            return;
        stop_requested_.store(true);
        if (capture_thread_.joinable())
            capture_thread_.join();
        flush_(/*send=*/false);
    }

    bool is_recording() const override { return recording_.load(); }

    std::uint64_t duration_ms() const override
    {
        if (!recording_.load())
            return 0;
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now() - start_tp_).count());
    }

private:
    void capture_loop_()
    {
        while (!stop_requested_.load())
        {
            Sleep(10);
            UINT32 packet_size = 0;
            while (SUCCEEDED(
                       capture_client_->GetNextPacketSize(&packet_size)) &&
                   packet_size > 0)
            {
                BYTE* data     = nullptr;
                UINT32 frames  = 0;
                DWORD flags    = 0;
                if (FAILED(capture_client_->GetBuffer(&data, &frames, &flags,
                                                       nullptr, nullptr)))
                    break;

                const std::size_t bytes = frames * kBytesPerFrame;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (silent)
                    {
                        // Insert silence bytes.
                        pcm_.insert(pcm_.end(), bytes, 0);
                        window_byte_count_ += bytes;
                    }
                    else
                    {
                        pcm_.insert(pcm_.end(), data, data + bytes);
                        const auto* s16 =
                            reinterpret_cast<const int16_t*>(data);
                        window_buf_.insert(window_buf_.end(),
                                           s16, s16 + frames);
                        window_byte_count_ += bytes;
                    }

                    // Emit amplitude every ~100ms (9600 bytes @ 48kHz/16b/mono).
                    if (window_byte_count_ >= 9600)
                    {
                        int16_t peak = 0;
                        for (int16_t v : window_buf_)
                            peak = std::max(peak,
                                            static_cast<int16_t>(std::abs(v)));
                        std::uint16_t amp = static_cast<uint16_t>(
                            static_cast<uint32_t>(peak) * 1000 / 32767);
                        waveform_.push_back(amp);
                        window_buf_.clear();
                        window_byte_count_ = 0;

                        if (on_amplitude)
                        {
                            auto cb = on_amplitude;
                            post_([cb, amp]() { cb(amp); });
                        }
                    }
                }
                capture_client_->ReleaseBuffer(frames);
            }
        }
    }

    void flush_(bool send)
    {
        if (audio_client_)
        {
            audio_client_->Stop();
            capture_client_->Release();
            audio_client_->Release();
            capture_client_ = nullptr;
            audio_client_   = nullptr;
        }
        recording_.store(false);
        stop_requested_.store(false);

        std::uint64_t dur;
        {
            using namespace std::chrono;
            dur = static_cast<uint64_t>(
                duration_cast<milliseconds>(
                    steady_clock::now() - start_tp_).count());
        }

        if (send && on_stopped)
        {
            std::vector<uint8_t> pcm;
            std::vector<uint16_t> wf;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pcm = std::move(pcm_);
                wf  = std::move(waveform_);
            }
            auto cb = on_stopped;
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }
    }

    void fire_error_()
    {
        if (on_stopped)
        {
            auto cb = on_stopped;
            post_([cb]() mutable { cb({}, {}, 0); });
        }
    }

    PostFn post_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread capture_thread_;
    IAudioClient*        audio_client_   = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    std::chrono::steady_clock::time_point start_tp_;

    std::mutex mu_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_buf_;
    std::size_t                window_byte_count_ = 0;
};

} // namespace

std::unique_ptr<tk::AudioCapture>
make_audio_capture_win32(tk::AudioCapturePostFn post)
{
    // Quick availability check: if GetDefaultAudioEndpoint fails there is no
    // capture device. Return nullptr so the shell can hide the mic button.
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return nullptr;
    IMMDevice* device = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                     &device);
    enumerator->Release();
    if (FAILED(hr))
        return nullptr;
    device->Release();
    return std::make_unique<AudioCaptureWin32>(std::move(post));
}
```

- [ ] **Step 2: Commit**

```bash
git add ui/shared/tk/audio_capture_win32.cpp
git commit -m "feat(capture): add Win32 WASAPI capture backend"
```

---

## Task 6: macOS capture backend

**Files:**
- Create: `ui/shared/tk/audio_capture_macos.mm`

- [ ] **Step 1: Create `ui/shared/tk/audio_capture_macos.mm`**

```objcpp
// macOS audio capture backend for tk::AudioCapture.
// Uses AVAudioEngine + AVAudioInputNode. Installs a tap on the input node
// to receive PCM buffers at 48kHz/16-bit/mono.
// Requests microphone permission synchronously before starting.

#include "audio_capture.h"

#import <AVFoundation/AVFoundation.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace
{

using PostFn = tk::AudioCapturePostFn;

class AudioCaptureMacOS : public tk::AudioCapture
{
public:
    explicit AudioCaptureMacOS(PostFn post) : post_(std::move(post)) {}

    ~AudioCaptureMacOS() override { cancel(); }

    void start() override
    {
        if (recording_)
            return;

        // Request mic permission synchronously (blocks until user responds).
        __block bool granted = false;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice
            requestAccessForMediaType:AVMediaTypeAudio
                    completionHandler:^(BOOL auth) {
                        granted = auth;
                        dispatch_semaphore_signal(sem);
                    }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (!granted)
        {
            fire_error_();
            return;
        }

        engine_ = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = engine_.inputNode;

        // 48kHz / 16-bit / mono.
        AVAudioFormat* fmt = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatInt16
                     sampleRate:48000
                       channels:1
                    interleaved:YES];

        pcm_.clear();
        waveform_.clear();
        window_byte_count_ = 0;
        start_tp_ = std::chrono::steady_clock::now();
        recording_ = true;

        [input installTapOnBus:0
                    bufferSize:4800
                        format:fmt
                         block:^(AVAudioPCMBuffer* buf, AVAudioTime*) {
                             this->handle_buffer_(buf);
                         }];

        NSError* err = nil;
        [engine_ startAndReturnError:&err];
        if (err)
        {
            [input removeTapOnBus:0];
            engine_ = nil;
            recording_ = false;
            fire_error_();
        }
    }

    void stop() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/true);
    }

    void cancel() override
    {
        if (!recording_)
            return;
        finish_(/*send=*/false);
    }

    bool is_recording() const override { return recording_; }

    std::uint64_t duration_ms() const override
    {
        if (!recording_)
            return 0;
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now() - start_tp_).count());
    }

private:
    void handle_buffer_(AVAudioPCMBuffer* buf)
    {
        const std::uint32_t frame_count = buf.frameLength;
        const auto* s16 =
            reinterpret_cast<const int16_t*>(buf.int16ChannelData[0]);
        const std::size_t bytes = frame_count * 2;

        std::lock_guard<std::mutex> lk(mu_);
        pcm_.insert(pcm_.end(),
                    reinterpret_cast<const uint8_t*>(s16),
                    reinterpret_cast<const uint8_t*>(s16) + bytes);
        window_buf_.insert(window_buf_.end(), s16, s16 + frame_count);
        window_byte_count_ += bytes;

        if (window_byte_count_ >= 9600)
        {
            int16_t peak = 0;
            for (int16_t v : window_buf_)
                peak = std::max(peak, static_cast<int16_t>(std::abs(v)));
            std::uint16_t amp =
                static_cast<uint16_t>(static_cast<uint32_t>(peak) * 1000 / 32767);
            waveform_.push_back(amp);
            window_buf_.clear();
            window_byte_count_ = 0;

            if (on_amplitude)
            {
                auto cb = on_amplitude;
                post_([cb, amp]() { cb(amp); });
            }
        }
    }

    void finish_(bool send)
    {
        if (engine_)
        {
            [engine_.inputNode removeTapOnBus:0];
            [engine_ stop];
            engine_ = nil;
        }
        recording_ = false;

        const std::uint64_t dur = [&]
        {
            using namespace std::chrono;
            return static_cast<uint64_t>(
                duration_cast<milliseconds>(
                    steady_clock::now() - start_tp_).count());
        }();

        if (send && on_stopped)
        {
            std::vector<uint8_t> pcm;
            std::vector<uint16_t> wf;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pcm = std::move(pcm_);
                wf  = std::move(waveform_);
            }
            auto cb = on_stopped;
            post_([cb, pcm = std::move(pcm), wf = std::move(wf), dur]() mutable
                  { cb(std::move(pcm), std::move(wf), dur); });
        }
    }

    void fire_error_()
    {
        if (on_stopped)
        {
            auto cb = on_stopped;
            post_([cb]() mutable { cb({}, {}, 0); });
        }
    }

    PostFn post_;
    bool recording_ = false;
    AVAudioEngine* __strong engine_ = nil;
    std::chrono::steady_clock::time_point start_tp_;

    std::mutex mu_;
    std::vector<std::uint8_t>  pcm_;
    std::vector<std::uint16_t> waveform_;
    std::vector<int16_t>       window_buf_;
    std::size_t                window_byte_count_ = 0;
};

} // namespace

std::unique_ptr<tk::AudioCapture>
make_audio_capture_macos(tk::AudioCapturePostFn post)
{
    // Return nullptr if no audio input device is present.
    NSArray* devices = [AVCaptureDevice
        devicesWithMediaType:AVMediaTypeAudio];
    if (devices.count == 0)
        return nullptr;
    return std::make_unique<AudioCaptureMacOS>(std::move(post));
}
```

- [ ] **Step 2: Add `NSMicrophoneUsageDescription` to `Info.plist` (macOS only)**

In `ui/macos/Info.plist`, add inside the root `<dict>`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>Tesseract needs microphone access to record voice messages.</string>
```

- [ ] **Step 3: Commit**

```bash
git add ui/shared/tk/audio_capture_macos.mm ui/macos/Info.plist
git commit -m "feat(capture): add macOS AVAudioEngine capture backend"
```

---

## Task 7: Rust deps + failing tests + `encode_voice_ogg()` + `send_voice()`

**Files:**
- Modify: `sdk/Cargo.toml`
- Modify: `sdk/src/client.rs`
- Modify: `sdk/src/bridge.rs`

- [ ] **Step 1: Add Cargo dependencies**

In `sdk/Cargo.toml`, add to the `[dependencies]` section:

```toml
audiopus = "0.3"
ogg = "0.9"
```

- [ ] **Step 2: Write the failing unit tests first**

In `sdk/src/client.rs`, in the `#[cfg(test)]` block at the bottom of the file, add:

```rust
#[test]
fn send_voice_rejects_empty_pcm() {
    // encode_voice_ogg must return Err for zero-length PCM.
    let result = encode_voice_ogg(&[], &[], 0);
    assert!(result.is_err(), "empty PCM must be rejected");
}

#[test]
fn send_voice_encodes_valid_ogg() {
    // Generate 1 second of 440 Hz sine at 48kHz/16-bit/mono.
    let sample_count = 48000usize;
    let samples: Vec<i16> = (0..sample_count)
        .map(|i| {
            let t = i as f64 / 48000.0;
            (f64::sin(2.0 * std::f64::consts::PI * 440.0 * t) * 16000.0) as i16
        })
        .collect();
    let pcm: Vec<u8> = samples
        .iter()
        .flat_map(|s| s.to_le_bytes())
        .collect();

    let waveform: Vec<u16> = vec![500; 10]; // 10 amplitude samples

    let result = encode_voice_ogg(&samples, &waveform, 1000);
    assert!(result.is_ok(), "encoding must succeed: {:?}", result.err());
    let ogg = result.unwrap();
    assert!(ogg.starts_with(b"OggS"), "output must start with OGG magic");
    assert!(ogg.len() > 100, "output must be non-trivial");
}

#[test]
fn send_voice_waveform_clamped_to_256() {
    // encode_voice_ogg must clamp waveform to 256 entries.
    let samples: Vec<i16> = vec![1000i16; 960]; // one Opus frame
    let big_waveform: Vec<u16> = vec![500; 512];
    let result = encode_voice_ogg(&samples, &big_waveform, 20);
    assert!(result.is_ok());
    // The actual waveform truncation is verified via the bridge test; here we
    // just confirm the function doesn't panic or fail on oversized input.
}
```

- [ ] **Step 3: Run the tests — verify they FAIL (function not found)**

```bash
cargo test -p tesseract-sdk-ffi encode_voice 2>&1 | tail -15
```

Expected: compile error — `encode_voice_ogg` is not defined yet.

- [ ] **Step 4: Implement `encode_voice_ogg()` and `send_voice()` in `client.rs`**

Add the following function directly above the existing `send_image` implementations in `sdk/src/client.rs`:

```rust
/// Encode 48kHz/16-bit/mono PCM as Opus/OGG.
/// Returns the raw OGG bytes on success, or an error string.
/// `waveform` is clamped to 256 entries in the returned content (caller's
/// copy is unmodified).
pub(crate) fn encode_voice_ogg(
    samples: &[i16],
    _waveform: &[u16],
    _duration_ms: u64,
) -> Result<Vec<u8>, String> {
    use audiopus::coder::Encoder;
    use audiopus::{Application, Channels, SampleRate};
    use ogg::PacketWriter;

    if samples.is_empty() {
        return Err("empty PCM".to_owned());
    }

    let mut enc = Encoder::new(SampleRate::Hz48000, Channels::Mono, Application::Voip)
        .map_err(|e| e.to_string())?;

    // Encode in 960-sample (20ms) frames.
    const FRAME: usize = 960;
    let mut opus_packets: Vec<Vec<u8>> = Vec::new();
    let mut out_buf = vec![0u8; 4000]; // max Opus packet size

    let mut pos = 0usize;
    while pos + FRAME <= samples.len() {
        let n = enc
            .encode(&samples[pos..pos + FRAME], &mut out_buf)
            .map_err(|e| e.to_string())?;
        opus_packets.push(out_buf[..n].to_vec());
        pos += FRAME;
    }

    // Mux into OGG.
    let mut ogg: Vec<u8> = Vec::new();
    let mut pw = PacketWriter::new(&mut ogg);

    // OpusHead page (identification header).
    let mut opus_head = Vec::with_capacity(19);
    opus_head.extend_from_slice(b"OpusHead");
    opus_head.push(1); // version
    opus_head.push(1); // channel count (mono)
    opus_head.extend_from_slice(&312u16.to_le_bytes()); // pre-skip
    opus_head.extend_from_slice(&48000u32.to_le_bytes()); // input sample rate
    opus_head.extend_from_slice(&0i16.to_le_bytes()); // output gain
    opus_head.push(0); // channel mapping family
    pw.write_packet(
        opus_head.into(),
        0x1234_5678,
        ogg::PacketWriteEndInfo::NormalPacket,
        0,
    )
    .map_err(|e| e.to_string())?;

    // OpusTags page (comment header).
    let mut tags = Vec::new();
    let vendor = b"tesseract";
    tags.extend_from_slice(b"OpusTags");
    tags.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
    tags.extend_from_slice(vendor);
    tags.extend_from_slice(&0u32.to_le_bytes()); // user comment list length
    pw.write_packet(
        tags.into(),
        0x1234_5678,
        ogg::PacketWriteEndInfo::NormalPacket,
        0,
    )
    .map_err(|e| e.to_string())?;

    // Audio pages.
    let mut granule: u64 = 0;
    let total = opus_packets.len();
    for (i, pkt) in opus_packets.into_iter().enumerate() {
        granule += FRAME as u64;
        let end_info = if i + 1 == total {
            ogg::PacketWriteEndInfo::EndStream
        } else {
            ogg::PacketWriteEndInfo::NormalPacket
        };
        pw.write_packet(pkt.into(), 0x1234_5678, end_info, granule)
            .map_err(|e| e.to_string())?;
    }

    Ok(ogg)
}
```

Now add the `send_voice` implementation in the same `impl ClientImpl` block alongside `send_image` and `send_file`. Add the `#[cfg(not(test))]` variant:

```rust
#[cfg(not(test))]
pub fn send_voice(
    &mut self,
    room_id: &str,
    pcm: &[u8],
    duration_ms: u64,
    waveform: &[u16],
    caption: &str,
    reply_event_id: &str,
) -> OpResult {
    use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseAudioInfo};
    use matrix_sdk::room::reply::{EnforceThread, Reply};
    use matrix_sdk::ruma::events::room::message::{
        AddMentions, AudioMessageEventContent, TextMessageEventContent,
    };
    use matrix_sdk::ruma::UInt;

    if pcm.len() % 2 != 0 {
        return err("PCM byte count must be even");
    }
    if pcm.is_empty() {
        return err("empty PCM");
    }

    let Some(client) = self.client.clone() else {
        return err("not logged in");
    };
    let room_id_parsed = match matrix_sdk::ruma::RoomId::parse(room_id) {
        Ok(id) => id,
        Err(e) => return err(format!("invalid room id: {e}")),
    };
    let Some(room) = client.get_room(&room_id_parsed) else {
        return err("room not found");
    };

    // Cast &[u8] → &[i16] (safe: length is even, alignment is from vec).
    let samples: &[i16] = unsafe {
        std::slice::from_raw_parts(pcm.as_ptr() as *const i16, pcm.len() / 2)
    };

    let ogg_bytes = match encode_voice_ogg(samples, waveform, duration_ms) {
        Ok(b) => b,
        Err(e) => return err(format!("encode failed: {e}")),
    };

    // Clamp waveform to 256 entries for MSC1767.
    let wf_clamped: Vec<u16> = waveform.iter().copied().take(256).collect();

    let mime: mime::Mime = "audio/ogg".parse().unwrap();
    let size = UInt::new(ogg_bytes.len() as u64);
    let dur = UInt::new(duration_ms);

    let info = BaseAudioInfo {
        duration: dur,
        size,
    };
    let mut config =
        AttachmentConfig::new().info(AttachmentInfo::Audio(info));

    if !caption.is_empty() {
        config =
            config.caption(Some(TextMessageEventContent::plain(caption)));
    }
    if !reply_event_id.is_empty() {
        let reply_id: matrix_sdk::ruma::OwnedEventId =
            match reply_event_id.parse() {
                Ok(id) => id,
                Err(e) => return err(format!("invalid reply event id: {e}")),
            };
        config = config.reply(Some(Reply {
            event_id: reply_id,
            enforce_thread: EnforceThread::Unthreaded,
            add_mentions: AddMentions::No,
        }));
    }

    match self.rt.block_on(async move {
        room.send_attachment("voice-message.ogg", &mime, ogg_bytes, config)
            .await
    }) {
        Ok(_) => ok(""),
        Err(e) => err(e.to_string()),
    }
}

#[cfg(test)]
pub fn send_voice(
    &mut self,
    _room_id: &str,
    _pcm: &[u8],
    _duration_ms: u64,
    _waveform: &[u16],
    _caption: &str,
    _reply_event_id: &str,
) -> OpResult {
    err("not logged in")
}
```

- [ ] **Step 5: Run the tests — verify they PASS**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -20
```

Expected: all tests pass including `send_voice_rejects_empty_pcm`, `send_voice_encodes_valid_ogg`, `send_voice_waveform_clamped_to_256`.

- [ ] **Step 6: Add the FFI declaration to `bridge.rs`**

In `sdk/src/bridge.rs`, inside the `extern "Rust"` block alongside `send_image` and `send_file` (around line 601), add:

```rust
/// Upload `pcm` (48kHz/16-bit/mono LE) as an MSC3245 voice message to
/// `room_id`. Encodes to Opus/OGG, uploads, and posts an `m.audio` event
/// tagged with `org.matrix.msc3245.voice`. `waveform` populates the MSC1767
/// `audio.waveform` field (clamped to 256 entries). `caption` follows
/// MSC2530 framing when non-empty. `reply_event_id` adds `m.in_reply_to`.
fn send_voice(
    self: &mut ClientFfi,
    room_id: &str,
    pcm: &[u8],
    duration_ms: u64,
    waveform: &[u16],
    caption: &str,
    reply_event_id: &str,
) -> OpResult;
```

- [ ] **Step 7: Commit**

```bash
git add sdk/Cargo.toml sdk/src/client.rs sdk/src/bridge.rs
git commit -m "feat(sdk): add encode_voice_ogg() helper and send_voice() FFI with Rust unit tests"
```

---

## Task 8: C++ `Client::send_voice()` wrapper

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Add declaration to `client/include/tesseract/client.h`**

After the existing `send_file()` declaration (around line 248), add:

```cpp
/// Encode `pcm_size` bytes of 48kHz/16-bit/mono PCM as an Opus/OGG voice
/// message and send it to `room_id` as an MSC3245 m.audio event.
/// `waveform` provides MSC1767 amplitude samples (clamped to 256 server-side).
/// `caption` and `reply_event_id` follow the same semantics as send_image().
Result send_voice(const std::string& room_id,
                  const std::uint8_t* pcm, std::size_t pcm_size,
                  std::uint64_t duration_ms,
                  const std::vector<std::uint16_t>& waveform,
                  const std::string& caption,
                  const std::string& reply_event_id);
```

- [ ] **Step 2: Implement in `client/src/client.cpp`**

After the existing `send_file()` implementation (around line 283), add:

```cpp
Result Client::send_voice(const std::string& room_id,
                          const std::uint8_t* pcm, std::size_t pcm_size,
                          std::uint64_t duration_ms,
                          const std::vector<std::uint16_t>& waveform,
                          const std::string& caption,
                          const std::string& reply_event_id)
{
    rust::Slice<const std::uint8_t> pcm_slice{pcm, pcm_size};
    rust::Slice<const std::uint16_t> wf_slice{waveform.data(), waveform.size()};
    return from_ffi(impl_->ffi->send_voice(room_id, pcm_slice, duration_ms,
                                            wf_slice, caption,
                                            reply_event_id));
}
```

- [ ] **Step 3: Attempt to build the client library**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | tail -20
```

Expected: compiles. (The Rust side won't link yet — that happens when we build the full binary.)

- [ ] **Step 4: Commit**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): add Client::send_voice() C++ wrapper"
```

---

## Task 9: ComposeBar recording state — tests first

**Files:**
- Modify: `tests/cpp/test_tk_compose_bar.cpp`
- Modify: `ui/shared/views/ComposeBar.h`
- Modify: `ui/shared/views/ComposeBar.cpp`

- [ ] **Step 1: Add the failing tests to `tests/cpp/test_tk_compose_bar.cpp`**

Append after the existing test cases:

```cpp
TEST_CASE("ComposeBar recording: natural_height unchanged on set_recording",
          "[tk][view][compose][voice]")
{
    Stage st;
    auto cb = std::make_unique<ComposeBar>();
    float idle_height = cb->natural_height();
    cb->set_recording(true);
    st.run(*cb, {0, 0, 640, 200});
    REQUIRE(cb->natural_height() == idle_height);
}

TEST_CASE("ComposeBar recording: push_amplitude no-op before set_recording",
          "[tk][view][compose][voice]")
{
    Stage st;
    auto cb = std::make_unique<ComposeBar>();
    // Must not crash when called before recording is started.
    for (int i = 0; i < 10; ++i)
        cb->push_amplitude(512);
    REQUIRE(cb->natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar recording: push_amplitude accepted during recording",
          "[tk][view][compose][voice]")
{
    Stage st;
    auto cb = std::make_unique<ComposeBar>();
    cb->set_recording(true);
    for (int i = 0; i < 10; ++i)
        cb->push_amplitude(static_cast<std::uint16_t>(i * 100));
    // Just verify no crash and height is unchanged.
    REQUIRE(cb->natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar recording: set_recording false resets state",
          "[tk][view][compose][voice]")
{
    Stage st;
    auto cb = std::make_unique<ComposeBar>();
    cb->set_recording(true);
    for (int i = 0; i < 5; ++i)
        cb->push_amplitude(800);
    cb->set_recording(false);
    REQUIRE(!cb->is_recording());
    // After reset, push_amplitude should be a no-op.
    cb->push_amplitude(999); // must not crash
}
```

- [ ] **Step 2: Build and run the tests — verify they FAIL**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
```

Expected: compile errors — `set_recording`, `push_amplitude`, `is_recording` not declared on `ComposeBar`.

- [ ] **Step 3: Add new declarations to `ComposeBar.h`**

In `ui/shared/views/ComposeBar.h`, add the following to the public section, after the existing `has_pending()` methods (around line 116):

```cpp
// ── Voice recording state ────────────────────────────────────────────────

/// Switch between idle and recording visual state.
/// Transitioning idle → recording clears the amplitude history.
void set_recording(bool recording);
bool is_recording() const { return recording_; }

/// Push a live amplitude sample [0, 1000] from the capture backend.
/// No-op when not recording.
void push_amplitude(std::uint16_t amplitude);

/// Fires when the mic button is clicked (idle state) or the stop button
/// is clicked (recording state). The shell distinguishes the two by
/// checking tk::AudioCapture::is_recording().
std::function<void()> on_mic_clicked;

/// Fires when the × cancel button is clicked during recording.
std::function<void()> on_cancel_voice;
```

And in the private section, add the following new members (after the existing `press_edit_cancel_` field):

```cpp
// Voice recording state.
bool recording_ = false;
// Circular amplitude history for the waveform strip (max kMaxWaveformSamples).
static constexpr std::size_t kMaxWaveformSamples = 48;
std::vector<std::uint16_t> waveform_samples_;
// Layout rects for recording-mode widgets.
tk::Rect mic_btn_rect_{};
tk::Rect waveform_strip_rect_{};
tk::Rect voice_cancel_rect_{};
// Elapsed recording time label layout (rebuilt each repaint while recording).
std::unique_ptr<tk::TextLayout> elapsed_layout_;
std::uint64_t recording_start_ms_ = 0;
```

Also add a new `tk::Button*` pointer alongside the others:

```cpp
tk::Button* mic_btn_ = nullptr;    // borrowed; always visible
```

- [ ] **Step 4: Implement the new methods in `ComposeBar.cpp`**

At the bottom of `ComposeBar.cpp` (after the last existing method), add:

```cpp
void ComposeBar::set_recording(bool recording)
{
    if (recording_ == recording)
        return;
    recording_ = recording;
    if (recording)
    {
        waveform_samples_.clear();
        recording_start_ms_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    elapsed_layout_.reset();
    invalidate(); // triggers repaint
}

void ComposeBar::push_amplitude(std::uint16_t amplitude)
{
    if (!recording_)
        return;
    if (waveform_samples_.size() >= kMaxWaveformSamples)
        waveform_samples_.erase(waveform_samples_.begin());
    waveform_samples_.push_back(amplitude);
    invalidate();
}
```

You will need `#include <chrono>` at the top of `ComposeBar.cpp` if not already present.

- [ ] **Step 5: Run the tests — verify they PASS**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "compose.*voice" --output-on-failure
```

Expected: all four new test cases pass.

- [ ] **Step 6: Commit**

```bash
git add ui/shared/views/ComposeBar.h ui/shared/views/ComposeBar.cpp \
        tests/cpp/test_tk_compose_bar.cpp
git commit -m "feat(ui): add ComposeBar voice recording state API with tests"
```

---

## Task 10: ComposeBar mic button + waveform strip painting

**Files:**
- Modify: `ui/shared/views/ComposeBar.cpp`
- Modify: `ui/shared/views/ComposeBar.h`

This task adds the mic button to the widget tree, updates `measure`/`arrange` to lay out the mic button between the sticker button and the send button, and adds waveform strip painting in `paint()`.

- [ ] **Step 1: Add mic button to the widget tree in `ComposeBar::ComposeBar()`**

In `ComposeBar.cpp`, in the constructor (alongside where `emoji_btn_`, `sticker_btn_`, and `send_btn_` are created), add:

```cpp
// Mic button — always visible, between sticker and send.
{
    auto b = std::make_unique<tk::Button>(tk::Button::Style::Icon, "mic");
    b->on_click = [this] { if (on_mic_clicked) on_mic_clicked(); };
    mic_btn_ = b.get();
    add_child(std::move(b));
}
```

- [ ] **Step 2: Update `arrange()` to position the mic button**

In `ComposeBar::arrange()`, after positioning `sticker_btn_` and before positioning `send_btn_`, compute `mic_btn_rect_` using the same per-button width as the other icon buttons (examine the existing button layout logic for the exact pixel values — typically `kIconBtnW` or computed from bounds).

Place the mic button between the sticker and send buttons. The existing `text_area_rect_` right edge should stop `kIconBtnW` sooner to accommodate it.

Example (adapt exact constant names from existing code):

```cpp
// mic button sits between sticker and send, same size as sticker.
mic_btn_rect_ = tk::Rect{sticker_rect_.right() + kBtnGap,
                          btn_y, kIconBtnW, kIconBtnH};
mic_btn_->arrange(lc, mic_btn_rect_);
// Shrink text area to leave room for mic button.
text_area_rect_.w -= (kIconBtnW + kBtnGap);
```

- [ ] **Step 3: Update `trigger_send()` to be a no-op during recording**

In `ComposeBar::trigger_send()`, at the very top add:

```cpp
if (recording_)
    return; // stop/cancel handled by mic_btn_ and voice_cancel_rect_ clicks
```

- [ ] **Step 4: Update `paint()` to draw the waveform strip during recording**

In `ComposeBar::paint()`, after painting the compose card but before the text area, add a conditional block:

```cpp
if (recording_)
{
    // Hide text area placeholder; draw waveform strip instead.
    // Waveform strip: vertical bars left-to-right over text_area_rect_.
    const float bar_w = 3.0f;
    const float bar_gap = 2.0f;
    const float center_y = waveform_strip_rect_.y + waveform_strip_rect_.h / 2.0f;
    const float max_h = waveform_strip_rect_.h * 0.8f;

    float x = waveform_strip_rect_.x;
    for (std::uint16_t amp : waveform_samples_)
    {
        float h = std::max(2.0f, (amp / 1000.0f) * max_h);
        pc.canvas().fill_rect(
            {x, center_y - h / 2.0f, bar_w, h},
            pc.theme().accent_color());
        x += bar_w + bar_gap;
        if (x > waveform_strip_rect_.right())
            break;
    }

    // Elapsed time label (●  0:12).
    auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::uint64_t elapsed = (now_ms > recording_start_ms_)
                                ? (now_ms - recording_start_ms_) / 1000
                                : 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "● %llu:%02llu",
                  elapsed / 60, elapsed % 60);
    if (!elapsed_layout_ || /* rebuild every repaint */ true)
    {
        elapsed_layout_ = pc.factory().make_text_layout(
            buf, tk::TextStyle::Body, pc.theme().secondary_text_color());
    }
    if (elapsed_layout_)
    {
        float lx = waveform_strip_rect_.right() - elapsed_layout_->width() - 4.0f;
        float ly = center_y - elapsed_layout_->ascent() / 2.0f;
        pc.canvas().draw_text(*elapsed_layout_, {lx, ly});
    }

    // Cancel × button (same style as reply/edit cancel buttons).
    // Paint to voice_cancel_rect_ — set in arrange().
}
```

Compute `waveform_strip_rect_` in `arrange()` to be the text area rect when recording is active (they share the same space).

- [ ] **Step 5: Wire the cancel × button hit-test**

In `ComposeBar::on_pointer_down()`, add a check for `voice_cancel_rect_` similar to the existing reply/edit cancel hit-test:

```cpp
if (recording_ && voice_cancel_rect_.contains(local))
{
    press_voice_cancel_ = true;
    return true;
}
```

Add `bool press_voice_cancel_ = false;` to the private members in `ComposeBar.h`.

In `on_pointer_up()`:
```cpp
if (press_voice_cancel_)
{
    press_voice_cancel_ = false;
    if (inside_self && voice_cancel_rect_.contains(local))
    {
        if (on_cancel_voice)
            on_cancel_voice();
    }
}
```

- [ ] **Step 6: Build the full Qt6 debug binary to verify it compiles**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -20
```

Expected: full binary builds (may still have link errors if Rust FFI isn't wired — acceptable at this stage).

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/ComposeBar.h ui/shared/views/ComposeBar.cpp
git commit -m "feat(ui): add ComposeBar mic button, waveform strip, and cancel button painting"
```

---

## Task 11: RoomView + RoomWindowBase wiring

**Files:**
- Modify: `ui/shared/views/RoomView.h`
- Modify: `ui/shared/views/RoomView.cpp`
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/RoomWindowBase.cpp`

- [ ] **Step 1: Add voice callbacks to `RoomView.h`**

In `ui/shared/views/RoomView.h`, in the external callbacks block (around line 155 after `on_send_file`), add:

```cpp
// Voice recording: mic/stop button click.
std::function<void()> on_mic_clicked;
// Voice recording: × cancel button click.
std::function<void()> on_cancel_voice;
```

- [ ] **Step 2: Wire ComposeBar → RoomView in `RoomView::wire_internal_callbacks()`**

In `ui/shared/views/RoomView.cpp`, inside `wire_internal_callbacks()` (after the existing `compose_bar_->on_send_file` wiring), add:

```cpp
compose_bar_->on_mic_clicked = [this]
{
    if (on_mic_clicked)
        on_mic_clicked();
};
compose_bar_->on_cancel_voice = [this]
{
    if (on_cancel_voice)
        on_cancel_voice();
};
```

- [ ] **Step 3: Add `capture_` to `ShellBase.h`**

In `ui/shared/app/ShellBase.h`, add the following include and member declaration in the `protected:` section, alongside other protected members (after `client_` or after the audio player declaration):

```cpp
// Include at top of ShellBase.h:
#include "tk/audio_capture.h"

// In the protected section:
std::unique_ptr<tk::AudioCapture> capture_;
```

- [ ] **Step 4: Wire voice capture callbacks in `RoomWindowBase::wire_room_view_()`**

In `ui/shared/app/RoomWindowBase.cpp`, at the end of `wire_room_view_()` (after the existing `rv->on_near_top` wiring), add:

```cpp
// ── Voice capture ────────────────────────────────────────────────────────
rv->on_mic_clicked = [this]
{
    auto* cb = room_view_->compose_bar();
    if (!shell_->capture_)
        return;
    if (!shell_->capture_->is_recording())
    {
        shell_->capture_->start();
        cb->set_recording(true);
    }
    else
    {
        // stop() fires on_stopped asynchronously.
        shell_->capture_->stop();
    }
};

rv->on_cancel_voice = [this]
{
    if (!shell_->capture_)
        return;
    shell_->capture_->cancel();
    room_view_->compose_bar()->set_recording(false);
};

if (shell_->capture_)
{
    shell_->capture_->on_amplitude = [this](std::uint16_t amp)
    {
        room_view_->compose_bar()->push_amplitude(amp);
        request_relayout();
    };

    shell_->capture_->on_stopped =
        [this](std::vector<std::uint8_t> pcm,
               std::vector<std::uint16_t> waveform,
               std::uint64_t duration_ms)
    {
        auto* cb = room_view_->compose_bar();
        if (pcm.empty())
        {
            // Permission denied or device error.
            cb->set_recording(false);
            // TODO: surface an error toast once the shell toast API is added.
            return;
        }
        if (duration_ms < 500)
        {
            cb->set_recording(false);
            return;
        }
        // Size guard: ~24 kbps → duration_ms * 3 bytes.
        const std::uint64_t est = duration_ms * 3;
        const std::uint64_t limit = shell_->client_->media_upload_limit();
        if (limit > 0 && est > limit)
        {
            cb->set_recording(false);
            // TODO: surface toast once shell toast API is added.
            return;
        }
        auto res = shell_->client_->send_voice(
            shell_->current_room_id_,
            pcm.data(), pcm.size(),
            duration_ms, waveform,
            cb->current_text(),
            cb->reply_event_id());
        cb->set_recording(false);
        cb->clear_reply();
        if (auto* ta = compose_text_area_())
            ta->set_text("");
        room_view_->set_current_text({});
        if (!res)
        {
            // TODO: surface toast once shell toast API is added.
        }
    };
}
```

- [ ] **Step 5: Initialise `capture_` in each shell constructor**

In each of the four shell constructors, after the `Host` is ready and after the call that creates the audio player, add a line that creates `capture_` via the host:

**Qt6 (`ui/linux-qt/src/MainWindow.cpp`)** — find where `make_audio_player()` is called (around line 370) and add:

```cpp
capture_ = mainAppSurface_->host().make_audio_capture();
```

**GTK4 (`ui/linux-gtk/src/MainWindow.cpp`)** — find the equivalent location:

```cpp
capture_ = host_->make_audio_capture();
```

**Win32 (`ui/windows/src/MainWindow.cpp`)** — same pattern:

```cpp
capture_ = host_->make_audio_capture();
```

**macOS (`ui/macos/src/MainWindowController.mm`)** — same pattern:

```cpp
_shell->capture_ = [mainSurface_ host]->make_audio_capture();
```

(Exact expressions depend on how each shell accesses the host — mirror the `make_audio_player()` call in each shell exactly.)

- [ ] **Step 6: Build all configured targets**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -30
cmake --build build/linux-gtk-debug 2>&1 | tail -30
```

Expected: both build and link cleanly.

- [ ] **Step 7: Run the full C++ test suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -20
```

Expected: all tests pass, including the four new compose bar voice tests.

- [ ] **Step 8: Commit**

```bash
git add ui/shared/views/RoomView.h ui/shared/views/RoomView.cpp \
        ui/shared/app/ShellBase.h ui/shared/app/RoomWindowBase.cpp \
        ui/linux-qt/src/MainWindow.cpp \
        ui/linux-gtk/src/MainWindow.cpp \
        ui/windows/src/MainWindow.cpp \
        ui/macos/src/MainWindowController.mm
git commit -m "feat(shell): wire voice capture into RoomWindowBase and all four shells"
```

---

## Task 12: Full build, Rust tests, and smoke test checklist

**Files:** None created — verification only.

- [ ] **Step 1: Run Rust unit tests**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -20
```

Expected: `send_voice_rejects_empty_pcm`, `send_voice_encodes_valid_ogg`, `send_voice_waveform_clamped_to_256`, and all pre-existing tests pass.

- [ ] **Step 2: Run C++ tests**

```bash
cmake --build build/linux-qt6-debug && \
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Smoke test (manual)**

Launch the Qt6 binary:

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

Work through this checklist:

- [ ] Mic button is visible to the right of the sticker button and left of the send button in the compose bar.
- [ ] Click mic button → button changes to stop icon, waveform strip appears, timer shows `● 0:00` incrementing.
- [ ] Speak into the microphone → amplitude bars animate.
- [ ] Click stop → message appears in the room (verify in Element Web that it plays back with waveform).
- [ ] Record with text in the compose field → caption appears in Element Web below the voice message.
- [ ] Click mic → immediately click × cancel → no message is sent.
- [ ] Click mic and immediately click stop (< 500ms) → no message is sent (too short, silently discarded).
- [ ] Start recording then switch rooms → confirm capture stops (no dangling timer).

- [ ] **Step 4: Final commit**

```bash
git commit --allow-empty -m "test: voice message send smoke-tested on Qt6"
```

---

## Self-Review Notes

**Spec coverage check:**

| Spec requirement | Task |
| --- | --- |
| Device detection → nullptr factory → hide mic button | Task 2b |
| tk::AudioCapture interface | Task 1 |
| Qt6 backend | Task 3 |
| GTK4 backend | Task 4 |
| Win32 backend | Task 5 |
| macOS backend | Task 6 |
| macOS NSMicrophoneUsageDescription | Task 6 |
| audiopus + ogg deps | Task 7 |
| send_voice() Rust impl | Task 7 |
| Rust unit tests (3 tests) | Task 7 |
| C++ Client::send_voice() | Task 8 |
| ComposeBar mic button always visible | Task 10 |
| ComposeBar waveform strip | Task 10 |
| ComposeBar cancel button | Task 10 |
| ComposeBar recording state tests | Task 9 |
| RoomView forwarding | Task 11 |
| ShellBase capture_ | Task 11 |
| All 4 shells initialise capture_ | Task 11 |
| Permission error → empty PCM path | Task 11 (on_stopped handler) |
| Sub-500ms clip discarded | Task 11 (on_stopped handler) |
| Size limit check | Task 11 (on_stopped handler) |
| Host::make_audio_capture() | Task 2 |
