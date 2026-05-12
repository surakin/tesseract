# Generate Tesseract.icns from the shared SVG at build time.
set(_svg  "${CMAKE_CURRENT_SOURCE_DIR}/../icons/tesseract.svg")
set(_set  "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.iconset")
set(_icns "${CMAKE_CURRENT_BINARY_DIR}/Tesseract.icns")

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

# All three archives have circular dependencies through the FFI; link them
# with WHOLE_ARCHIVE so every symbol is present regardless of order.
target_link_libraries(tesseract_macos PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework CoreGraphics"
)
