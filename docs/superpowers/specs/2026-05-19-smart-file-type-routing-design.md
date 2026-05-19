# Smart File-Type Routing on Drop

**Date:** 2026-05-19
**Status:** Approved for implementation

## Context

When a user drops a file on any Tesseract window, the app currently routes it one of two ways: `image/*` goes to `set_pending_image()`, everything else goes to `set_pending_file()`. This means video files are sent as `m.file`, audio files are sent as `m.file`, and animated GIFs/WebPs are sent as plain `m.image` with no animation markers. The Matrix spec (and Element, Cinny, FluffyChat, etc.) distinguish `m.image`, `m.video`, and `m.audio` and render them differently. This feature adds full MIME-aware routing end-to-end.

## Scope

- Route dropped files to `m.image`, `m.video`, `m.audio`, or `m.file` based on MIME type.
- For animated GIF and WebP: detect animation via platform APIs and set `org.matrix.msc4230.is_animated` and `fi.mau.video.gif` flags on `m.image` events.
- For video: generate a thumbnail (first frame, JPEG) and include it in the `m.video` event; also extract video dimensions and duration.
- For audio: send as plain `m.audio` with duration. No MSC3245 voice extension (that is reserved for voice recordings only).
- Compose-bar preview: video shows thumbnail with a video-camera icon overlay; audio shows a chip with icon + filename + duration.
- Extraction runs on a background thread; a spinner is shown while it runs.

**Out of scope:** Re-encoding video/audio, generating waveforms for plain audio, changing the voice-message path.

## Architecture

Seven layers are touched, all extending existing code:

```
Drop event (4 platforms)
  → MIME routing in MainWindow::set_on_file_drop
    → extract_media_info_() per platform [NEW, background thread]
      → ComposeBar pending state (new Video + Audio kinds)
        → on_send_video / on_send_audio callbacks [NEW]
          → MainWindow wiring (4 platforms)
            → client::send_video / send_audio [NEW]
              → Rust FFI bridge (new functions in bridge.rs)
                → sdk/src/client.rs Rust implementation
```

## Layer 1: Drop Routing (4 MainWindows)

Current code in every shell's `set_on_file_drop` callback:
```cpp
if (mime.starts_with("image/")) set_pending_image(...)
else set_pending_file(...)
```

New routing:

| MIME | Action |
|------|--------|
| `image/gif`, `image/webp` | `set_pending_image` (loading state), background detects frame count → `update_pending_attachment(is_animated)` |
| other `image/*` | `set_pending_image` directly, `is_animated = false` |
| `video/*` | `set_pending_video` (loading state), background extracts thumbnail + dimensions + duration |
| `audio/*` | `set_pending_audio` (loading state), background extracts duration |
| other | `set_pending_file` directly (unchanged) |

For the loading state, bytes and filename are set immediately; thumbnail/duration/is_animated are filled in after extraction.

## Layer 2: Per-Platform Media Extraction

New function `extract_media_info_()` per shell. Returns:

```cpp
struct MediaInfo {
    std::vector<uint8_t> thumb_bytes;  // JPEG; empty for audio/gif
    uint32_t thumb_w = 0, thumb_h = 0;
    uint32_t video_w = 0, video_h = 0;
    uint64_t duration_ms = 0;
    bool is_animated = false;
};
```

Runs on a `std::thread`. On completion posts to UI thread via `post_to_ui_()` and calls `compose_bar()->update_pending_attachment(info)`.

### Platform implementations

| Platform | Animated images | Video thumbnail | Duration |
|----------|----------------|-----------------|----------|
| Qt6 | `QImageReader::imageCount() > 1` | `QMediaPlayer` + `QVideoSink::videoFrameChanged`, grab frame 0 | `QMediaPlayer::duration()` after load |
| GTK4 | `gdk_pixbuf_animation_is_static_image()` returns FALSE | GStreamer `filesrc → decodebin → appsink` (PREROLL, single frame) | `GST_FORMAT_TIME` duration query |
| Win32 | `IWICBitmapDecoder::GetFrameCount() > 1` | `IMFSourceReader::ReadSample()` from in-memory stream | `IMF_PD_DURATION` attribute |
| macOS | `CGImageSourceGetCount() > 1` | `AVAssetImageGenerator` on local temp URL | `AVAsset.duration` |

