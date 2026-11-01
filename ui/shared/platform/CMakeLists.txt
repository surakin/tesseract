# tesseract_platform — OS service abstractions layered on tk: video/screen
# capture, location, video decode, single-instance. Portable dispatchers sit
# at platform/; the per-OS backends sit in platform/linux/ (GStreamer/portal/
# GeoClue, shared by the Qt6 and GTK4 shells), platform/win32/ (Media
# Foundation, WinRT) and platform/macos/ (AVFoundation, CoreLocation).
# TESSERACT_UI picks which backend set is compiled.
# Layering: tk <- platform <- views <- app.

set(_platform_sources
    video_capture.h
    video_capture.cpp
    screen_capture.h
    screen_capture.cpp
    location_provider.h
    location_provider.cpp
    video_decode.h
    timezone.h
    timezone.cpp
)

if(NOT TESSERACT_UI STREQUAL "win32")
    # Plain POSIX (flock): used on macOS and Linux.
    list(APPEND _platform_sources
        single_instance.h
        single_instance.cpp
    )
endif()

if(TESSERACT_UI STREQUAL "qt6" OR TESSERACT_UI STREQUAL "gtk" OR TESSERACT_UI STREQUAL "linux")
    list(APPEND _platform_sources
        linux/video_capture_gst.cpp
        linux/screen_capture_gst.cpp
        linux/screen_capture_portal.cpp
        linux/location_provider_geoclue.cpp
        linux/video_decode_gst.cpp
        linux/gst_hw_probe.h
        linux/gst_hw_probe.cpp
        linux/gst_device_listing.h
        linux/gst_device_listing.cpp
        # Transport-independent half of the Qt6/GTK4 UnifiedPush connectors.
        linux/linux_up_connector_core.h
        linux/linux_up_connector_core.cpp
        linux/linux_autostart.h
        linux/linux_portal.h
        linux/linux_notification_reply.h
    )
elseif(TESSERACT_UI STREQUAL "macos")
    list(APPEND _platform_sources
        macos/video_capture_macos.mm
        macos/screen_capture_macos.mm
        macos/location_provider_macos.mm
        macos/video_decode_av.mm
    )
elseif(TESSERACT_UI STREQUAL "win32")
    list(APPEND _platform_sources
        win32/video_capture_win32.cpp
        win32/screen_capture_win32.cpp
        win32/location_provider_win32.cpp
        win32/video_decode_mf.cpp
        win32/winrt_coroutine_shim.h
    )
endif()

add_library(tesseract_platform STATIC ${_platform_sources})

target_link_libraries(tesseract_platform
    PUBLIC  tesseract_tk
    PRIVATE nlohmann_json::nlohmann_json
)

tesseract_target_pch(tesseract_platform
    <algorithm>
    <memory>
    <utility>
    <vector>
    <string>
    <string_view>
    <cstring>
    <cstdint>
    <cmath>
    <chrono>
    <unordered_map>
    <unordered_set>
    <mutex>
    <atomic>
    <functional>
    <optional>
)

# GStreamer is shared by the two Linux backends (qt6, gtk); its PUBLIC link
# propagates to whichever shell links this library via tesseract_app.
if(TESSERACT_UI STREQUAL "qt6" OR TESSERACT_UI STREQUAL "gtk" OR TESSERACT_UI STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PLATFORM_GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-video-1.0
        gio-2.0
    )
    target_link_libraries(tesseract_platform PUBLIC
        PkgConfig::PLATFORM_GST
        ${CMAKE_DL_LIBS}
    )
    tesseract_target_pch(tesseract_platform
        <gst/gst.h>
        <glib.h>
    )
endif()

if(TESSERACT_UI STREQUAL "win32")
    tesseract_add_cppwinrt_includes(tesseract_platform)
    target_compile_definitions(tesseract_platform PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        # GetDpiForWindow + per-monitor v2 awareness require Windows 10 (1607).
        _WIN32_WINNT=0x0A00)
    if(MSVC)
        # C++/WinRT headers on Windows SDK 10.0.19041 predate /permissive-'s
        # two-phase lookup rules; relax it for the one TU that uses them.
        set_source_files_properties(win32/location_provider_win32.cpp
            PROPERTIES COMPILE_OPTIONS "/Zc:twoPhase-")
    endif()
    # Same /utf-8 as the shell target; see the comment in ui/windows/CMakeLists.txt.
    if(MSVC)
        target_compile_options(tesseract_platform PRIVATE /utf-8)
    endif()
    target_link_libraries(tesseract_platform PUBLIC
        mf mfplat mfreadwrite mfuuid shlwapi d3d11 dxgi windowsapp)
elseif(TESSERACT_UI STREQUAL "macos")
    # The shells compile with target-wide -fobjc-arc; do the same per file here.
    # The PCH is built without ARC and ObjC++ can't share a unity blob with the
    # C++ sources, so these files opt out of both.
    set_source_files_properties(
        macos/video_capture_macos.mm
        macos/screen_capture_macos.mm
        macos/location_provider_macos.mm
        macos/video_decode_av.mm
        PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
            SKIP_PRECOMPILE_HEADERS ON
            SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(tesseract_platform PUBLIC
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        "-framework CoreLocation"
        "-weak_framework ScreenCaptureKit")
    # Mirrors ui/macos/CMakeLists.txt: file:line crash frames for the moved
    # backends too (experimental, off by default).
    if(TESSERACT_ENABLE_CRASH_HANDLER)
        target_compile_options(tesseract_platform PRIVATE "$<$<CONFIG:Release>:-gline-tables-only>")
    endif()
endif()
