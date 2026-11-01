#include "tk/image_sniff.h"

#include <cstddef>
#include <cstring>

namespace tk
{

namespace
{

bool has_prefix(std::span<const std::uint8_t> b, const char* p, std::size_t at = 0)
{
    const std::size_t n = std::strlen(p);
    return b.size() >= at + n && std::memcmp(b.data() + at, p, n) == 0;
}

bool png_has_actl_before_idat(std::span<const std::uint8_t> b)
{
    std::size_t pos = 8; // after the 8-byte signature
    while (pos + 8 <= b.size())
    {
        const std::uint32_t len = (std::uint32_t{b[pos]} << 24) |
                                  (std::uint32_t{b[pos + 1]} << 16) |
                                  (std::uint32_t{b[pos + 2]} << 8) | std::uint32_t{b[pos + 3]};
        if (has_prefix(b, "acTL", pos + 4))
        {
            return true;
        }
        if (has_prefix(b, "IDAT", pos + 4) || has_prefix(b, "IEND", pos + 4))
        {
            return false;
        }
        const std::size_t next =
            pos + 12 + static_cast<std::size_t>(len); // len + type + data + crc
        if (next <= pos)
        {
            return false;
        }
        pos = next;
    }
    return false;
}

std::uint32_t rd_be32(std::span<const std::uint8_t> b, std::size_t at)
{
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
           (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

std::uint32_t rd_le16(std::span<const std::uint8_t> b, std::size_t at)
{
    return std::uint32_t{b[at]} | (std::uint32_t{b[at + 1]} << 8);
}

std::uint32_t rd_le24(std::span<const std::uint8_t> b, std::size_t at)
{
    return rd_le16(b, at) | (std::uint32_t{b[at + 2]} << 16);
}

std::uint32_t rd_le32(std::span<const std::uint8_t> b, std::size_t at)
{
    return rd_le24(b, at) | (std::uint32_t{b[at + 3]} << 24);
}

// Walk JPEG marker segments to the first SOFn (C0-CF except DHT C4, JPG C8,
// DAC CC). Stops at SOS/EOI or on anything malformed.
std::optional<ImageDims> probe_jpeg(std::span<const std::uint8_t> b)
{
    std::size_t pos = 2;
    while (pos + 4 <= b.size())
    {
        if (b[pos] != 0xFF)
        {
            // Junk between segments: decoders resync on the next 0xFF, so
            // the probe must too or one stray byte hides an oversized SOF.
            while (pos < b.size() && b[pos] != 0xFF)
            {
                ++pos;
            }
            continue;
        }
        const std::uint8_t marker = b[pos + 1];
        if (marker == 0xFF)
        {
            ++pos; // fill byte
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
        {
            pos += 2; // standalone marker, no length
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA)
        {
            return std::nullopt;
        }
        const std::size_t len = (std::size_t{b[pos + 2]} << 8) | b[pos + 3];
        if (len < 2)
        {
            return std::nullopt;
        }
        const bool sof =
            marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof)
        {
            if (pos + 9 > b.size())
            {
                return std::nullopt;
            }
            const std::uint32_t h = (std::uint32_t{b[pos + 5]} << 8) | b[pos + 6];
            const std::uint32_t w = (std::uint32_t{b[pos + 7]} << 8) | b[pos + 8];
            return ImageDims{w, h};
        }
        pos += 2 + len;
    }
    return std::nullopt;
}

// LSB-first bit reader for the JPEG XL size header; nullopt past the end.
class BitReader
{
public:
    explicit BitReader(std::span<const std::uint8_t> b) : b_(b)
    {
    }
    std::optional<std::uint32_t> read(int n)
    {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i, ++pos_)
        {
            if (pos_ / 8 >= b_.size())
            {
                return std::nullopt;
            }
            v |= static_cast<std::uint64_t>((b_[pos_ / 8] >> (pos_ % 8)) & 1u) << i;
        }
        return static_cast<std::uint32_t>(v);
    }

private:
    std::span<const std::uint8_t> b_;
    std::size_t pos_ = 0;
};

constexpr std::uint8_t kJxlSignatureBox[12] = {0,   0,   0,    0x0C, 'J',  'X',
                                               'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};

// The codestream inside `b`, past its `FF 0A` signature: the bare file itself,
// or the payload of the first `jxlc` / `jxlp` box of a container. Empty when absent.
std::span<const std::uint8_t> jxl_codestream(std::span<const std::uint8_t> b)
{
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0x0A)
    {
        return b.subspan(2);
    }
    std::size_t pos = 0;
    while (pos + 8 <= b.size())
    {
        std::uint64_t size = rd_be32(b, pos);
        std::size_t hdr = 8;
        if (size == 1)
        {
            if (pos + 16 > b.size())
            {
                return {};
            }
            size = (std::uint64_t{rd_be32(b, pos + 8)} << 32) | rd_be32(b, pos + 12);
            hdr = 16;
        }
        else if (size == 0)
        {
            size = b.size() - pos; // box runs to the end of the file
        }
        if (size < hdr)
        {
            return {};
        }
        const bool jxlc = has_prefix(b, "jxlc", pos + 4);
        const bool jxlp = has_prefix(b, "jxlp", pos + 4);
        if (jxlc || jxlp)
        {
            const std::size_t start = pos + hdr + (jxlp ? 4 : 0); // jxlp: 4-byte sequence index
            if (start + 2 > b.size() || b[start] != 0xFF || b[start + 1] != 0x0A)
            {
                return {};
            }
            return b.subspan(start + 2); // the box payload carries the FF 0A signature too
        }
        if (size > b.size() - pos)
        {
            return {};
        }
        pos += static_cast<std::size_t>(size);
    }
    return {};
}

// Reads the SizeHeader at the start of a JPEG XL codestream.
std::optional<ImageDims> probe_jxl(std::span<const std::uint8_t> file)
{
    const auto cs = jxl_codestream(file);
    BitReader r(cs);
    const auto read_size = [&](bool div8) -> std::optional<std::uint32_t>
    {
        if (div8)
        {
            const auto v = r.read(5);
            return v ? std::optional<std::uint32_t>((*v + 1) * 8) : std::nullopt;
        }
        const auto sel = r.read(2);
        if (!sel)
        {
            return std::nullopt;
        }
        constexpr int kBits[4] = {9, 13, 18, 30};
        const auto v = r.read(kBits[*sel]);
        return v ? std::optional<std::uint32_t>(*v + 1) : std::nullopt;
    };
    const auto div8 = r.read(1);
    if (!div8)
    {
        return std::nullopt;
    }
    const auto h = read_size(*div8 != 0);
    const auto ratio = r.read(3);
    if (!h || !ratio)
    {
        return std::nullopt;
    }
    std::optional<std::uint32_t> w;
    if (*ratio == 0)
    {
        w = read_size(*div8 != 0);
    }
    else
    {
        constexpr std::uint32_t kNum[8] = {0, 1, 12, 4, 3, 16, 5, 2};
        constexpr std::uint32_t kDen[8] = {0, 1, 10, 3, 2, 9, 4, 1};
        w = static_cast<std::uint32_t>(std::uint64_t{*h} * kNum[*ratio] / kDen[*ratio]);
    }
    if (!w)
    {
        return std::nullopt;
    }
    return ImageDims{*w, *h};
}

} // namespace

bool is_jxl_bytes(std::span<const std::uint8_t> b)
{
    return (b.size() >= 2 && b[0] == 0xFF && b[1] == 0x0A) ||
           (b.size() >= 12 && std::memcmp(b.data(), kJxlSignatureBox, 12) == 0);
}

std::string_view sniff_image_mime(std::span<const std::uint8_t> b)
{
    if (has_prefix(b, "\x89PNG\r\n\x1a\n"))
    {
        return "image/png";
    }
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    {
        return "image/jpeg";
    }
    if (has_prefix(b, "GIF87a") || has_prefix(b, "GIF89a"))
    {
        return "image/gif";
    }
    if (has_prefix(b, "RIFF") && has_prefix(b, "WEBP", 8))
    {
        return "image/webp";
    }
    return {};
}

bool bytes_may_be_animated(std::span<const std::uint8_t> bytes)
{
    if (has_prefix(bytes, "GIF8"))
    {
        return true;
    }
    if (is_jxl_bytes(bytes))
    {
        return true;
    }
    if (has_prefix(bytes, "RIFF") && has_prefix(bytes, "WEBP", 8) && has_prefix(bytes, "VP8X", 12))
    {
        return bytes.size() > 20 && (bytes[20] & 0x02) != 0; // animation flag
    }
    if (bytes.size() > 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
    {
        return png_has_actl_before_idat(bytes);
    }
    return false;
}

std::optional<ImageDims> probe_image_dimensions(std::span<const std::uint8_t> b)
{
    if (b.size() >= 24 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0 &&
        has_prefix(b, "IHDR", 12))
    {
        return ImageDims{rd_be32(b, 16), rd_be32(b, 20)};
    }
    if (b.size() >= 10 && has_prefix(b, "GIF8"))
    {
        return ImageDims{rd_le16(b, 6), rd_le16(b, 8)};
    }
    if (b.size() >= 30 && has_prefix(b, "RIFF") && has_prefix(b, "WEBP", 8))
    {
        if (has_prefix(b, "VP8X", 12))
        {
            return ImageDims{rd_le24(b, 24) + 1, rd_le24(b, 27) + 1};
        }
        if (has_prefix(b, "VP8L", 12) && b[20] == 0x2f)
        {
            const std::uint32_t bits = rd_le32(b, 21);
            return ImageDims{(bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1};
        }
        if (has_prefix(b, "VP8 ", 12))
        {
            return ImageDims{rd_le16(b, 26) & 0x3fff, rd_le16(b, 28) & 0x3fff};
        }
        return std::nullopt;
    }
    if (b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8)
    {
        return probe_jpeg(b);
    }
    if (is_jxl_bytes(b))
    {
        return probe_jxl(b);
    }
    return std::nullopt;
}

bool decode_size_allowed(std::span<const std::uint8_t> bytes, bool animated)
{
    const auto d = probe_image_dimensions(bytes);
    if (!d)
    {
        return true;
    }
    const bool jpeg_still = !animated && bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8;
    const std::uint32_t max_side = jpeg_still ? kMaxJpegDecodeSide : kMaxDecodeSide;
    if (d->width > max_side || d->height > max_side)
    {
        return false;
    }
    const std::uint64_t px = std::uint64_t{d->width} * d->height;
    const std::uint64_t max_px =
        animated ? kMaxAnimatedDecodePixels
                 : (jpeg_still ? kMaxJpegStillDecodePixels : kMaxStillDecodePixels);
    return px <= max_px;
}

} // namespace tk
