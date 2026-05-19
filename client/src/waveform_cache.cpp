#include "tesseract/waveform_cache.h"
#include "ffi_convert.h"

namespace tesseract
{

void init_waveform_cache(const std::string& db_path)
{
    tesseract_ffi::init_waveform_store(db_path);
}

std::vector<std::uint16_t> load_voice_waveform(const std::string& mxc_uri)
{
    auto rv = tesseract_ffi::load_voice_waveform(mxc_uri);
    return std::vector<std::uint16_t>(rv.begin(), rv.end());
}

void store_voice_waveform(const std::string&               mxc_uri,
                          const std::vector<std::uint16_t>& waveform)
{
    rust::Slice<const std::uint16_t> slice{waveform.data(), waveform.size()};
    tesseract_ffi::store_voice_waveform(mxc_uri, slice);
}

void evict_voice_waveform(const std::string& mxc_uri)
{
    tesseract_ffi::evict_voice_waveform(mxc_uri);
}

std::vector<std::uint16_t>
compute_waveform_from_ogg(const std::vector<std::uint8_t>& bytes)
{
    rust::Slice<const std::uint8_t> slice{bytes.data(), bytes.size()};
    auto rv = tesseract_ffi::compute_waveform_from_ogg(slice);
    return std::vector<std::uint16_t>(rv.begin(), rv.end());
}

} // namespace tesseract
