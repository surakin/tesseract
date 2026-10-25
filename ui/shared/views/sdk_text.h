#pragma once
#include <string>
#include <string_view>

namespace tesseract::views::sdk_text
{

// Decodes the language-neutral codes the SDK layer puts in place of English
// prose (sdk/src/client/text_code.rs): previews of non-text messages
// ("image", "poll" + question, …), undecryptable-message reasons ("utd_*")
// and pinned-message actions ("pin_*"). A code is "\x01name" optionally
// followed by "\x01argument". Text without the leading marker (a message
// body, or a preview cached before codes existed) is returned unchanged, and
// so is a code this build doesn't know, minus its marker.
std::string localize(std::string_view text);

// True when `text` is the SDK code `name` (with or without an argument).
bool is_code(std::string_view text, std::string_view name);

} // namespace tesseract::views::sdk_text
