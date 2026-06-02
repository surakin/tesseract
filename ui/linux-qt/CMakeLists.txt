cmake_minimum_required(VERSION 3.25)

set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia DBus Concurrent Network)
# qt_standard_project_setup() requires Qt >= 6.3; Ubuntu 22.04 ships 6.2.4.
# Set the equivalent properties manually so the build works on both.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---------------------------------------------------------------------------
# Optional: xdg-activation-v1 Wayland protocol binding.
#
# Qt 6.11 removed _q_waylandActivationToken with no public replacement.
# We call xdg_activation_v1_activate() directly, getting wl_display* and
# wl_surface* from QPlatformNativeInterface.
#
# The protocol glue (src/xdg-activation-v1-protocol.[ch]) was generated once
# from wayland-protocols 1.25 and checked in; it only needs updating if the
# protocol itself changes (it is stable at version 1).
#
# GuiPrivate (qt6-base-private-dev) is only needed for the native interface
# calls used here; look for it separately so the build degrades gracefully
# on systems that only have the public Qt6 dev packages.
# ---------------------------------------------------------------------------
find_library(WAYLAND_CLIENT_LIB wayland-client)
set(_xdg_sources "")
if(WAYLAND_CLIENT_LIB)
    find_package(Qt6 OPTIONAL_COMPONENTS GuiPrivate)
    if(Qt6GuiPrivate_FOUND)
        set(_xdg_sources src/xdg-activation-v1-protocol.c)
    endif()
endif()

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

# Qt module umbrella headers dominate per-file parse cost in the shell. Applied
# only to tesseract_qt; the separate C `tesseract_xdg_protocol` target gets no
# PCH. (Qt's AUTOMOC mocs_compilation.cpp shares this PCH fine.)
tesseract_target_pch(tesseract_qt
    <string>
    <memory>
    <vector>
    <utility>
    <QtCore/QtCore>
    <QtGui/QtGui>
    <QtWidgets/QtWidgets>
    <QtDBus/QtDBus>
    <QtMultimedia/QtMultimedia>
)

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
    Qt6::Network
    opus
)

if(WAYLAND_CLIENT_LIB AND Qt6GuiPrivate_FOUND)
    # The root project only enables LANGUAGES CXX. Enable C here so we can
    # compile the wayland-scanner generated .c protocol glue.
    enable_language(C)
    # qt_add_executable / AUTOMOC (enabled globally by qt_standard_project_setup)
    # silently drops plain .c sources. Compile the Wayland protocol glue into
    # its own static lib with Qt autogen disabled.
    add_library(tesseract_xdg_protocol STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/xdg-activation-v1-protocol.c")
    set_target_properties(tesseract_xdg_protocol PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_compile_definitions(tesseract_qt PRIVATE HAVE_XDG_ACTIVATION)
    target_link_libraries(tesseract_qt PRIVATE
        tesseract_xdg_protocol "${WAYLAND_CLIENT_LIB}"
        Qt6::GuiPrivate)
endif()

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
