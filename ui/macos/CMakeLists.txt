# Generate Tesseract.icns from the shared SVG at build time.
# Patch the viewBox at configure time to add 1.5/16-per-side transparent border
# so the Dock icon matches the visual weight of other macOS app icons.
set(_svg_src "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg")
set(_svg     "${CMAKE_CURRENT_BINARY_DIR}/tesseract_icon.svg")
set(_set     "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.iconset")
set(_icns    "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.icns")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_svg_src}")
file(READ "${_svg_src}" _svg_raw)
string(REPLACE
    "viewBox=\"0 0 512 512\""
    "viewBox=\"-59.077 -59.077 630.154 630.154\""
    _svg_patched "${_svg_raw}")
file(WRITE "${_svg}" "${_svg_patched}")

add_custom_command(
    OUTPUT "${_icns}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_set}"
    COMMAND rsvg-convert -w 16   -h 16   "${_svg}" -o "${_set}/icon_16x16.png"
    COMMAND rsvg-convert -w 32   -h 32   "${_svg}" -o "${_set}/icon_16x16@2x.png"
    COMMAND rsvg-convert -w 32   -h 32   "${_svg}" -o "${_set}/icon_32x32.png"
    COMMAND rsvg-convert -w 64   -h 64   "${_svg}" -o "${_set}/icon_32x32@2x.png"
    COMMAND rsvg-convert -w 128  -h 128  "${_svg}" -o "${_set}/icon_128x128.png"
    COMMAND rsvg-convert -w 256  -h 256  "${_svg}" -o "${_set}/icon_128x128@2x.png"
    COMMAND rsvg-convert -w 256  -h 256  "${_svg}" -o "${_set}/icon_256x256.png"
    COMMAND rsvg-convert -w 512  -h 512  "${_svg}" -o "${_set}/icon_256x256@2x.png"
    COMMAND rsvg-convert -w 512  -h 512  "${_svg}" -o "${_set}/icon_512x512.png"
    COMMAND rsvg-convert -w 1024 -h 1024 "${_svg}" -o "${_set}/icon_512x512@2x.png"
    COMMAND iconutil -c icns "${_set}" -o "${_icns}"
    DEPENDS "${_svg}"
    COMMENT "Generating Tesseract.icns"
    VERBATIM
)

add_executable(tesseract_macos MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.h
    src/AppDelegate.mm
    src/MainWindowController.h
    src/MainWindowController.mm
    src/LoginView.h
    src/LoginView.mm
    src/EmojiPicker.h
    src/EmojiPicker.mm
    src/StickerPicker.h
    src/StickerPicker.mm
    src/MacOSTrayIcon.h
    src/MacOSTrayIcon.mm
    src/MacScreenLock.h
    src/MacScreenLock.mm
    src/RoomWindowController.h
    src/RoomWindowController.mm
    "${_icns}"
)

set_source_files_properties("${_icns}" PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
    GENERATED TRUE
)

target_compile_features(tesseract_macos PRIVATE cxx_std_20)
target_compile_options(tesseract_macos  PRIVATE -fobjc-arc)
target_include_directories(tesseract_macos PRIVATE src)

set_target_properties(tesseract_macos PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_ICON_FILE  "Tesseract"
    OUTPUT_NAME "Tesseract"
)

# opus: audiopus_sys (the Rust crate) emits a link-lib=opus directive that
# the C++ linker must satisfy. Homebrew installs opus to a non-standard
# prefix, so find_library locates it before the exe link step runs.
find_library(OPUS_LIB NAMES opus
    HINTS "$ENV{OPUS_LIB_DIR}" /opt/homebrew/lib /usr/local/lib
    REQUIRED)

# All three archives have circular dependencies through the FFI; link them
# with WHOLE_ARCHIVE so every symbol is present regardless of order.
target_link_libraries(tesseract_macos PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreMedia"
    "-framework AVFoundation"
    "-framework CoreVideo"
    "-framework UserNotifications"
)

# ---------------------------------------------------------------------------
# Installer (CPack DragNDrop) — see cmake/Installers.cmake for generator
# config. BUNDLE DESTINATION "." drops Tesseract.app at the DMG root next
# to the Applications-folder symlink CPack adds automatically.
# ---------------------------------------------------------------------------
install(TARGETS tesseract_macos
        BUNDLE  DESTINATION .
        COMPONENT Application)

set(TESSERACT_MAC_CODESIGN_IDENTITY "" CACHE STRING
    "codesign identity (e.g. 'Developer ID Application: Foo (TEAMID)') — empty = ad-hoc signing")
set(TESSERACT_MAC_NOTARIZE_PROFILE "" CACHE STRING
    "notarytool keychain profile name — empty = no notarization")

# Sign the build-dir app after every build so macOS accepts UNUserNotificationCenter
# permission requests. Uses the real identity when one is configured, ad-hoc otherwise.
if(TESSERACT_MAC_CODESIGN_IDENTITY)
    set(_tesseract_sign_id "${TESSERACT_MAC_CODESIGN_IDENTITY}")
else()
    set(_tesseract_sign_id "-")
endif()
add_custom_command(TARGET tesseract_macos POST_BUILD
    COMMAND codesign --force --deep --sign "${_tesseract_sign_id}"
            "$<TARGET_BUNDLE_DIR:tesseract_macos>"
    COMMENT "Signing Tesseract.app (identity: ${_tesseract_sign_id})"
    VERBATIM
)

if(TESSERACT_MAC_CODESIGN_IDENTITY)
    install(CODE "
        execute_process(COMMAND codesign --force --options runtime --timestamp
            --sign \"${TESSERACT_MAC_CODESIGN_IDENTITY}\"
            \"\${CMAKE_INSTALL_PREFIX}/Tesseract.app\")
    " COMPONENT Application)
endif()

# Bundle .mo translation catalogs into Resources/i18n
tesseract_bundle_mo_files(tesseract_macos)