For platforms that require a file path (macOS AVFoundation), bytes are written to a temp file and deleted after extraction. The existing `generate_video_thumbnail_()` works with MXC URLs (receive-side); this is a new companion for raw bytes.

## Layer 3: ComposeBar

### PendingAttachment struct

```cpp
struct PendingAttachment {
    enum class Kind { Image, Video, Audio, File };
    Kind kind = Kind::Image;
    bool loading = false;

    std::vector<uint8_t> bytes;
    std::string mime;
    std::string filename;

    std::unique_ptr<tk::Image> preview;
    uint32_t width = 0, height = 0;

    bool is_animated = false;          // gif/webp; resolved after background check

    uint32_t thumb_width = 0, thumb_height = 0;  // video thumbnail dimensions
    std::vector<uint8_t> thumb_bytes_raw;        // raw JPEG bytes passed to on_send_video

    uint64_t duration_ms = 0;          // video + audio
};
```

### New/updated API

```cpp
void set_pending_image(bytes, mime, filename, is_animated = false);
void set_pending_video(bytes, mime, filename);   // loading=true
void set_pending_audio(bytes, mime, filename);   // loading=true
void update_pending_attachment(const MediaInfo&); // called on UI thread after extraction

// Updated callback — adds is_animated
on_send_image(bytes, mime, filename, caption, width, height, is_animated, reply_id)

// New callbacks
on_send_video(bytes, mime, filename, caption, width, height,
              thumb_bytes, thumb_width, thumb_height, duration_ms, reply_id)
on_send_audio(bytes, mime, filename, caption, duration_ms, reply_id)
```

### Preview rendering

- **Video loading**: plain chip (film icon + filename + size).
- **Video with thumbnail**: 96 px thumbnail band (same as image), with a camera icon badge in the bottom-right corner.
- **Audio loading**: chip with audio icon + filename.
- **Audio resolved**: chip with audio icon + filename + formatted duration (`0:42`).
- **GIF/WebP loading**: first frame shown immediately (decoded by `set_pending_image`); small spinner badge until animation status resolves.

## Layer 4: RoomView Wiring

`ui/shared/views/RoomView.cpp` forwards `on_send_video` and `on_send_audio` from ComposeBar to the shell, following the existing `on_send_image` / `on_send_file` pattern.

## Layer 5: C++ Client API

File: `client/include/tesseract/client.h`

```cpp
// Updated — adds is_animated
Result send_image(const std::string& room_id,
                  const std::vector<uint8_t>& bytes,
                  const std::string& mime_type,
                  const std::string& filename,
                  const std::string& caption,
                  uint32_t width, uint32_t height,
                  bool is_animated,
                  const std::string& reply_event_id = "");

// New
Result send_video(const std::string& room_id,
                  const std::vector<uint8_t>& bytes,
                  const std::string& mime_type,
                  const std::string& filename,
                  const std::string& caption,
                  uint32_t width, uint32_t height,
                  const std::vector<uint8_t>& thumb_bytes,
                  uint32_t thumb_width, uint32_t thumb_height,
                  uint64_t duration_ms,
                  const std::string& reply_event_id = "");

Result send_audio(const std::string& room_id,
                  const std::vector<uint8_t>& bytes,
                  const std::string& mime_type,
                  const std::string& filename,
                  const std::string& caption,
                  uint64_t duration_ms,
                  const std::string& reply_event_id = "");
```

## Layer 6: Rust FFI Bridge

File: `sdk/src/bridge.rs` — add to the `extern "Rust"` block:

