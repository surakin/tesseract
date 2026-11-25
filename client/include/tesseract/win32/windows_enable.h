// Opens a region in which <windows.h> and other Windows SDK headers may be included inside a unity build. Pair with
// tesseract/win32/windows_disable.h, which normally follows the Windows includes at once (see tesseract/win32.h, the
// one header Windows code includes <windows.h> through).
//
// <windows.h> defines macros whose names are also Tesseract identifiers: the A/W function switches (`PostMessage`
// becomes `PostMessageW`, `DrawText` becomes `DrawTextW`), the empty legacy `near` and `far`, `small` from <rpcndr.h>,
// `min`/`max` without NOMINMAX. In a unity build one translation unit holds several .cpp files, so a portable file
// that follows a Windows one would see those macros (`float near`, `char small`). This header remembers the state of
// each such name; windows_disable.h restores it, so the macros are gone again and Windows code calls the explicit
// `...W` functions. Not here: `interface`, which the COM headers need, and the annotation and constant macros IN, OUT,
// OPTIONAL, ERROR and DELETE, which later SDK headers use. Intentionally has no include guard.
#pragma push_macro("LoadLibrary")
#pragma push_macro("CreateDirectory")
#pragma push_macro("SetCurrentDirectory")
#pragma push_macro("CreateSemaphore")
#pragma push_macro("near")
#pragma push_macro("far")
#pragma push_macro("small")
#pragma push_macro("CopyFile")
#pragma push_macro("CreateEvent")
#pragma push_macro("CreateFile")
#pragma push_macro("CreateMutex")
#pragma push_macro("CreateProcess")
#pragma push_macro("CreateWindow")
#pragma push_macro("DeleteFile")
#pragma push_macro("DrawText")
#pragma push_macro("GetCommandLine")
#pragma push_macro("GetCurrentDirectory")
#pragma push_macro("GetEnvironmentVariable")
#pragma push_macro("GetMessage")
#pragma push_macro("GetObject")
#pragma push_macro("GetUserName")
#pragma push_macro("MoveFile")
#pragma push_macro("PostMessage")
#pragma push_macro("RegisterClass")
#pragma push_macro("RemoveDirectory")
#pragma push_macro("SendMessage")
#pragma push_macro("SetEnvironmentVariable")
#pragma push_macro("Yield")
#pragma push_macro("min")
#pragma push_macro("max")
#pragma push_macro("GetClassName")
#pragma push_macro("GetModuleHandle")
#pragma push_macro("LoadIcon")
#pragma push_macro("LoadImage")
#pragma push_macro("LoadCursor")
#pragma push_macro("LoadString")
#pragma push_macro("MessageBox")
#pragma push_macro("SetWindowText")
#pragma push_macro("GetWindowText")
#pragma push_macro("GetFileAttributes")
#pragma push_macro("FindFirstFile")
#pragma push_macro("FindWindow")
#pragma push_macro("GetTempPath")
#pragma push_macro("GetModuleFileName")
#pragma push_macro("CreateFont")
#pragma push_macro("GetTextExtentPoint")
#pragma push_macro("ShellExecute")
#pragma push_macro("ShellExecuteEx")
#pragma push_macro("LoadBitmap")
#pragma push_macro("RegisterClassEx")
#pragma push_macro("DefWindowProc")
#pragma push_macro("DispatchMessage")
#pragma push_macro("PeekMessage")
#pragma push_macro("GetOpenFileName")
#pragma push_macro("GetSaveFileName")
#pragma push_macro("SHBrowseForFolder")
#pragma push_macro("SHGetPathFromIDList")
#pragma push_macro("CreateEventEx")
#pragma push_macro("CharUpper")
#pragma push_macro("CharLower")
#pragma push_macro("GetProp")
#pragma push_macro("SetProp")
#pragma push_macro("RemoveProp")
#pragma push_macro("Rectangle")
#pragma push_macro("Polygon")
#pragma push_macro("Ellipse")
#pragma push_macro("GetMonitorInfo")
#pragma push_macro("TextOut")
#pragma push_macro("SetWindowLongPtr")
#pragma push_macro("GetWindowLongPtr")
#pragma push_macro("GetClassLongPtr")
#pragma push_macro("SetClassLongPtr")
#pragma push_macro("CreateDialog")
#pragma push_macro("GetStartupInfo")
#pragma push_macro("OutputDebugString")
// Inside the region `far` and `near` are what <windows.h> makes them, empty: an SDK header included in a later pair of
// its own (shlobj.h, dbghelp.h) spells `FAR` and `NEAR`, which expand to them, and windows_disable.h has taken the
// definitions away again. <windows.h> redefines both the same way, so this changes nothing for it.
#undef far
#define far
#undef near
#define near
