#pragma once

// Shared drag-and-drop ingest. Routes a dropped file's bytes into a
// ComposeBar by MIME type, identically for the main window and every pop-out
// window across all four platform shells. The platform-specific async media
// probe (animation detection / video-frame + duration extraction) stays in
// the shell and is injected here as `extract`, so this layer owns only the
// uniform dispatch + guard logic.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class ComposeBar;

// Outcome of routing one dropped file into a compose bar.
enum class FileDropOutcome
{
    Accepted, // queued as a pending attachment
    Empty,    // zero-byte payload; nothing queued
    TooLarge, // exceeds the server upload limit; nothing queued
};

// Per-shell async media-info extractor. Invoked for gif/webp/video/audio with
// the compose bar's pending generation token (read after the attachment is
// queued) so the eventual update_pending_attachment() can discard stale
// results. Backends decode off the UI thread and post the result back to the
// same compose bar.
using MediaInfoExtractor = std::function<void(std::uint32_t pending_gen,
                                              std::vector<std::uint8_t> bytes,
                                              std::string mime)>;

// Route a dropped file into `cb` by MIME type.
//
//   - empty payload                  → FileDropOutcome::Empty (nothing queued)
//   - size > upload_limit (when > 0) → FileDropOutcome::TooLarge (nothing queued)
//   - image/gif, image/webp          → queue as a still image, then `extract`
//                                       to detect animation
//   - other image/*                  → queue directly (no extract)
//   - video/*, audio/*               → queue a loading chip, then `extract`
//                                       for thumbnail / dimensions / duration
//   - anything else                  → queue as a generic file chip
//
// `extract` may be null (e.g. a reduced pop-out path); animation detection and
// video/audio metadata are then skipped, but the attachment is still queued.
// The caller surfaces any user-facing message for the Empty / TooLarge cases.
FileDropOutcome dispatch_file_drop(ComposeBar& cb,
                                   std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename,
                                   std::uint64_t upload_limit,
                                   const MediaInfoExtractor& extract);

} // namespace tesseract::views
