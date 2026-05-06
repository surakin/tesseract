cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

add_executable(tesseract_gtk
    src/main.cpp
    src/MainWindow.cpp
    src/LoginDialog.cpp
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
    ${GTK4_LIBRARIES}
)

set_target_properties(tesseract_gtk PROPERTIES
    OUTPUT_NAME "tesseract"
)