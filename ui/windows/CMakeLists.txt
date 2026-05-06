cmake_minimum_required(VERSION 3.25)

add_executable(tesseract_win32 WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/LoginDialog.cpp
)

target_compile_features(tesseract_win32 PRIVATE cxx_std_20)

target_include_directories(tesseract_win32 PRIVATE src)

target_link_libraries(tesseract_win32 PRIVATE
    tesseract_client
    comctl32
)

target_compile_definitions(tesseract_win32 PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(tesseract_win32 PROPERTIES
    O