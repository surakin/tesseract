cmake_minimum_required(VERSION 3.25)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

qt_add_executable(tesseract_qt
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h     # MOC input
    src/LoginDialog.cpp
    src/LoginDialog.h    # MOC input
)

target_compile_features(tesseract_qt PRIVATE cxx_std_20)

target_include_directories(tesseract_qt PRIVATE src)

target_link_libraries(tesseract_qt PRIVATE
    tesseract_client
    Qt6::Core
    Qt6::Widgets
)

set_target_properties(tesserac