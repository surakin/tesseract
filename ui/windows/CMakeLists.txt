cmake_minimum_required(VERSION 3.25)

add_executable(matrix_win32 WIN32
    src/main.cpp
    src/MainWindow.cpp
)

target_compile_features(matrix_win32 PRIVATE cxx_std_20)

target_include_directories(matrix_win32 PRIVATE src)

target_link_libraries(matrix_win32 PRIVATE
    matrix_client
    comctl32
)

target_compile_definitions(matrix_win32 PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(matrix_win32 PROPERTIES
    OUTPUT_NAME "MatrixClient")
