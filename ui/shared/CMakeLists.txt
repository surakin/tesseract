cmake_minimum_required(VERSION 3.25)

# tesseract_tk — the shared UI toolkit core. Cross-platform widget classes,
# layout, theme, and the abstract Canvas interface (see tk/canvas.h).
#
# Backend translation units (Direct2D on Win32, CoreGraphics on macOS,
# QPainter on Qt6, Cairo+Pango on GTK4) live in this same target but are
# compiled conditionally based on TESSERACT_UI.

set(_tk_sources
    tk/canvas.h
    tk/host.h
    tk/theme.h
    tk/theme.cpp
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
    views/LoginView.h
    views/LoginView.cpp
    views/RoomListView.h
    views/RoomListView.cpp
    views/MessageListView.h
    views/MessageListView.cpp
    views/EmojiPicker.h
    views/EmojiPicker.cpp
    views/RecoveryBanner.h
    views/RecoveryBanner.cpp
    views/ComposeBar.h
    views/ComposeBar.cpp
)

if(TESSERACT_UI STREQUAL "win32")
    list(APPEND _tk_sources
        tk/canvas_d2d.h
        tk/canvas_d2d.cpp
        tk/host_win32.h
        tk/host_win32.cpp
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    list(APPEND _tk_sources
        tk/canvas_qpainter.h
        tk/canvas_qpainter.cpp
        tk/host_qt.h
        tk/host_qt.cpp
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    list(APPEND _tk_sources
        tk/canvas_cairo.h
        tk/canvas_cairo.cpp
        tk/host_gtk.h
        tk/host_gtk.cpp
    )
elseif(TESSERACT_UI STREQUAL "macos")
    list(APPEND _tk_sources
        tk/canvas_cg.h
        tk/canvas_cg.cpp
        tk/host_macos.h
        tk/host_macos.mm
    )
endif()

add_library(tesseract_tk STATIC ${_tk_sources})

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
        # ID2D1Factory1 / IDWriteFactory2 / WIC require Windows 8.1+.
        _WIN32_WINNT=0x0603
    )
    target_link_libraries(tesseract_tk PUBLIC
        d2d1
        dwrite
        windowscodecs    # WIC: PNG/JPEG decode for tk::Image
        ole32
        comctl32         # SetWindowSubclass, EM_SETCUEBANNER, etc.
        user32
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
    target_link_libraries(tesseract_tk PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TK_GTK4 REQUIRED IMPORTED_TARGET
        gtk4
        pangocairo
        gdk-pixbuf-2.0
    )
    target_link_libraries(tesseract_tk PUBLIC
        PkgConfig::TK_GTK4
    )
elseif(TESSERACT_UI STREQUAL "macos")
    target_link_libraries(tesseract_tk PUBLIC
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
        "-framework AppKit"
        "-framework Foundation"
    )
endif()