```rust
fn send_image(
    self: &mut ClientFfi, room_id: &str,
    bytes: &[u8], mime_type: &str, filename: &str, caption: &str,
    width: u32, height: u32,
    is_animated: bool,
    reply_event_id: &str,
) -> OpResult;

fn send_video(
    self: &mut ClientFfi, room_id: &str,
    bytes: &[u8], mime_type: &str, filename: &str, caption: &str,
    width: u32, height: u32,
    thumb_bytes: &[u8], thumb_width: u32, thumb_height: u32,
    duration_ms: u64,
    reply_event_id: &str,
) -> OpResult;

fn send_audio(
    self: &mut ClientFfi, room_id: &str,
    bytes: &[u8], mime_type: &str, filename: &str, caption: &str,
    duration_ms: u64,
    reply_event_id: &str,
) -> OpResult;
```

## Layer 7: Rust Implementation

File: `sdk/src/client.rs`

### send_video

Uses `AttachmentInfo::Video(BaseVideoInfo { duration, width, height, size, .. })` and `AttachmentConfig::thumbnail(AttachmentThumbnail { data: thumb_bytes, height: thumb_height, width: thumb_width, content_type: mime::IMAGE_JPEG })`. matrix-sdk uploads the thumbnail and sets `info.thumbnail_url` automatically.

### send_audio

Uses `AttachmentInfo::Audio(BaseAudioInfo { duration, size })`. No MSC3245 markers, no waveform. Produces a plain `m.audio` event (not `m.voice`).

### send_image with is_animated

When `is_animated = false`: unchanged — use `send_attachment()` as today.

When `is_animated = true`:
1. Upload bytes via `client.media().upload(&mime, bytes)` → get MXC URI.
2. Assemble event content as `serde_json::Value` with all standard `m.image` fields plus:
   - Top-level: `"org.matrix.msc4230.is_animated": true`
   - Inside `info`: `"fi.mau.video.gif": true`
3. Add `m.in_reply_to` relation to the JSON when `reply_event_id` is non-empty.
4. Send via `room.send_raw(&content_json, "m.room.message", None)` (matrix-sdk raw-send API).

## Files to Modify

| File | Change |
|------|--------|
| `ui/linux-qt/src/MainWindow.cpp` / `.h` | MIME routing, `extract_media_info_()`, `on_send_video`/`on_send_audio` wiring |
| `ui/linux-gtk/src/MainWindow.cpp` / `.h` | Same |
| `ui/windows/src/MainWindow.cpp` / `.h` | Same |
| `ui/macos/src/MainWindowController.mm` | Same |
| `ui/shared/views/ComposeBar.h` / `.cpp` | New `Kind::Video`/`Audio`, `loading` flag, `update_pending_attachment()`, new callbacks, preview rendering |
| `ui/shared/views/RoomView.cpp` | Forward new callbacks |
| `client/include/tesseract/client.h` | `send_image` updated, `send_video`/`send_audio` added |
| `client/src/client.cpp` | Implement new Pimpl forwarders |
| `sdk/src/bridge.rs` | New bridge functions, updated `send_image` signature |
| `sdk/src/client.rs` | `send_video`, `send_audio`, animated `send_image` path |

## Verification

1. **Rust unit tests** (`cargo test -p tesseract-sdk-ffi`): add tests for `send_video`, `send_audio`, and the `is_animated` path of `send_image`.
2. **C++ tests** (`ctest --test-dir build/linux-qt6-debug`): add tests for MIME routing decisions and `MediaInfo` field population.
3. **Manual end-to-end** (Qt6 build): drop a `.gif`, `.webp`, `.mp4`, `.mp3`, `.png`, `.pdf` onto the window:
   - GIF/WebP: compose preview shows first frame + spinner badge → resolves; sent event has `org.matrix.msc4230.is_animated` and `fi.mau.video.gif` visible in Element `/devtools`.
   - Video: compose preview shows thumbnail with camera badge; sent event shows in timeline with thumbnail and duration.
   - Audio: compose preview shows chip with duration; sent event renders as audio player in Element (not a voice message).
   - PNG: compose preview unchanged.
   - PDF: still sent as `m.file`.
4. **Second shell** (GTK4 or macOS build): repeat GIF + video + audio drops to confirm per-platform extraction works.
