#include "tesseract/jxl.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

namespace tesseract
{

struct JxlDecoder::Impl
{
    rust::Box<tesseract_ffi::JxlDecoder> dec;
};

bool is_jxl_data(std::span<const std::uint8_t> bytes)
{
    return tesseract_ffi::jxl_is_data(rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()));
}

bool is_jxl_animated(std::span<const std::uint8_t> bytes)
{
    return tesseract_ffi::jxl_is_animated(
        rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()));
}

JxlDecoder::JxlDecoder(std::span<const std::uint8_t> bytes, std::uint32_t max_w,
                       std::uint32_t max_h)
    : impl_(new Impl{tesseract_ffi::jxl_decoder_new(
          rust::Slice<const std::uint8_t>(bytes.data(), bytes.size()), max_w, max_h)})
{
}

JxlDecoder::~JxlDecoder() = default;
JxlDecoder::JxlDecoder(JxlDecoder&&) noexcept = default;
JxlDecoder& JxlDecoder::operator=(JxlDecoder&&) noexcept = default;

bool JxlDecoder::valid() const
{
    return impl_->dec->valid();
}
std::uint32_t JxlDecoder::width() const
{
    return impl_->dec->width();
}
std::uint32_t JxlDecoder::height() const
{
    return impl_->dec->height();
}
std::uint32_t JxlDecoder::frame_count() const
{
    return impl_->dec->frame_count();
}
bool JxlDecoder::has_more_frames() const
{
    return impl_->dec->has_more_frames();
}

const std::uint8_t* JxlDecoder::next_frame(int& delay_ms)
{
    if (!impl_->dec->next_frame())
    {
        return nullptr;
    }
    delay_ms = impl_->dec->frame_delay_ms();
    return impl_->dec->frame_pixels().data();
}

void JxlDecoder::reset()
{
    impl_->dec->reset();
}

} // namespace tesseract
