#pragma once

// Precompiled header for the Windows shell: <windows.h> through the macro-guarded wrapper, then the COM/Direct2D headers
// inside their own enable/disable pair so their macros do not leak into the TUs either.
#include "tesseract/win32.h"

#include "tesseract/win32/windows_enable.h"
#include <wrl/client.h>
#include <d2d1.h>
#include <dwrite.h>
#include "tesseract/win32/windows_disable.h"
