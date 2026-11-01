#include "jxl_decode.h"

#include "tk/anim_decode_session.h"
#include "tk/image_sniff.h"

#include <algorithm>

namespace tesseract::jxl
{

namespace
{

// Resumable windowed decode over a Rust JxlDecoder, which renders frames
// lazily and so is naturally forward-only; restart() rewinds it.
class JxlAnimSession final : public tk::AnimDecodeSession
{
public:
    JxlAnimSession(JxlDecoder dec, MakeImage make_image, int total_frames,
                   std::size_t encoded_bytes)
        : dec_(std::move(dec)), make_image_(std::move(make_image)), total_(total_frames),
          encoded_bytes_(encoded_bytes)
    {
    }

    int decode_next_batch(
        int n, const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame) override
    {
        int produced = 0;
        while (produced < n && cursor_ < total_)
        {
            int delay = 0;
            const std::uint8_t* px = dec_.next_frame(delay);
            std::unique_ptr<tk::Image> img;
            if (px)
            {
                img = make_image_(px, static_cast<int>(dec_.width()),
                                  static_cast<int>(dec_.height()));
            }
            if (!img)
            {
                // A sequential decoder cannot skip a bad frame; treat as the end.
                cursor_ = total_;
                break;
            }
            on_frame(cursor_++, std::move(img), tk::normalize_frame_delay_ms(delay));
            ++produced;
        }
        return produced;
    }

    bool exhausted() const override
    {
        return cursor_ >= total_;
    }

    void restart() override
    {
        dec_.reset();
        cursor_ = 0;
    }

