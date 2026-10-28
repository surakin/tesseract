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

# Rasterise with resvg (single Rust binary, `cargo install resvg`; same
# renderer the Windows icon generator uses) or librsvg's rsvg-convert.
find_program(TESSERACT_RESVG resvg)
find_program(TESSERACT_RSVG_CONVERT rsvg-convert)
if(NOT TESSERACT_RESVG AND NOT TESSERACT_RSVG_CONVERT)
    message(FATAL_ERROR
        "Generating Tesseract.icns needs resvg (`cargo install resvg`) or rsvg-convert (librsvg)")
endif()

set(_icon_commands)
foreach(_entry
        "16|icon_16x16"     "32|icon_16x16@2x"
        "32|icon_32x32"     "64|icon_32x32@2x"
        "128|icon_128x128"  "256|icon_128x128@2x"
        "256|icon_256x256"  "512|icon_256x256@2x"
        "512|icon_512x512"  "1024|icon_512x512@2x")
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _px)
    list(GET _entry 1 _name)
    if(TESSERACT_RESVG)
        list(APPEND _icon_commands COMMAND "${TESSERACT_RESVG}"
            -w ${_px} -h ${_px} "${_svg}" "${_set}/${_name}.png")
    else()
        list(APPEND _icon_commands COMMAND "${TESSERACT_RSVG_CONVERT}"
            -w ${_px} -h ${_px} "${_svg}" -o "${_set}/${_name}.png")
    endif()
endforeach()

add_custom_command(
    OUTPUT "${_icns}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_set}"
    ${_icon_commands}
    COMMAND iconutil -c icns "${_set}" -o "${_icns}"
    DEPENDS "${_svg}"
    COMMENT "Generating Tesseract.icns"
    VERBATIM
)

add_executable(tesseract_macos MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.h
    src/AppDelegate.mm
    src/MainWindow.h
    src/MainWindow.mm
    src/MainWindowController.h
    src/MainWindowController.mm
    src/MacOSTrayIcon.h
    src/MacOSTrayIcon.mm
    src/MacNowPlaying.h
    src/MacNowPlaying.mm
    src/MacSpotlightSearch.h
    src/MacSpotlightSearch.mm
    src/MacScreenLock.h
    src/MacScreenLock.mm
    src/MacPowerMonitor.h
    src/MacPowerMonitor.mm
    src/MacAutostart.h
    src/MacAutostart.mm
    src/MacRelaunchHandoff.h
    src/MacRelaunchHandoff.mm
    src/RoomWindowController.h
    src/RoomWindowController.mm
    "${_icns}"
    # tk:: backend implementation — AppKit/CoreGraphics-specific, so it
    # lives here rather than in ui/shared (which holds only genuinely
    # cross-platform code).
    tk/canvas_cg.h
    tk/canvas_cg.cpp
    tk/canvas_cg_webp.h
    tk/canvas_cg_webp.cpp
    tk/host_macos.h
    tk/host_macos.mm
    tk/macos_accessible.h
    tk/macos_accessible.mm
    tk/audio_macos.mm
    tk/audio_capture_macos.mm
    tk/audio_playback_macos.mm
    tk/video_macos.mm
    tk/video_decode_av.mm
    tk/video_capture_macos.mm
)

target_sources(tesseract_macos PRIVATE
    src/AuxWindowController.h
    src/AuxWindowController.mm
    src/CallWindowController.h
    src/CallWindowController.mm
    tk/screen_capture_macos.mm
    tk/location_provider_macos.mm
)

set_source_files_properties("${_icns}" PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
    GENERATED TRUE
)

target_compile_features(tesseract_macos PRIVATE cxx_std_20)
target_compile_options(tesseract_macos  PRIVATE -fobjc-arc)

# video_macos.mm predates ARC and manually manages refcounts (explicit
# alloc/retain/release/dealloc) — it compiled fine under MRC back when it
# lived in tesseract_tk (never ARC-enabled); now that it's a direct source of
# this -fobjc-arc target, override ARC back off for just this file rather
# than rewriting its memory management.
# SKIP_PRECOMPILE_HEADERS: the target's ObjC++ PCH is built with ARC (the
# target-wide -fobjc-arc above), which conflicts with this file's -fno-objc-arc.
set_source_files_properties(tk/video_macos.mm PROPERTIES
    COMPILE_OPTIONS -fno-objc-arc
    SKIP_PRECOMPILE_HEADERS ON)

# Std-only PCH (compiled as ObjC++ for these .mm sources). AppKit/Foundation
# are intentionally excluded — ObjC umbrella frameworks in a PCH are brittle
# and add little, and project headers are excluded for incremental-build speed.
tesseract_target_pch(tesseract_macos
    <memory>
    <string>
    <vector>
    <algorithm>
    <functional>
)

