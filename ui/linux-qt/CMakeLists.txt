cmake_minimum_required(VERSION 3.25)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia)
qt_standard_project_setup()

qt_add_executable(tesseract_qt
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h          # MOC input
    src/LoginView.cpp
    src/LoginView.h           # MOC input
    src/EmojiPicker.cpp
    src/EmojiPicker.h         # MOC input
    src/StickerPicker.cpp
    src/StickerPicker.h       # MOC input
)

target_compile_features(tesseract_qt PRIVATE cxx_std_20)

target_include_directories(tesseract_qt PRIVATE src)

target_link_libraries(tesseract_qt PRIVATE
    # All three static archives are mutually dependent (client ↔ bridge ↔ ffi).
    # Link them as whole-archive so every symbol is unconditionally present,
    # removing all ordering constraints between them.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
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

install(TARGETS tesseract_qt
        RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg"
        DESTINATION share/icons/hicolor/scalable/apps)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/tesseract.desktop"
        DESTINATION share/applications)

# i18n: extract translatable strings to a Qt .ts template (run manually).
find_program(LUPDATE_EXECUTABLE lupdate lupdate-qt6 NAMES lupdate lupdate-qt6)
if(LUPDATE_EXECUTABLE)
    add_custom_target(i18n_extract_qt
        COMMAND ${LUPDATE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            -ts ${CMAKE_SOURCE_DIR}/i18n/qt/tesseract_LANG.ts
            -no-obsolete
        COMMENT "Extract Qt translation strings (rename tesseract_LANG.ts to your locale)"
        VERBATIM
    )
endif()