# Voice Message Sending — Design Spec

**Date:** 2026-05-18  
**Status:** Approved  
**Scope:** All four platforms (Qt6, GTK4, Win32, macOS)

---

## Overview

Implement sending MSC3245 voice messages from Tesseract. Playback of incoming voice messages is already complete. This spec covers the record → encode → upload → send path.

User interaction: click mic button to start recording, click stop button to send, click × to cancel. The mic button is always visible alongside the send button, allowing an optional text caption.

---

## Architecture

C++ owns mic I/O; Rust owns encoding + network. The stack:

```text
ComposeBar (mic/stop button click)
    ↓ starts/stops
tk::AudioCapture  [ui/shared/tk/audio_capture.h]
    ├── audio_capture_qt.cpp     (QAudioSource, 48kHz/16-bit/mono PCM)
    ├── audio_capture_gtk.cpp    (GStreamer: pulsesrc → audioconvert → audioresample → appsink)
    ├── audio_capture_win32.cpp  (WASAPI IAudioCaptureClient)
    └── audio_capture_macos.mm   (AVAudioEngine + AVAudioInputNode)
    on_amplitude callback (~10/sec) → ComposeBar live waveform bars
    on_stopped callback → PCM buffer + waveform samples + duration_ms

Shell (on_send_voice / on_stopped)
    ↓ passes PCM + waveform + duration + caption + reply_event_id
Client::send_voice()  [C++ wrapper in client/include/tesseract/client.h]
    ↓ FFI
sdk::send_voice()  [Rust, sdk/src/client.rs + sdk/src/bridge.rs]
    audiopus  → Opus encode (48kHz mono, Voip mode, ~24 kbps)
    ogg       → OGG mux (OpusHead + OpusTags + audio pages)
    room.send_attachment() → upload + post m.audio + MSC3245 voice marker + MSC1767 waveform
```

---

## Section 1 — `tk::AudioCapture` Interface

**New file:** `ui/shared/tk/audio_capture.h`

```cpp
namespace tk {

class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    virtual void start() = 0;   // begin capture; no-op if already recording
    virtual void stop() = 0;    // stop and fire on_stopped with full PCM
    virtual void cancel() = 0;  // stop and discard; on_stopped NOT fired

    virtual bool is_recording() const = 0;
    virtual std::uint64_t duration_ms() const = 0;

    // Fired ~10x/sec on the UI thread. amplitude is in [0, 1000].
    std::function<void(std::uint16_t amplitude)> on_amplitude;

    // Fired on the UI thread after stop() completes.
    // pcm: 48kHz / 16-bit signed LE / mono
    // waveform: amplitude samples collected during recording, [0, 1000] each
    std::function<void(std::vector<std::uint8_t> pcm,
                       std::vector<std::uint16_t> waveform,
                       std::uint64_t duration_ms)> on_stopped;
};

} // namespace tk
```

All backends normalise to **48 kHz / 16-bit signed / mono** before invoking `on_stopped`. Amplitude sampling is performed inside each backend at ~100 ms windows (max absolute sample value, normalised to [0, 1000]).

### Platform backends

| File | Backend |
| --- | --- |
| `ui/shared/tk/audio_capture_qt.cpp` | `QAudioSource` (Qt Multimedia — already linked) |
| `ui/shared/tk/audio_capture_gtk.cpp` | GStreamer `pulsesrc ! audioconvert ! audioresample ! appsink` (gst-plugins-base already linked) |
| `ui/shared/tk/audio_capture_win32.cpp` | WASAPI `IAudioCaptureClient` (no new system dep) |
| `ui/shared/tk/audio_capture_macos.mm` | `AVAudioEngine` + `AVAudioInputNode` (AVFoundation already linked) |

Each platform file also defines its factory function:

```cpp
// audio_capture.h declares:
std::unique_ptr<tk::AudioCapture> make_audio_capture_qt(tk::Host*);
std::unique_ptr<tk::AudioCapture> make_audio_capture_gtk(tk::Host*);
std::unique_ptr<tk::AudioCapture> make_audio_capture_win32(tk::Host*);
std::unique_ptr<tk::AudioCapture> make_audio_capture_macos(tk::Host*);
```

`tk::Host*` is passed so each backend can marshal `on_amplitude` and `on_stopped` to the UI thread via `host->post_to_ui()`, mirroring the existing `AudioPlayer` pattern.

---

## Section 2 — Rust Encoding + FFI

### New Cargo dependencies (`sdk/Cargo.toml`)

```toml
audiopus = "0.3"   # safe bindings to libopus; static on Win/macOS, dynamic on Linux
ogg = "0.9"        # pure-Rust OGG muxer; no C deps
```

