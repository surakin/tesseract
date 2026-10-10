#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tesseract::views
{

// One entry shown by MediaViewerOverlay: a plain value type with no `client/`
// types (the source/thumbnail are the opaque fetch tokens RoomPane already
// passes around). Built by the factories in media_viewer_items.h.
struct MediaViewerItem
{
    enum class Kind
    {
        Image,
        Video,
        Audio,
        File,
    };

    Kind kind = Kind::Image;
    std::string event_id;
    std::string source;    // full-resolution fetch token (image URL / video source JSON)
    std::string thumbnail; // thumbnail cache key shown while `source` loads (may be empty)
    std::string caption;   // text drawn under an image (may be empty)
    std::string filename;  // suggested save name (may be empty)
    std::string mime_type;
    int width = 0;  // 0 = unknown
    int height = 0; // 0 = unknown
    std::uint64_t duration_ms = 0;
    std::uint64_t file_size = 0;
    std::vector<std::uint16_t> waveform; // audio only, amplitudes 0..=1024

    // fi.mau.* playback hints (video only).
    bool loop = false;
    bool no_audio = false;
    bool hide_controls = false;
};

} // namespace tesseract::views
