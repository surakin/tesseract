cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GIO  REQUIRED gio-2.0)
# gdk-pixbuf + cairo render the tray icon. Both are standalone (they do NOT
# pull GTK), so the tray can live in-process. The system tray is a pure
# org.kde.StatusNotifierItem implementation over GDBus — see GtkSniTrayIcon.
# (The old libayatana-appindicator3 tray was dropped because it is GTK3 and
# loading libgtk-3 into a GTK4 process makes gtk_init() abort.)
pkg_check_modules(GDKPIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(CAIRO     REQUIRED cairo)
# PangoCairo: text layout for tk::Canvas's Cairo/Pango backend (tk/canvas_cairo.cpp).
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)

add_executable(tesseract_gtk
    src/main.cpp
    src/MainWindow.cpp
    src/RoomWindow.cpp
    src/LoginView.cpp
    src/SettingsWidget.cpp
    src/LinuxNotifier.cpp
    src/LinuxScreenLockGtk.cpp
    src/LinuxUpConnectorGtk.cpp
    src/GtkSniTrayIcon.cpp
    # tk:: backend implementation — GTK4/Cairo-specific, so it lives here
    # rather than in ui/shared (which holds only genuinely cross-platform
    # code). GStreamer support (video_decode_gst.cpp, gst_hw_probe.cpp) is
    # shared between this and ui/linux-qt, and stays linked into
    # tesseract_tk — its PUBLIC link to GStreamer propagates here too.
    tk/canvas_cairo.h
    tk/canvas_cairo.cpp
    tk/host_gtk.h
    tk/host_gtk.cpp
    tk/audio_gtk.cpp
    tk/audio_capture_gtk.cpp
    tk/audio_playback_gtk.cpp
    tk/video_gtk.cpp
)

if(TESSERACT_ENABLE_CALLS)
    target_sources(tesseract_gtk PRIVATE
        src/CallWindow.h
        src/CallWindow.cpp
    )
endif()

target_compile_features(tesseract_gtk PRIVATE cxx_std_20)

# GTK/Cairo/pixbuf umbrella headers + std base. (GStreamer lives in
# tesseract_tk, not the shell sources, so it's not in this PCH.)
tesseract_target_pch(tesseract_gtk
    <string>
    <vector>
    <cstring>
    <algorithm>
    <filesystem>
    <gtk/gtk.h>
    <gdk-pixbuf/gdk-pixbuf.h>
    <cairo.h>
    <libintl.h>
)

target_include_directories(tesseract_gtk PRIVATE
    src
    # src/*.cpp #include the GTK backend as "tk/host_gtk.h" / "tk/canvas_cairo.h";
    # those headers live under ui/linux-gtk/tk, so this dir is the include base.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # tk/host_gtk.cpp etc. bare-#include genuinely-shared headers (e.g.
    # "anim_image_cache.h") that still live in ui/shared/tk — this makes
    # those resolve without rewriting every #include in the moved files.
    "${CMAKE_SOURCE_DIR}/ui/shared/tk"
    ${GTK4_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GDKPIXBUF_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGOCAIRO_INCLUDE_DIRS}
)

target_link_directories(tesseract_gtk PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${GIO_LIBRARY_DIRS}
    ${GDKPIXBUF_LIBRARY_DIRS}
    ${CAIRO_LIBRARY_DIRS}
    ${PANGOCAIRO_LIBRARY_DIRS}
)

target_compile_options(tesseract_gtk PRIVATE
    ${GTK4_CFLAGS_OTHER}
    ${GIO_CFLAGS_OTHER}
)

target_link_libraries(tesseract_gtk PRIVATE
    # All three static archives are mutually dependent (client ↔ bridge ↔ ffi).
    # Link them as whole-archive so every symbol is unconditionally present,
    # removing all ordering constraints between them. Mirrors ui/linux-qt.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    ${GTK4_LIBRARIES}
    ${GIO_LIBRARIES}
    ${GDKPIXBUF_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGOCAIRO_LIBRARIES}
    opus
)

set_target_properties(tesseract_gtk PROPERTIES
    OUTPUT_NAME "tesseract"
)

# ---------------------------------------------------------------------------
# Application icon
# ---------------------------------------------------------------------------
# Copy the SVG into the icon-theme directory structure inside the build tree
# so gtk_icon_theme_add_search_path() can locate it at runtime.
set(TESSERACT_ICON_SEARCH_PATH "${CMAKE_CURRENT_BINARY_DIR}/icons")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg"
    "${TESSERACT_ICON_SEARCH_PATH}/hicolor/scalable/apps/tesseract.svg"
    COPYONLY
)
target_compile_definitions(tesseract_gtk PRIVATE
    TESSERACT_ICON_SEARCH_PATH="${TESSERACT_ICON_SEARCH_PATH}"
)

install(TARGETS tesseract_gtk
        RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg"
        DESTINATION share/icons/hicolor/scalable/apps)
install(FILES "${CMAKE_BINARY_DIR}/packaging/tesseract.desktop"
        DESTINATION share/applications)

# Rename installed artifacts from `tesseract` to `tesseract-matrix-gtk` so
# this package can coexist with the unrelated Tesseract OCR engine and with
# the Qt6 variant (tesseract-matrix).
install(CODE [[
    set(_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    set(_new  "tesseract-matrix-gtk")
    if(EXISTS "${_root}/bin/tesseract")
        file(RENAME "${_root}/bin/tesseract"
                    "${_root}/bin/${_new}")
    endif()
    if(EXISTS "${_root}/share/applications/tesseract.desktop")
        file(READ  "${_root}/share/applications/tesseract.desktop" _d)
        string(REPLACE "Exec=tesseract %U"        "Exec=${_new} %U"          _d "${_d}")
        string(REPLACE "Icon=tesseract"           "Icon=${_new}"             _d "${_d}")
        string(REPLACE "StartupWMClass=tesseract" "StartupWMClass=${_new}"   _d "${_d}")
        file(WRITE  "${_root}/share/applications/${_new}.desktop" "${_d}")
        file(REMOVE "${_root}/share/applications/tesseract.desktop")
    endif()
    if(EXISTS "${_root}/share/icons/hicolor/scalable/apps/tesseract.svg")
        file(RENAME "${_root}/share/icons/hicolor/scalable/apps/tesseract.svg"
                    "${_root}/share/icons/hicolor/scalable/apps/${_new}.svg")
    endif()
]])

# i18n: use the root i18n-extract target (cmake/I18n.cmake) which scans all
# source directories including this one. Run: cmake --build . --target i18n-extract