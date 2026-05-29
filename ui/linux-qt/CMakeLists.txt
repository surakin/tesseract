cmake_minimum_required(VERSION 3.25)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia DBus Concurrent)
qt_standard_project_setup()

qt_add_executable(tesseract_qt
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h          # MOC input
    src/RoomWindow.cpp
    src/RoomWindow.h          # MOC input
    src/LoginView.cpp
    src/LoginView.h           # MOC input
    src/SettingsWidget.cpp
    src/SettingsWidget.h      # MOC input
    src/EmojiPicker.cpp
    src/EmojiPicker.h         # MOC input
    src/StickerPicker.cpp
    src/StickerPicker.h       # MOC input
    src/JoinRoomDialog.cpp
    src/JoinRoomDialog.h      # MOC input
    src/LinuxNotifier.cpp
    src/LinuxNotifier.h       # MOC input
    src/LinuxScreenLockQt.cpp
    src/LinuxScreenLockQt.h   # MOC input
    src/LinuxUpConnectorQt.cpp
    src/LinuxQtTrayIcon.cpp
    src/LinuxQtTrayIcon.h     # MOC input
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
    Qt6::DBus
    Qt6::Concurrent
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

# i18n: use the root i18n-extract target (cmake/I18n.cmake) which scans all
# source directories including this one. Run: cmake --build . --target i18n-extract