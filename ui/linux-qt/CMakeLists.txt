cmake_minimum_required(VERSION 3.25)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

qt_add_executable(matrix_qt
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h   # MOC input
)

target_compile_features(matrix_qt PRIVATE cxx_std_20)

target_include_directories(matrix_qt PRIVATE src)

target_link_libraries(matrix_qt PRIVATE
    matrix_client
    Qt6::Core
    Qt6::Widgets
)

set_target_properties(matrix_qt PROPERTIES
    OUTPUT_NAME "MatrixClient-qt"
    AUTOMOC ON)
