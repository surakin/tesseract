cmake_minimum_required(VERSION 3.25)

# Download Twemoji Mozilla (COLR/CPAL v0 color emoji font) once at configure
# time. Keeps the binary out of git while ensuring a reproducible build.
set(_twemoji_ttf "${CMAKE_CURRENT_SOURCE_DIR}/fonts/twemoji-mozilla.ttf")
if(NOT EXISTS "${_twemoji_ttf}")
    message(STATUS "Downloading Twemoji Mozilla font (one-time)...")
    file(DOWNLOAD
        "https://github.com/mozilla/twemoji-colr/releases/download/v0.7.0/Twemoji.Mozilla.ttf"
        "${_twemoji_ttf}"
        EXPECTED_HASH SHA256=6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e
        SHOW_PROGRESS
        STATUS _twemoji_status)
    list(GET _twemoji_status 0 _twemoji_err)
    if(_twemoji_err)
        message(FATAL_ERROR "Failed to download Twemoji Mozilla font: ${_twemoji_status}")
    endif()
endif()

# Generate Tesseract.ico from the shared SVG at build time. Renderer is
# resvg-py (pure Rust SVG -> PNG), then a small stdlib packer wraps the
# frames in an ICO container. Install once per build environment with:
#     python -m pip install resvg-py
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_svg    "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg")
set(_script "${CMAKE_CURRENT_SOURCE_DIR}/../icons/generate_ico.py")
set(TESSERACT_ICO_PATH "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.ico")

add_custom_command(
    OUTPUT "${TESSERACT_ICO_PATH}"
    COMMAND ${Python3_EXECUTABLE} "${_script}" "${TESSERACT_ICO_PATH}"
    DEPENDS "${_svg}" "${_script}"
    COMMENT "Generating Tesseract.ico from tesseract.svg"
    VERBATIM
)

# Bake the generated ICO's absolute path into the .rc so rc.exe / windres
# can find it regardless of build-dir layout.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Tesseract.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.rc"
    @ONLY
)

# Tell CMake the resource depends on the .ico, so rebuilding the SVG forces
# rc.exe to re-run.
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.rc"
    PROPERTIES OBJECT_DEPENDS "${TESSERACT_ICO_PATH}"
)

add_executable(tesseract_win32 WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/RoomWindow.cpp
    src/LoginView.cpp
    src/TextRenderer.cpp
    src/Theme.cpp
    src/Win32Notifier.cpp
    src/Win32ScreenLock.cpp
    src/Win32TrayIcon.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.rc"
)

target_compile_features(tesseract_win32 PRIVATE cxx_std_20)

# MSVC defaults to interpreting source files as Windows-1252. /utf-8 sets
# both source and execution charset to UTF-8 so that L"…" wide-string
# literals compile to the correct Unicode codepoints.
if(MSVC)
    target_compile_options(tesseract_win32 PRIVATE /utf-8)
endif()

# Windows SDK 10.0.19041's bundled C++/WinRT headers predate /permissive-
# (implied by /std:c++20). winrt/impl/Windows.Foundation.0.h issues the
# qualified call winrt::impl::wait_for() before that template is declared
# (it's defined later in winrt/Windows.Foundation.h), which conforming
# two-phase name lookup rejects. Relax two-phase lookup for just the two
# TUs that pull in cppwinrt (the toast-notification path). The companion
# <experimental/coroutine> incompatibility is handled in source by
# src/winrt_coroutine_shim.h.
if(MSVC)
    set_source_files_properties(src/main.cpp src/Win32Notifier.cpp
        PROPERTIES COMPILE_OPTIONS "/Zc:twoPhase-")
endif()

target_include_directories(tesseract_win32 PRIVATE src)

# C++/WinRT headers live in <WinSDK>/include/<ver>/cppwinrt/ (SDK 10.0.17134+).
# The MSVC toolchain adds <sdk>/winrt/ as an external path for WRL-style
# Windows Runtime headers, but NOT cppwinrt/ — add it explicitly.
if(MSVC)
    cmake_host_system_information(RESULT _winsdk_root QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10" VIEW BOTH)
    if(_winsdk_root)
        file(GLOB _cppwinrt_ver_dirs LIST_DIRECTORIES true
             "${_winsdk_root}include/10.*")
        list(SORT _cppwinrt_ver_dirs ORDER DESCENDING)
        list(GET  _cppwinrt_ver_dirs 0 _cppwinrt_ver_dir)
        get_filename_component(_cppwinrt_ver "${_cppwinrt_ver_dir}" NAME)
        target_include_directories(tesseract_win32 SYSTEM PRIVATE
            "${_winsdk_root}include/${_cppwinrt_ver}/cppwinrt")
    endif()
endif()

target_link_libraries(tesseract_win32 PRIVATE
    # The two C++ archives have a circular dependency (client ↔ bridge), so they
    # use WHOLE_ARCHIVE to guarantee all symbols are present regardless of order.
    # The Rust staticlib links normally — WHOLE_ARCHIVE would duplicate Windows
    # import-descriptor stubs (__IMPORT_DESCRIPTOR_kernel32, etc.) across crates.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client>
    tesseract_sdk_ffi-static
    tesseract_tk
    comctl32
    gdiplus
    ole32
    dwmapi
    uxtheme
    d2d1
    d3d11
    dxgi
    dwrite
    windowsapp
    wtsapi32
)

target_compile_definitions(tesseract_win32 PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    # GetDpiForWindow + per-monitor v2 awareness require Windows 10 (1607).
    _WIN32_WINNT=0x0A00)

# MinGW: main.cpp uses wWinMain (wide entry). MinGW's default crtexewin
# object resolves WinMain; -municode swaps to the unicode entry resolver.
# MSVC auto-detects the entry point, so this only matters for MinGW.
if(MINGW)
    target_compile_options(tesseract_win32 PRIVATE -municode)
    target_link_options(tesseract_win32 PRIVATE -municode)
endif()

set_target_properties(tesseract_win32 PROPERTIES
    OUTPUT_NAME "Tesseract")

# ---------------------------------------------------------------------------
# Installer (CPack NSIS) — see cmake/Installers.cmake for generator config.
# Nothing else needs bundling: the Rust SDK is a staticlib and all Win32
# system libraries are part of the OS, so the install set is just the .exe.
# ---------------------------------------------------------------------------
install(TARGETS tesseract_win32
        RUNTIME DESTINATION bin
        COMPONENT Application)

set(TESSERACT_WIN_SIGN_CERT "" CACHE STRING
    "Path to .pfx for signtool (empty = no signing)")
set(TESSERACT_WIN_SIGN_PASS "" CACHE STRING
    "Password for the .pfx referenced by TESSERACT_WIN_SIGN_CERT")

if(TESSERACT_WIN_SIGN_CERT)
    install(CODE "
        execute_process(COMMAND signtool sign
            /f \"${TESSERACT_WIN_SIGN_CERT}\"
            /p \"${TESSERACT_WIN_SIGN_PASS}\"
            /tr http://timestamp.digicert.com /td sha256 /fd sha256
            \"\${CMAKE_INSTALL_PREFIX}/bin/Tesseract.exe\")
    " COMPONENT Application)
endif()
