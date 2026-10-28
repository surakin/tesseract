#include "views/image_animation.h"

#include <tesseract/jxl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace tesseract
{

namespace
{

bool has_prefix(std::span<const std::uint8_t> b, const char* p, std::size_t at = 0)
{
    const std::size_t n = std::strlen(p);
    return b.size() >= at + n && std::memcmp(b.data() + at, p, n) == 0;
}

std::uint32_t rd_be32(std::span<const std::uint8_t> b, std::size_t at)
{
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
           (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

// Skips a chain of GIF data sub-blocks starting at `pos` (each: size byte +
// data, ended by a zero size). Returns the position past the terminator, or
// npos when the data is truncated.
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

std::size_t skip_gif_sub_blocks(std::span<const std::uint8_t> b, std::size_t pos)
{
    while (pos < b.size())
    {
        const std::size_t len = b[pos];
        if (len == 0)
            return pos + 1;
        pos += 1 + len;
    }
    return kNpos;
}

// More than one image block (0x2C) before the trailer. A truncated tail stops
// the walk but frames already seen still count.
bool gif_has_multiple_frames(std::span<const std::uint8_t> b)
{
    if (b.size() < 13)
        return false;
    std::size_t pos = 13; // 6-byte signature + 7-byte logical screen descriptor
    if (b[10] & 0x80)
        pos += std::size_t{3} << ((b[10] & 0x07) + 1); // global colour table
    int frames = 0;
    while (pos < b.size())
    {
        const std::uint8_t tag = b[pos];
        if (tag == 0x3B) // trailer
            break;
        if (tag == 0x21) // extension: label + sub-blocks
        {
            pos = skip_gif_sub_blocks(b, pos + 2);
        }
        else if (tag == 0x2C) // image descriptor
        {
            if (++frames >= 2)
                return true;
            if (pos + 10 > b.size())
                break;
            const std::uint8_t packed = b[pos + 9];
            pos += 10;
            if (packed & 0x80)
                pos += std::size_t{3} << ((packed & 0x07) + 1); // local colour table
            pos = pos + 1 <= b.size() ? skip_gif_sub_blocks(b, pos + 1) : kNpos; // LZW min code size
        }
        else
            break; // malformed
        if (pos == kNpos)
            break;
    }
    return false;
}

// RIFF....WEBPVP8X<size><flags>: bit 1 is the animation flag.
bool webp_is_animated(std::span<const std::uint8_t> b)
{
    return has_prefix(b, "RIFF") && has_prefix(b, "WEBP", 8) && has_prefix(b, "VP8X", 12) &&
           b.size() > 20 && (b[20] & 0x02) != 0;
}

// acTL (before the first IDAT) announcing more than one frame.
bool apng_is_animated(std::span<const std::uint8_t> b)
{
    std::size_t pos = 8; // after the PNG signature
    while (pos + 8 <= b.size())
    {
        const std::size_t len = rd_be32(b, pos);
        if (has_prefix(b, "acTL", pos + 4))
            return pos + 12 <= b.size() && rd_be32(b, pos + 8) > 1;
        if (has_prefix(b, "IDAT", pos + 4) || has_prefix(b, "IEND", pos + 4))
            return false;
        const std::size_t next = pos + 12 + len; // len + type + data + crc
        if (next <= pos)
            return false;
        pos = next;
    }
    return false;
}

// ISOBMFF `ftyp` whose major or compatible brands include `avis` (AVIF image
// sequence).
bool avif_is_sequence(std::span<const std::uint8_t> b)
{
    if (b.size() < 16 || !has_prefix(b, "ftyp", 4))
        return false;
    const std::size_t box = std::min<std::size_t>(rd_be32(b, 0), b.size());
    if (has_prefix(b, "avis", 8))
        return true;
    for (std::size_t at = 16; at + 4 <= box; at += 4) // compatible brands follow the minor version
        if (has_prefix(b, "avis", at))
            return true;
    return false;
}

bool mime_rules_out(std::string_view mime)
{
    if (mime.empty() || mime.starts_with("image/") || mime == "application/octet-stream")
        return false;
    return true;
}

} // namespace

bool is_animated_image(std::span<const std::uint8_t> bytes, std::string_view mime)
{
    if (bytes.size() < 4 || mime_rules_out(mime))
        return false;
    if (has_prefix(bytes, "GIF8"))
        return gif_has_multiple_frames(bytes);
    if (has_prefix(bytes, "RIFF"))
        return webp_is_animated(bytes);
    if (bytes.size() > 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        return apng_is_animated(bytes);
    if (avif_is_sequence(bytes))
        return true;
    return is_jxl_animated(bytes);
}

} // namespace tesseract