    std::size_t memory_bytes() const override
    {
        return encoded_bytes_ + static_cast<std::size_t>(dec_.width()) * dec_.height() * 4;
    }

private:
    JxlDecoder dec_;
    MakeImage make_image_;
    int total_;
    int cursor_ = 0;
    std::size_t encoded_bytes_;
};

// Budget + signature gate shared by every entry point. `animated` selects the
// stricter animation pixel budget (a JXL's frame count is unknown pre-decode,
// so the streaming paths, which only accept animations, use it; the whole-image
// path starts with the still budget and the decoder enforces the animated one).
bool allowed(std::span<const std::uint8_t> bytes, bool animated)
{
    return is_jxl_data(bytes) && tk::decode_size_allowed(bytes, animated);
}

} // namespace

DecodedImage decode_image(std::span<const std::uint8_t> bytes, int max_w, int max_h,
                          const MakeImage& make_image)
{
    DecodedImage out;
    if (!allowed(bytes, /*animated=*/false) || max_w <= 0 || max_h <= 0)
    {
        return out;
    }
    JxlDecoder dec(bytes, static_cast<std::uint32_t>(max_w), static_cast<std::uint32_t>(max_h));
    if (!dec.valid())
    {
        return out;
    }
    const auto w = static_cast<int>(dec.width());
    const auto h = static_cast<int>(dec.height());
    const bool animated = dec.frame_count() > 1;
    int delay = 0;
    while (static_cast<int>(out.frames.size()) < tk::kAnimDecodeMaxFrames || !animated)
    {
        const std::uint8_t* px = dec.next_frame(delay);
        if (!px)
        {
            break;
        }
        auto img = make_image(px, w, h);
        if (!img)
        {
            break;
        }
        if (!animated)
        {
            out.still = std::move(img);
            return out;
        }
        out.frames.push_back(std::move(img));
        out.delays_ms.push_back(tk::normalize_frame_delay_ms(delay));
    }
    if (out.frames.size() < 2)
    {
        // An animation that failed after frame 0 still shows as a still.
        if (!out.frames.empty())
        {
            out.still = std::move(out.frames.front());
        }
        out.frames.clear();
        out.delays_ms.clear();
    }
    return out;
}

bool decode_streamed(std::span<const std::uint8_t> bytes, int max_w, int max_h,
                     const MakeImage& make_image,
                     const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
                     const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (!allowed(bytes, /*animated=*/true) || max_w <= 0 || max_h <= 0)
    {
        return false;
    }
    JxlDecoder dec(bytes, static_cast<std::uint32_t>(max_w), static_cast<std::uint32_t>(max_h));
    if (!dec.valid() || dec.frame_count() <= 1)
    {
        return false;
    }
    const auto w = static_cast<int>(dec.width());
    const auto h = static_cast<int>(dec.height());
    bool got_first = false;
    for (int index = 0; index < tk::kAnimDecodeMaxFrames; ++index)
    {
        int delay = 0;
        const std::uint8_t* px = dec.next_frame(delay);
        auto img = px ? make_image(px, w, h) : nullptr;
        if (!img)
        {
            break;
        }
        delay = tk::normalize_frame_delay_ms(delay);
        if (index == 0)
        {
            got_first = true;
            on_first_frame(std::move(img), delay);
        }
        else
        {
            on_frame(index, std::move(img), delay);
        }
    }
    return got_first;
}

bool decode_streamed_windowed(
    std::span<const std::uint8_t> bytes, int max_w, int max_h, const MakeImage& make_image,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>, std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (!allowed(bytes, /*animated=*/true) || max_w <= 0 || max_h <= 0)
    {
        return false;
    }
    JxlDecoder dec(bytes, static_cast<std::uint32_t>(max_w), static_cast<std::uint32_t>(max_h));
    if (!dec.valid() || dec.frame_count() <= 1)
    {
        return false;
    }
    const int total = std::min<int>(static_cast<int>(dec.frame_count()), tk::kAnimDecodeMaxFrames);
    if (total <= tk::kAnimDecodeWindowThresholdFrames)
    {
        // Short animation: decode whole, unwindowed.
        bool got_first = false;
        const auto w = static_cast<int>(dec.width());
        const auto h = static_cast<int>(dec.height());
        for (int index = 0; index < total; ++index)
        {
            int delay = 0;
            const std::uint8_t* px = dec.next_frame(delay);
            auto img = px ? make_image(px, w, h) : nullptr;
            if (!img)
            {
                break;
            }
            delay = tk::normalize_frame_delay_ms(delay);
            if (index == 0)
            {
                got_first = true;
                on_first_frame(std::move(img), delay, nullptr, 0);
            }
            else
            {
                on_frame(index, std::move(img), delay);
            }
        }
        return got_first;
    }

    auto session =
        std::make_shared<JxlAnimSession>(std::move(dec), make_image, total, bytes.size());
    bool got_first = false;
    session->decode_next_batch(tk::kAnimDecodeInitialBatchFrames,
                               [&](int idx, std::unique_ptr<tk::Image> img, int delay_ms)
                               {
                                   if (!got_first)
                                   {
                                       got_first = true;
                                       on_first_frame(std::move(img), delay_ms, session,
                                                      static_cast<std::size_t>(total));
                                   }
                                   else
                                   {
                                       on_frame(idx, std::move(img), delay_ms);
                                   }
                               });
    return got_first;
}

std::unique_ptr<tk::Image> decode_still(tk::CanvasFactory& factory,
                                        std::span<const std::uint8_t> bytes)
{
    auto d = decode_image(bytes, static_cast<int>(tk::kMaxDecodeSide),
                          static_cast<int>(tk::kMaxDecodeSide),
                          [&factory](const std::uint8_t* px, int w, int h)
                          {
                              return factory.create_image_rgba(px, w, h);
                          });
    if (d.still)
    {
        return std::move(d.still);
    }
    if (!d.frames.empty())
    {
        return std::move(d.frames.front());
    }
    return nullptr;
}

std::unique_ptr<tk::AnimatedImage> decode_animated(tk::CanvasFactory& factory,
                                                   std::span<const std::uint8_t> bytes, int max_px)
{
    auto d = decode_image(bytes, max_px, max_px,
                          [&factory](const std::uint8_t* px, int w, int h)
                          {
                              return factory.create_image_rgba(px, w, h);
                          });
    if (d.frames.size() < 2)
    {
        return nullptr;
    }
    return std::make_unique<tk::AnimatedImage>(std::move(d.frames), std::move(d.delays_ms));
}

} // namespace tesseract::jxl
