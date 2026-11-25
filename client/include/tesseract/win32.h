#pragma once

// The one way Tesseract code includes <windows.h>. It is included between win32/windows_enable.h and
// win32/windows_disable.h, which remove at once the macros whose names are also Tesseract identifiers (`PostMessage`,
// `near`, `far`, `small`, ...; the list and the reason are in windows_enable.h). Consequences for Windows code:
//   - Call the explicit wide functions (`PostMessageW`, `GetObjectW`, `CreateFileW`), never the macro names.
//   - Ordinary macros (`INVALID_HANDLE_VALUE`, `MAX_PATH`, `WM_*`, ...) stay available.
//   - A file that needs more Windows headers (shlobj.h, d2d1.h, mfapi.h, ...) includes them inside its own
//     enable/disable pair, after this header.
// WIN32_LEAN_AND_MEAN and NOMINMAX are left to each file / target, as some code needs the full windows.h or min/max.

#include "tesseract/win32/windows_enable.h"
#include <windows.h>
#include "tesseract/win32/windows_disable.h"
