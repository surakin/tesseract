cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

add_executable(matrix_gtk
    src/main.cpp
    src/MainWindow.cpp
)

target_compile_features(matrix_gtk PRIVATE cxx_std_20)

target_include_directories(matrix_gtk PRIVATE
    src
    ${GTK4_INCLUDE_DIRS}
)

target_link_directories(matrix_gtk PRIVATE ${GTK4_LIBRARY_DIRS})

target_compile_options(matrix_gtk PRIVATE ${GTK4_CFLAGS_OTHER})

target_link_libraries(matrix_gtk PRIVATE
    matrix_client
    ${GTK4_LIBRARIES}
)

set_target_properties(matrix_gtk PROPERTIES
    OUTPUT_NAME "MatrixClient-gtk")
