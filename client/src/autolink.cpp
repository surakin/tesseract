#include "tesseract/autolink.h"

#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <string>
#include <vector>

namespace tesseract
{

std::vector<UrlSpan> find_url_spans(std::string_view text)
{
    if (text.empty())
        return {};

    auto rust_spans = tesseract_ffi::find_url_spans(std::string(text));
    std::vector<UrlSpan> out;
    out.reserve(rust_spans.size());
    for (const auto& s : rust_spans)
    {
        out.push_back(UrlSpan{s.start, s.end, std::string(s.url)});
    }
    return out;
}

} // namespace tesseract
