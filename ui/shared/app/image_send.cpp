#include "image_send.h"

#include "views/image_animation.h"

namespace tesseract
{

PreparedImage prepare_image_for_send(std::vector<std::uint8_t> bytes, std::string mime,
                                     std::string filename, std::uint32_t width,
                                     std::uint32_t height, bool is_animated_flag, bool recompress,
                                     const EncodeForSendFn& encode, const StrippableFn& strippable)
{
    PreparedImage out;
    if (bytes.empty())
        return out;

    const bool animated = is_animated_flag || is_animated_image(bytes, mime);

    const auto refuse = []
    {
        PreparedImage p;
        p.refused = true;
        return p;
    };

    // The original payload. For stills the host sniffs the true mime and the
    // EXIF-oriented size; for animations (never decoded here) and decode
    // failures the caller's values stand.
    const auto original = [&]() -> PreparedImage
    {
        PreparedImage p;
        p.mime = mime;
        p.filename = filename;
        p.width = width;
        p.height = height;
        p.is_animated = animated;
        if (!animated && encode)
        {
            auto enc = encode(bytes.data(), bytes.size(), /*compress=*/false);
            if (!enc.bytes.empty())
            {
                p.mime = std::move(enc.mime);
                p.width = enc.width;
                p.height = enc.height;
            }
        }
        p.bytes = std::move(bytes);
        return p;
    };

    // Can the SDK vouch for stripping the original's metadata on upload? The
    // check runs the whole strip (this is the UI thread), so it is evaluated
    // only where the original is actually a candidate: never when a smaller
    // recompression wins, which is the common case for photos.
    bool original_ok_known = false;
    bool original_ok_value = true;
    const auto original_ok = [&]
    {
        if (!original_ok_known)
        {
            original_ok_value = !strippable || strippable(bytes, mime);
            original_ok_known = true;
        }
        return original_ok_value;
    };

    // An animation cannot be recompressed without flattening it, so one whose
    // metadata cannot be stripped is not sent at all.
    if (animated)
        return original_ok() ? original() : refuse();

    if (!encode)
        return original_ok() ? original() : refuse();
    if (!recompress && original_ok())
        return original();

    auto enc = encode(bytes.data(), bytes.size(), /*compress=*/true);
    if (enc.bytes.empty())
        return original_ok() ? original() : refuse();
    if (enc.bytes.size() >= bytes.size() && original_ok())
        return original();

    out.mime = std::move(enc.mime);
    out.width = enc.width;
    out.height = enc.height;
    out.filename = std::move(filename);
    if (out.mime == "image/jpeg")
    {
        const auto dot = out.filename.find_last_of('.');
        if (dot != std::string::npos)
            out.filename.erase(dot);
        out.filename += ".jpg";
    }
    out.bytes = std::move(enc.bytes);
    return out;
}

} // namespace tesseract
