#include "views/media_drop.h"

#include "views/ComposeBar.h"

namespace tesseract::views
{

FileDropOutcome route_file_drop_to_compose_bar(ComposeBar& cb,
                                               std::vector<std::uint8_t> bytes,
                                               std::string mime, std::string filename,
                                               std::uint64_t upload_limit,
                                               const MediaInfoExtractor& extract)
{
    if (upload_limit > 0 && bytes.size() > upload_limit)
        return FileDropOutcome::TooLarge;
    if (bytes.empty())
        return FileDropOutcome::Empty;

    // For gif/webp/video/audio: queue the attachment first (so set_pending_*
    // bumps pending_gen()), then hand the bytes to the per-shell probe keyed by
    // the fresh generation token.
    const auto probe = [&](std::vector<std::uint8_t> b, std::string m)
    {
        if (extract)
            extract(cb.pending_gen(), std::move(b), std::move(m));
    };

    if (mime == "image/gif" || mime == "image/webp")
    {
        // Show the first frame immediately; detect animation in the background.
        cb.set_pending_image(bytes, mime, filename, /*is_animated=*/false);
        probe(std::move(bytes), std::move(mime));
    }
    else if (mime.rfind("image/", 0) == 0)
    {
        cb.set_pending_image(std::move(bytes), std::move(mime),
                             std::move(filename), /*is_animated=*/false);
    }
    else if (mime.rfind("video/", 0) == 0)
    {
        cb.set_pending_video(bytes, mime, filename);
        probe(std::move(bytes), std::move(mime));
    }
    else if (mime.rfind("audio/", 0) == 0)
    {
        cb.set_pending_audio(bytes, mime, filename);
        probe(std::move(bytes), std::move(mime));
    }
    else
    {
        cb.set_pending_file(std::move(bytes), std::move(mime),
                            std::move(filename));
    }
    return FileDropOutcome::Accepted;
}

} // namespace tesseract::views