target_include_directories(tesseract_macos PRIVATE
    src
    # src/*.mm #include the AppKit backend as "tk/host_macos.h" /
    # "tk/canvas_cg.h"; those headers live under ui/macos/tk, so this dir
    # is the include base.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # tk/host_macos.mm etc. bare-#include genuinely-shared headers (e.g.
    # "anim_image_cache.h") that still live in ui/shared/tk — this makes
    # those resolve without rewriting every #include in the moved files.
    "${CMAKE_SOURCE_DIR}/ui/shared/tk"
)

# Info.plist substitutions. CFBundleVersion must be unique per App Store
# Connect upload, so CI passes TESSERACT_MAC_BUILD_NUMBER (github.run_number).
set(TESSERACT_MAC_BUILD_NUMBER "" CACHE STRING
    "CFBundleVersion override (App Store Connect rejects a repeated build number); empty = project version")
if(TESSERACT_MAC_BUILD_NUMBER)
    set(TESSERACT_MAC_BUNDLE_VERSION "${TESSERACT_MAC_BUILD_NUMBER}")
else()
    set(TESSERACT_MAC_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()
set(TESSERACT_MAC_APPSTORE_PLIST_KEYS "")
if(TESSERACT_MAC_APP_STORE)
    # MAINTAINER: this value must match the export-compliance answer given in
    # App Store Connect. Matrix E2EE uses standard algorithms, which usually
    # qualifies for the exemption (false), but confirm before uploading.
    set(TESSERACT_MAC_APPSTORE_PLIST_KEYS
        "    <key>ITSAppUsesNonExemptEncryption</key><false/>")
endif()

set_target_properties(tesseract_macos PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_ICON_FILE  "Tesseract"
    OUTPUT_NAME              "Tesseract"
    BUILD_RPATH              "@executable_path/../Frameworks"
    INSTALL_RPATH            "@executable_path/../Frameworks"
)

# Release has no -g today (see the root CMakeLists.txt Linux equivalent), so
# Apple Clang never emits a dSYM for it. -gline-tables-only gives the crash
# handler file:line resolution without the size cost of full debug info, and
# dsymutil bundles it into a .dSYM a developer can hold onto to symbolicate a
# crash report sent back by a user running that exact build. Gated behind
# TESSERACT_ENABLE_CRASH_HANDLER (root CMakeLists.txt) — experimental, off by
# default.
if(TESSERACT_ENABLE_CRASH_HANDLER)
    target_compile_options(tesseract_macos PRIVATE "$<$<CONFIG:Release>:-gline-tables-only>")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_command(TARGET tesseract_macos POST_BUILD
            COMMAND dsymutil $<TARGET_BUNDLE_EXECUTABLE:tesseract_macos>
                    -o $<TARGET_BUNDLE_DIR:tesseract_macos>.dSYM
            COMMENT "Generating dSYM for crash symbolication")
    endif()
endif()

# opus: audiopus_sys (the Rust crate) emits a link-lib=opus directive that
# the C++ linker must satisfy. Homebrew installs opus to a non-standard
# prefix, so find_library locates it before the exe link step runs.
find_library(OPUS_LIB NAMES opus
    HINTS "$ENV{OPUS_LIB_DIR}" /opt/homebrew/lib /usr/local/lib
    REQUIRED)

# Resolve the Homebrew symlink to get the real versioned filename, then read
# the embedded LC_ID_DYLIB (the string the linker writes into LC_LOAD_DYLIB
# entries in the exe — may differ from the symlink path we found above).
get_filename_component(_opus_real "${OPUS_LIB}" REALPATH)
get_filename_component(_opus_filename "${_opus_real}" NAME)
execute_process(
    COMMAND otool -D "${_opus_real}"
    OUTPUT_VARIABLE _opus_otool
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX REPLACE "^[^\n]*\n" "" _opus_id "${_opus_otool}")
string(STRIP "${_opus_id}" _opus_id)

# dyld refuses a library whose minimum OS is newer than the running system, so
# a libopus built for the build machine (Homebrew's is) makes the bundle
# unlaunchable below that version whatever LSMinimumSystemVersion says. CI
# builds opus with -DCMAKE_OSX_DEPLOYMENT_TARGET to match and fails on this.
execute_process(
    COMMAND vtool -show-build "${_opus_real}"
    OUTPUT_VARIABLE _opus_build
    ERROR_QUIET)
string(REGEX MATCH "minos ([0-9.]+)" _ "${_opus_build}")
set(TESSERACT_OPUS_MINOS "${CMAKE_MATCH_1}")
if(TESSERACT_OPUS_MINOS AND TESSERACT_OPUS_MINOS VERSION_GREATER CMAKE_OSX_DEPLOYMENT_TARGET)
    message(WARNING
        "${_opus_real} requires macOS ${TESSERACT_OPUS_MINOS}, newer than the "
        "deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}: the app will not launch "
        "below ${TESSERACT_OPUS_MINOS}. Build opus with "
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} for a shippable bundle.")
endif()

# All three archives have circular dependencies through the FFI; link them
# with WHOLE_ARCHIVE so every symbol is present regardless of order.
target_link_libraries(tesseract_macos PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    tesseract_tk
    "-framework AppKit"
    "-framework Carbon"                   # tk/host_macos.mm TIS input sources (Cmd shortcuts on non-Latin layouts)
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"                 # tk/canvas_cg.cpp text layout
    "-framework ImageIO"                  # tk/canvas_cg.cpp image decode
    "-framework UniformTypeIdentifiers"   # tk/canvas_cg.cpp / host_macos.mm
    "-framework CoreMedia"
    "-framework AVFoundation"
    "-framework CoreVideo"
    "-framework CoreAudio"                # tk/audio_capture_macos.mm, host_macos.mm
    "-framework AudioToolbox"             # tk/audio_capture_macos.mm, host_macos.mm
    "-framework UserNotifications"
    "-framework CoreLocation"             # tk/location_provider_macos.mm
    "-framework ServiceManagement"        # src/MacAutostart.mm (SMAppService)
    "-framework MediaPlayer"              # src/MacNowPlaying.mm
    "-framework CoreSpotlight"            # src/MacSpotlightSearch.mm
    "-framework SystemConfiguration"      # tk/host_macos.mm is_network_available()
    "-framework IOKit"                    # src/MacPowerMonitor.mm (IOPowerSources)
)
if(TESSERACT_LD_NO_WARN_DUPLICATE_LIBRARIES)
    target_link_options(tesseract_macos PRIVATE "LINKER:-no_warn_duplicate_libraries")
endif()


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
set(TESSERACT_MAC_NOTARIZE_KEYCHAIN "" CACHE STRING
    "keychain file holding the notarytool profile (passed as --keychain) — empty = default keychain")
set(_tesseract_entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Tesseract.entitlements")

# Copy libopus into Contents/Frameworks and rewrite all install-name references so
# the bundle is self-contained and works on machines without Homebrew.
add_custom_command(TARGET tesseract_macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_DIR:tesseract_macos>/Contents/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_opus_real}"
        "$<TARGET_BUNDLE_DIR:tesseract_macos>/Contents/Frameworks/${_opus_filename}"
    COMMAND install_name_tool -id
        "@rpath/${_opus_filename}"
        "$<TARGET_BUNDLE_DIR:tesseract_macos>/Contents/Frameworks/${_opus_filename}"
    COMMAND install_name_tool -change
        "${_opus_id}"
        "@rpath/${_opus_filename}"
        "$<TARGET_BUNDLE_DIR:tesseract_macos>/Contents/MacOS/Tesseract"
    COMMENT "Bundling ${_opus_filename} into Tesseract.app/Contents/Frameworks"
    VERBATIM
)

if(TESSERACT_MAC_APP_STORE)
    set(TESSERACT_MAC_APPSTORE_APP_IDENTITY "" CACHE STRING
        "App Store signing identity (e.g. 'Apple Distribution: Foo (TEAMID)')")
    set(TESSERACT_MAC_APPSTORE_INSTALLER_IDENTITY "" CACHE STRING
        "Installer identity for the .pkg (e.g. '3rd Party Mac Developer Installer: Foo (TEAMID)')")
    set(TESSERACT_MAC_PROVISIONING_PROFILE "" CACHE FILEPATH
        "Path to the Mac App Store .provisionprofile embedded in the bundle")
    set(TESSERACT_MAC_TEAM_ID "" CACHE STRING
        "Apple developer team id (prefix of the application-identifier entitlement)")

    target_compile_definitions(tesseract_macos PRIVATE TESSERACT_MAC_APP_STORE)

    set(_bundle "$<TARGET_BUNDLE_DIR:tesseract_macos>")
    set(_as_ids "")
    if(TESSERACT_MAC_APPSTORE_APP_IDENTITY AND TESSERACT_MAC_PROVISIONING_PROFILE
       AND TESSERACT_MAC_TEAM_ID)
        set(_as_signed TRUE)
        set(_as_sign_id "${TESSERACT_MAC_APPSTORE_APP_IDENTITY}")
        set(_as_timestamp --timestamp)
        set(_as_ids
"    <key>com.apple.application-identifier</key><string>${TESSERACT_MAC_TEAM_ID}.im.gnomos.tesseract</string>
    <key>com.apple.developer.team-identifier</key><string>${TESSERACT_MAC_TEAM_ID}</string>")
    else()
        # No certificates: ad-hoc sign with the sandbox entitlements only, so
        # sandbox behaviour can be tested locally. No timestamp for ad-hoc.
        set(_as_signed FALSE)
        set(_as_sign_id "-")
        set(_as_timestamp "")
    endif()
    set(TESSERACT_MAC_APPSTORE_ENTITLEMENT_IDS "${_as_ids}")
    set(_as_entitlements "${CMAKE_CURRENT_BINARY_DIR}/Tesseract-AppStore.entitlements")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Tesseract-AppStore.entitlements.in"
                   "${_as_entitlements}" @ONLY)

    # Declared after the libopus bundling POST_BUILD above (commands run in
    # declaration order). libopus is signed on its own: --deep is discouraged
    # for App Store submissions. The build-dir bundle is what appstore-pkg
    # packages; `cmake --install` would rewrite rpaths and break this signature.
    set(_as_cmds
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/container-migration.plist"
            "${_bundle}/Contents/Resources/container-migration.plist")
    if(_as_signed)
        list(APPEND _as_cmds
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TESSERACT_MAC_PROVISIONING_PROFILE}"
                "${_bundle}/Contents/embedded.provisionprofile")
    endif()
    add_custom_command(TARGET tesseract_macos POST_BUILD
        ${_as_cmds}
        COMMAND codesign --force ${_as_timestamp} --options runtime
                --sign "${_as_sign_id}"
                "${_bundle}/Contents/Frameworks/${_opus_filename}"
        COMMAND codesign --force ${_as_timestamp} --options runtime
                --entitlements "${_as_entitlements}"
                --sign "${_as_sign_id}" "${_bundle}"
        COMMENT "Signing Tesseract.app for the Mac App Store (identity: ${_as_sign_id})"
        VERBATIM
    )
