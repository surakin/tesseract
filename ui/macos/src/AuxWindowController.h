#pragma once

#include "app/AuxWindowBase.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <memory>
#include <string>

// C++ factory — allocates a MacAuxWindow (which creates its own NSWindow and
// delegate) hosting `root`. The caller takes ownership of the returned pointer.
namespace tesseract
{
AuxWindowBase* make_mac_aux_window(const std::string& title,
                                   std::unique_ptr<tk::Widget> root,
                                   int width, int height, const tk::Theme& theme);
} // namespace tesseract
