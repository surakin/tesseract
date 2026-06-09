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
)

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
    ${GTK4_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GDKPIXBUF_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
)

target_link_directories(tesseract_gtk PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${GIO_LIBRARY_DIRS}
    ${GDKPIXBUF_LIBRARY_DIRS}
    ${CAIRO_LIBRARY_DIRS}
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

# i18n: use the root i18n-extract target (cmake/I18n.cmake) which scans all
# source directories including this one. Run: cmake --build . --target i18n-extract