else()
# Sign the build-dir app after every build so macOS accepts UNUserNotificationCenter
# permission requests. Uses the real identity when one is configured, ad-hoc otherwise.
# --deep covers Contents/Frameworks/ so libopus.dylib is signed alongside the exe.
# With a real identity the app is signed with the hardened runtime (required for
# notarization) and the entitlements in Tesseract.entitlements: the hardened
# runtime blocks microphone, camera and location unless they are granted there.
# The build-dir and installed signatures use the same options so they match.
if(TESSERACT_MAC_CODESIGN_IDENTITY)
    set(_tesseract_sign_id "${TESSERACT_MAC_CODESIGN_IDENTITY}")
    set(_tesseract_sign_opts --options runtime --entitlements "${_tesseract_entitlements}")
else()
    set(_tesseract_sign_id "-")
    set(_tesseract_sign_opts)
endif()
add_custom_command(TARGET tesseract_macos POST_BUILD
    COMMAND codesign --force --deep ${_tesseract_sign_opts} --sign "${_tesseract_sign_id}"
            "$<TARGET_BUNDLE_DIR:tesseract_macos>"
    COMMENT "Signing Tesseract.app (identity: ${_tesseract_sign_id})"
    VERBATIM
)

# Re-sign after install. CMake's bundle install rewrites build-dir rpaths via
# install_name_tool, which invalidates the POST_BUILD ad-hoc signature. Without
# a re-sign here the DMG ships a broken signature that the Apple Silicon kernel
# rejects at launch. COMMAND_ERROR_IS_FATAL makes a failed sign fail the install.
if(TESSERACT_MAC_CODESIGN_IDENTITY)
    install(CODE "
        execute_process(COMMAND codesign --force --deep --options runtime --timestamp
            --entitlements \"${_tesseract_entitlements}\"
            --sign \"${TESSERACT_MAC_CODESIGN_IDENTITY}\"
            \"\${CMAKE_INSTALL_PREFIX}/Tesseract.app\"
            COMMAND_ERROR_IS_FATAL ANY)
    " COMPONENT Application)
else()
    install(CODE "
        execute_process(COMMAND codesign --force --deep --sign \"-\"
            \"\${CMAKE_INSTALL_PREFIX}/Tesseract.app\"
            COMMAND_ERROR_IS_FATAL ANY)
    " COMPONENT Application)
endif()
endif()

# Bundle .mo translation catalogs into Resources/i18n
tesseract_bundle_mo_files(tesseract_macos)
