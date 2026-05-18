#pragma once
#include <string>

#include "views/text_util.h"

namespace tesseract::macos
{

// Thin alias kept for existing macOS call sites; delegates to the shared
// tesseract::text::trim (identical " \t\n\r" semantics).
inline std::string trim(std::string s)
{
    return tesseract::text::trim(s);
}

} // namespace tesseract::macos
