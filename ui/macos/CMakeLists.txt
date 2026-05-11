cmake_minimum_required(VERSION 3.25)

enable_language(OBJCXX)

add_executable(tesseract_macos MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.h
    src/AppDelegate.mm
    src/EventBridge.h
    src/EventBridge.mm
    src/AvatarCache.h
    src/AvatarCache.mm
    src/MainWindowController.h
    src/MainWindowController.mm
    src/LoginWindowController.h
    src/LoginWindowController.mm
    src/RoomListController.h
    src/RoomListController.mm
    src/MessageListController.h
    src/MessageListController.mm
    src/ComposeBar.h
    src/ComposeBar.mm
)

target_compile_features(tesseract_macos PRIVATE cxx_std_20)
target_compile_options(tesseract_macos  PRIVATE -fobjc-arc)
target_include_directories(tesseract_macos PRIVATE src)

set_target_properties(tesseract_macos PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    OUTPUT_NAME "Tesseract"
)

# All three archives have circular dependencies through the FFI; link them
# with WHOLE_ARCHIVE so every symbol is present regardless of order.
target_link_libraries(tesseract_macos PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
)
