#include "tesseract/hash.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"

namespace tesseract
{

Sha256Digest sha256(const void* data, std::size_t size)
{
    static const std::uint8_t kEmpty = 0;
    const auto* p = size == 0 ? &kEmpty : static_cast<const std::uint8_t*>(data);
    auto v = tesseract_ffi::sha256(rust::Slice<const std::uint8_t>(p, size));
    Sha256Digest out{};
    if (v.size() == out.size())
    {
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = v[i];
        }
    }
    return out;
}

std::string to_hex(const Sha256Digest& d)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(d.size() * 2);
    for (auto b : d)
    {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 0xf]);
    }
    return s;
}

} // namespace tesseract
