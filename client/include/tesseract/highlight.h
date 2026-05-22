#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract
{

/// One colored run of a syntax-highlighted code block. Concatenating every
/// span's `text` reproduces the input code (newlines preserved).
struct HighlightSpan
{
    std::string  text;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// Syntax-highlight `code` as language `lang` (a markdown fence token such as
/// "rust", "py", or "c++") for the given light/dark mode. Returns colored runs
/// whose concatenated text equals `code`. An empty vector means the language
/// was not recognized — the caller should render the block as plain monospace.
///
/// Lives in the client layer (no UI/tk dependency) so it can be unit-tested and
/// shared by every shell. Results are memoized in a bounded in-process cache
/// keyed by (dark, lang, code) because the renderer calls this on every
/// measure/paint pass.
std::vector<HighlightSpan>
highlight_code(const std::string& code, const std::string& lang, bool dark);

} // namespace tesseract
