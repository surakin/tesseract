cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

add_executable(tesseract_gtk
    src/main.cpp
    src/MainWindow.cpp
    src/LoginView.cpp
)

target_compile_features(tesseract_gtk PRIVATE cxx_std_20)

target_include_directories(tesseract_gtk PRIVATE
    src
    ${GTK4_INCLUDE_DIRS}
)

target_link_directories(tesseract_gtk PRIVATE ${GTK4_LIBRARY_DIRS})

target_compile_options(tesseract_gtk PRIVATE ${GTK4_CFLAGS_OTHER})

target_link_libraries(tesseract_gtk PRIVATE
    # All three static archives are mutually dependent (client ↔ bridge ↔ ffi).
    # Link them as whole-archive so every symbol is unconditionally present,
    # removing all ordering constraints between them. Mirrors ui/linux-qt.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    ${GTK4_LIBRARIES}
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