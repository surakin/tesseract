#include "tesseract/markdown.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
#include <string>

namespace tesseract
{

MarkdownResult markdown_to_html(std::string_view text)
{
    std::string body(text);
    auto r = tesseract_ffi::markdown_to_html(body);
    return {std::move(body), std::string(r.formatted_body)};
}

std::string markdown_inline_to_html(std::string_view text)
{
    std::string body(text);
    return std::string(tesseract_ffi::markdown_inline_to_html(body));
}

} // namespace tesseract
