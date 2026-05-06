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
    tesseract_client
    ${GTK4_LIBRARIES}
)

set_target_properties(tesseract_gtk PROPERTIES
    OUTPUT_NAME "tesseract"
)