cmake_minimum_required(VERSION 3.25)

# tesseract_tk — the shared UI toolkit core. Cross-platform widget classes,
# layout, theme, and the abstract Canvas interface (see tk/canvas.h).
#
# Backend translation units (Direct2D on Win32, CoreGraphics on macOS,
# QPainter on Qt6, Cairo+Pango on GTK4) live in this same target but are
# compiled conditionally based on TESSERACT_UI.

set(_tk_sources
    app/ShellBase.h
    app/ShellBase.cpp
    app/EventHandlerBase.h
    app/EventHandlerBase.cpp
    app/RoomWindowBase.h
    app/RoomWindowBase.cpp
    tk/audio.h
    tk/video.h
    tk/canvas.h
    tk/host.h
    tk/theme.h
    tk/theme.cpp
    tk/anim_image_cache.h
    tk/anim_image_cache.cpp
    tk/widget.h
    tk/widget.cpp
    tk/layout.h
    tk/layout.cpp
    tk/controls.h
    tk/controls.cpp
    tk/image_view.h
    tk/image_view.cpp
    tk/list_view.h
    tk/list_view.cpp
    tk/tab_bar.h
    tk/tab_bar.cpp
    tk/side_tab_view.h
    tk/side_tab_view.cpp
    tk/blurhash.h
    tk/blurhash.cpp
    tk/media_disk_cache.h
    tk/media_disk_cache.cpp
    views/BrandView.h
    views/BrandView.cpp
    views/LoginView.h
    views/LoginView.cpp
    views/RoomListView.h
    views/RoomListView.cpp
    views/MessageListView.h
    views/MessageListView.cpp
    views/html_spans.h
    views/html_spans.cpp
    views/EmojiPicker.h
    views/EmojiPicker.cpp
    views/StickerPicker.h
    views/StickerPicker.cpp
    views/RecoveryBanner.h
    views/RecoveryBanner.cpp
    views/VerificationBanner.h
    views/VerificationBanner.cpp
    views/RoomHeader.h
    views/RoomHeader.cpp
    views/RoomView.h
    views/RoomView.cpp
    views/ComposeBar.h
    views/ComposeBar.cpp
    views/UserInfo.h
    views/UserInfo.cpp
    views/AccountPicker.h
    views/AccountPicker.cpp
    views/ImageViewerOverlay.h
    views/ImageViewerOverlay.cpp
    views/VideoViewerOverlay.h
    views/VideoViewerOverlay.cpp
    views/ShortcodeEngine.h
    views/ShortcodeEngine.cpp
    views/ShortcodePopup.h
    views/ShortcodePopup.cpp
    views/JoinRoomView.h
    views/JoinRoomView.cpp
    views/MainAppWidget.h
    views/MainAppWidget.cpp
    views/SettingsView.h
    views/SettingsView.cpp
    views/settings/AccountSection.h
    views/settings/AccountSection.cpp
    views/settings/AppearanceSection.h
    views/settings/AppearanceSection.cpp
    views/settings/NotificationsSection.h
    views/settings/NotificationsSection.cpp
)

if(TESSERACT_UI STREQUAL "win32")
    list(APPEND _tk_sources
        tk/canvas_d2d.h
        tk/canvas_d2d.cpp
        tk/host_win32.h
        tk/host_win32.cpp
        tk/audio_win32.cpp
        tk/video_win32.cpp
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    list(APPEND _tk_sources
        tk/canvas_qpainter.h
        tk/canvas_qpainter.cpp
        tk/host_qt.h
        tk/host_qt.cpp
        tk/audio_qt.cpp
        tk/video_qt.cpp
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    list(APPEND _tk_sources
        tk/canvas_cairo.h
        tk/canvas_cairo.cpp
        tk/host_gtk.h
        tk/host_gtk.cpp
        tk/audio_gtk.cpp
        tk/video_gtk.cpp
    )
elseif(TESSERACT_UI STREQUAL "macos")
    list(APPEND _tk_sources
        tk/canvas_cg.h
        tk/canvas_cg.cpp
        tk/host_macos.h
        tk/host_macos.mm
        tk/audio_macos.mm
        tk/video_macos.mm
    )
endif()

add_library(tesseract_tk STATIC ${_tk_sources})

# Embed the application icon PNG as a C byte array so BrandView can decode it
# cross-platform without per-shell resource loading. Runs at configure time.
set(_icon_png "${CMAKE_SOURCE_DIR}/ui/icons/tesseract_160.png")
if(EXISTS "${_icon_png}")
    file(READ "${_icon_png}" _icon_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _icon_bytes "${_icon_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/brand_icon.h"
        "#pragma once\n"
        "#include <cstdint>\n\n"
        "static constexpr std::uint8_t kBrandIconPng[] = { ${_icon_bytes} };\n"
    )
    target_compile_definitions(tesseract_tk PRIVATE TESSERACT_HAS_BRAND_ICON=1)
endif()
target_include_directories(tesseract_tk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_features(tesseract_tk PUBLIC cxx_std_20)

target_include_directories(tesseract_tk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The toolkit interacts with shared visual constants and (eventually) the
# client event-handler interface.
target_link_libraries(tesseract_tk PUBLIC
    tesseract_client
)

if(TESSERACT_UI STREQUAL "win32")
    target_compile_definitions(tesseract_tk PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        # GetDpiForWindow requires Windows 10 (1607); D2D/DWrite/WIC are
        # available from 8.1, so 0x0A00 is the floor for the toolkit.
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(tesseract_tk PUBLIC
        d2d1
        dwrite
        windowscodecs    # WIC: PNG/JPEG decode for tk::Image
        ole32
        comctl32         # SetWindowSubclass, EM_SETCUEBANNER, etc.
        user32
        mf               # Media Foundation: IMFMediaEngine (audio_win32.cpp)
        mfplat
        mfuuid
        shlwapi          # SHCreateMemStream (also used in host_win32.cpp)
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    # Multimedia is used by audio_qt.cpp for MSC3245 voice playback
    # (QMediaPlayer + QAudioOutput).
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
    target_link_libraries(tesseract_tk PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Multimedia
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TK_GTK4 REQUIRED IMPORTED_TARGET
        gtk4
        pangocairo
        gdk-pixbuf-2.0
    )
    # GStreamer + gstreamer-app for MSC3245 voice playback (audio_gtk.cpp).
    # The opus decoder is provided by gst-plugins-base; package this runtime
    # dependency in installer manifests.
    pkg_check_modules(TK_GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
    )
    target_link_libraries(tesseract_tk PUBLIC
        PkgConfig::TK_GTK4
        PkgConfig::TK_GST
    )
elseif(TESSERACT_UI STREQUAL "macos")
    target_link_libraries(tesseract_tk PUBLIC
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
        "-framework AppKit"
        "-framework Foundation"
        # MSC3245 voice playback (audio_macos.mm) uses AVAudioPlayer.
        "-framework AVFoundation"
        "-framework AudioToolbox"
    )
endif()
