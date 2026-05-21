cmake_minimum_required(VERSION 3.25)

find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Multimedia DBus Concurrent Network)
qt_standard_project_setup()

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
# ---------------------------------------------------------------------------
find_library(WAYLAND_CLIENT_LIB wayland-client)
set(_xdg_sources "")
if(WAYLAND_CLIENT_LIB)
    set(_xdg_sources src/xdg-activation-v1-protocol.c)
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

if(WAYLAND_CLIENT_LIB)
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
