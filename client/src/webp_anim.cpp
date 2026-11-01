#include "tesseract/webp_anim.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

namespace tesseract
{

struct WebpAnimDecoder::Impl
{
    rust::Box<tesseract_ffi::WebpAnimDecoder> dec;
};

bool is_webp_data(std::span<const std::uint8_t> bytes)
{
    return tesseract_ffi::webp_is_data(rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()));
}

WebpAnimDecoder::WebpAnimDecoder(std::span<const std::uint8_t> bytes)
    : impl_(new Impl{tesseract_ffi::webp_anim_decoder_new(
          rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()))})
{
}

WebpAnimDecoder::~WebpAnimDecoder() = default;
WebpAnimDecoder::WebpAnimDecoder(WebpAnimDecoder&&) noexcept = default;
WebpAnimDecoder& WebpAnimDecoder::operator=(WebpAnimDecoder&&) noexcept = default;

bool WebpAnimDecoder::valid() const
{
    return impl_->dec->valid();
}
std::uint32_t WebpAnimDecoder::canvas_width() const
{
    return impl_->dec->canvas_width();
}
std::uint32_t WebpAnimDecoder::canvas_height() const
{
    return impl_->dec->canvas_height();
}
std::uint32_t WebpAnimDecoder::frame_count() const
{
    return impl_->dec->frame_count();
}
bool WebpAnimDecoder::has_more_frames() const
{
    return impl_->dec->has_more_frames();
}

const std::uint8_t* WebpAnimDecoder::next_frame(int& timestamp_ms)
{
    if (!impl_->dec->next_frame())
    {
        return nullptr;
    }
    timestamp_ms = impl_->dec->frame_timestamp_ms();
    return impl_->dec->frame_pixels().data();
}

void WebpAnimDecoder::reset()
{
    impl_->dec->reset();
}

} // namespace tesseract
