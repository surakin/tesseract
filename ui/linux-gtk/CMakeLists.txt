cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GIO  REQUIRED gio-2.0)
pkg_check_modules(AYATANA_APPINDICATOR REQUIRED IMPORTED_TARGET ayatana-appindicator3-0.1)

# libayatana-appindicator3 is a GTK3 library. The tray code is compiled in
# its own static library so the GTK3 include paths stay isolated from the
# GTK4 translation units. See LinuxGtkTrayIcon.{h,cpp} for the type-erased
# header used by the rest of the shell.
add_library(tesseract_gtk_tray STATIC
    src/LinuxGtkTrayIcon.cpp
    src/LinuxGtkTrayIcon.h
)
target_compile_features(tesseract_gtk_tray PRIVATE cxx_std_20)
target_include_directories(tesseract_gtk_tray PRIVATE
    ${CMAKE_SOURCE_DIR}/client/include
)
target_link_libraries(tesseract_gtk_tray PRIVATE
    PkgConfig::AYATANA_APPINDICATOR
)
target_compile_definitions(tesseract_gtk_tray PRIVATE
    TESSERACT_ICON_SEARCH_PATH="${CMAKE_CURRENT_BINARY_DIR}/icons"
)

add_executable(tesseract_gtk
    src/main.cpp
    src/MainWindow.cpp
    src/RoomWindow.cpp
    src/LoginView.cpp
    src/SettingsWidget.cpp
    src/LinuxNotifier.cpp
    src/LinuxUpConnectorGtk.cpp
)

target_compile_features(tesseract_gtk PRIVATE cxx_std_20)

target_include_directories(tesseract_gtk PRIVATE
    src
    ${GTK4_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
)

target_link_directories(tesseract_gtk PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${GIO_LIBRARY_DIRS}
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
    tesseract_gtk_tray
    PkgConfig::AYATANA_APPINDICATOR  # transitive runtime: appindicator + GTK3
    ${GTK4_LIBRARIES}
    ${GIO_LIBRARIES}
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
install(FILES "${CMAKE_SOURCE_DIR}/packaging/tesseract.desktop"
        DESTINATION share/applications)

# i18n: extract translatable strings to a gettext .pot template (run manually).
find_program(XGETTEXT_EXECUTABLE xgettext)
if(XGETTEXT_EXECUTABLE)
    add_custom_target(i18n_extract_gtk
        COMMAND ${XGETTEXT_EXECUTABLE}
            --keyword=_ --keyword=N_
            --language=C++ --from-code=UTF-8
            --package-name=tesseract
            --output=${CMAKE_SOURCE_DIR}/i18n/gtk/tesseract.pot
            ${CMAKE_CURRENT_SOURCE_DIR}/src/MainWindow.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/LoginView.cpp
        COMMENT "Extract GTK translation strings to i18n/gtk/tesseract.pot"
        VERBATIM
    )
endif()