### FFI declaration (`sdk/src/bridge.rs`)

```rust
/// Upload `pcm` (48kHz/16-bit/mono LE) as an MSC3245 voice message to `room_id`.
/// Encodes to Opus/OGG in-process, uploads via the homeserver media repository,
/// and posts an `m.audio` event tagged with `org.matrix.msc3245.voice`.
/// `waveform` carries the amplitude samples for the MSC1767 `audio.waveform` field
/// (clamped to 256 entries). `caption` follows MSC2530 framing when non-empty.
/// `reply_event_id` adds `m.in_reply_to` when non-empty.
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

### Rust implementation (`sdk/src/client.rs`)

Steps inside `send_voice()`:

1. Cast `pcm: &[u8]` → `&[i16]` (assert even length; return `err` if odd).
2. Create `audiopus::Encoder::new(48000, Channels::Mono, Application::Voip)`.
3. Encode in 960-sample frames (20 ms at 48 kHz) → collect Opus packets.
4. Mux into OGG via `ogg::PacketWriter`:
   - Page 0: `OpusHead` (magic, version=1, channel_count=1, pre_skip, input_sample_rate=48000, output_gain=0, channel_mapping_family=0).
   - Page 1: `OpusTags` (magic + vendor string + zero user comments).
   - Remaining pages: one Opus packet per OGG page (granule position = cumulative sample count).
5. Build `AudioMessageEventContent` with:
   - `body` = `"voice-message.ogg"` (or caption as body + filename field when non-empty, MSC2530).
   - `info.duration` = `UInt::new(duration_ms)`.
   - `audio` = MSC1767 `AudioContent { duration: duration_ms, waveform: waveform[..256] }`.
   - `voice` = `Some(VoiceEventContent {})` (MSC3245 marker).
6. Upload via `room.send_attachment("voice-message.ogg", &mime::APPLICATION_OGG, ogg_bytes, config)` where config carries `AttachmentInfo::Audio(BaseAudioInfo { duration, size })`.
7. Apply `reply_event_id` relation identically to `send_image`.

### C++ client wrapper (`client/include/tesseract/client.h`)

```cpp
/// Encode `pcm` (48kHz/16-bit/mono LE, `pcm_size` bytes) as an Opus/OGG
/// voice message and send it to `room_id`. `waveform` populates the MSC1767
/// waveform field. `caption` and `reply_event_id` follow the same semantics
/// as send_image.
Result send_voice(const std::string& room_id,
                  const std::uint8_t* pcm, std::size_t pcm_size,
                  std::uint64_t duration_ms,
                  const std::vector<std::uint16_t>& waveform,
                  const std::string& caption,
                  const std::string& reply_event_id);
```

---

## Section 3 — ComposeBar UI Changes

### Visual states

**Idle** (no recording):

- Mic button: microphone icon, same style as emoji/sticker buttons. Positioned between sticker button and send button. Always visible.

**Recording**:

- Mic button becomes a stop button (■, red tint).
- A `×` cancel button appears to the left of the waveform strip.
- The `NativeTextArea` overlay is hidden; a live waveform strip occupies the text area rect. The strip shows the last N amplitude bars scrolling right-to-left, plus an elapsed time label (`● 0:12`).
- Caption text typed before recording started is preserved and sent with the clip.
- `natural_height()` does not change — the waveform strip fits within the existing compose bar height.

**On stop click:** fires `on_send_voice` immediately (no preview step) and transitions back to idle.

### New ComposeBar API

```cpp
/// Switch between idle and recording visual state. Clears amplitude
/// history when transitioning idle → recording.
void set_recording(bool recording);

/// Push an amplitude sample [0, 1000] into the live waveform strip.
/// No-op when not recording.
void push_amplitude(std::uint16_t amplitude);

/// Fires when the mic button is clicked (idle state) OR the stop
/// button is clicked (recording state). Shell uses is_recording state
/// of tk::AudioCapture to decide whether to start or stop.
std::function<void()> on_mic_clicked;

/// Fires when the × cancel button is clicked during recording.
std::function<void()> on_cancel_voice;
```

`trigger_send()` is extended to check for recording state: if recording, it is a no-op (the stop button is a separate widget path, not the send button).

---

## Section 4 — Shell Wiring

`capture_` lives on `ShellBase` as `std::unique_ptr<tk::AudioCapture>`, alongside other shared state. Each shell's constructor calls the platform factory after the Host is ready.

```cpp
// ShellBase.h addition:
std::unique_ptr<tk::AudioCapture> capture_;
```

Callback wiring (identical across all four shells, implemented once in `ShellBase` or a shared helper):

```cpp
// Amplitude → waveform animation
capture_->on_amplitude = [this](uint16_t amp) {
    room_view()->compose_bar()->push_amplitude(amp);
    relayout();
};

