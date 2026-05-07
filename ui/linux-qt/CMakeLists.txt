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
    # All three static archives are mutually dependent (client ↔ bridge ↔ ffi).
    # Link them as whole-archive so every symbol is unconditionally present,
    # removing all ordering constraints between them.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    Qt6::Core
    Qt6::Widgets
)

set_target_properties(tesseract_qt PROPERTIES
    OUTPUT_NAME "tesseract"
)

# Embed the application icon as a Qt resource so the binary is self-contained.
qt_add_resources(tesseract_qt "tesseract_icons"
    PREFIX "/icons"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}/../icons"
    FILES  "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg"
)