// PCM ready → encode + send
capture_->on_stopped = [this](std::vector<uint8_t> pcm,
                               std::vector<uint16_t> waveform,
                               uint64_t duration_ms) {
    auto* cb = room_view()->compose_bar();
    if (duration_ms < 500) {          // discard sub-500ms clips silently
        cb->set_recording(false);
        return;
    }
    // Estimated OGG size check before encoding
    uint64_t est_bytes = duration_ms * 3;  // ~24 kbps
    if (est_bytes > client_->media_upload_limit() && client_->media_upload_limit() != 0) {
        cb->set_recording(false);
        show_error_toast("Voice message too long for this server.");
        return;
    }
    auto res = client_->send_voice(current_room_id_,
                                   pcm.data(), pcm.size(),
                                   duration_ms, waveform,
                                   cb->current_text(),
                                   cb->reply_event_id());
    cb->set_recording(false);
    cb->clear_reply();
    cb->set_current_text({});
    if (!res.ok) show_error_toast(res.error);
};

// Mic/stop button click
compose_bar->on_mic_clicked = [this]() {
    if (!capture_->is_recording()) {
        capture_->start();
        room_view()->compose_bar()->set_recording(true);
    } else {
        capture_->stop();   // fires on_stopped asynchronously
    }
};

// Cancel button
compose_bar->on_cancel_voice = [this]() {
    capture_->cancel();
    room_view()->compose_bar()->set_recording(false);
};
```

---

## Section 5 — Error Handling & Permissions

**Mic permission denied (macOS):** Call `AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio` synchronously before `start()`. If denied, `start()` returns immediately without setting `is_recording()` to true, and calls `on_stopped` with empty PCM so the shell receives a uniform signal. Shell detects empty PCM and shows toast: *"Microphone access denied. Check System Settings → Privacy."*

**Mic permission denied (Win32):** WASAPI `IAudioClient::Initialize` returns an access-denied HRESULT → `start()` likewise does not set `is_recording()` and fires `on_stopped` with empty PCM → same toast path.

**Qt6/GTK4 (Linux):** PipeWire/PulseAudio portal handles permissions at the OS level. If `QAudioSource` or the GStreamer pipeline fails to open, `start()` fires `on_stopped` with empty PCM.

**Empty PCM signal (all platforms):** `on_stopped` with zero-length `pcm` → `send_voice` is skipped. If the ComposeBar is in recording state (set_recording was called before start() returned), show a permission error toast and call `set_recording(false)`. If start() failed before set_recording was called (permission check fires synchronously), the shell detects empty PCM in `on_stopped` and shows the toast without touching compose bar state.

**Upload failure:** `send_voice()` returns `OpResult{ok=false}` → same error toast path as `send_image`/`send_file`. Clip discarded, no retry.

**Sub-500ms clip:** Silently discarded in the `on_stopped` handler before `send_voice` is called.

**Size limit exceeded:** Estimated OGG size (`duration_ms * 3` bytes) checked against `media_upload_limit()` before encoding. Toast: *"Voice message too long for this server."*

---

## Section 6 — Testing

### Rust unit tests (`sdk/src/client.rs`, `#[cfg(test)]`)

| Test | Assertion |
| --- | --- |
| `send_voice_encodes_valid_ogg` | Feed 1s of 440Hz 48kHz/16-bit sine; assert output starts with `OggS` magic and is non-empty. |
| `send_voice_waveform_clamped_to_256` | Pass 512 amplitude samples; assert MSC1767 waveform field has ≤ 256 entries. |
| `send_voice_rejects_empty_pcm` | Pass zero-length PCM; assert `OpResult::ok == false`. |
| `send_voice_stub_not_logged_in` | `#[cfg(test)]` stub returns `err("not logged in")` — keeps test build linking cleanly. |

### C++ / ctest

| Test | Assertion |
| --- | --- |
| `test_audio_capture_interface` | Instantiate platform backend, `start()` + `stop()` immediately, assert `on_stopped` fires within 500ms with non-null PCM. Skipped if `TESSERACT_SKIP_AUDIO_CAPTURE_TESTS=1`. |
| `test_compose_bar_recording_state` | Call `set_recording(true)` → assert `natural_height()` unchanged; `push_amplitude(512)` × 10 → no crash; `set_recording(false)` → state resets. |

### Manual smoke checklist

- Record a voice message in an E2EE room; verify Element Web plays it back with waveform.
- Record with a text caption; verify caption appears in Element Web.
- Deny mic permission on macOS; verify toast appears without crash.
- Record exactly at the server upload limit; verify size-check toast fires.
- Cancel a recording in progress; verify no message is sent.
