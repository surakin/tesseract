# ---------------------------------------------------------------------------
# CPack-driven installer packaging for Tesseract.
#
# Windows: NSIS .exe — Start Menu shortcut + uninstaller.
# macOS:   DragNDrop .dmg — drag-to-Applications volume.
#
# Build with:  cmake --build build/<preset> --target package
# ---------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME                "Tesseract")
set(CPACK_PACKAGE_VENDOR              "Tesseract")
set(CPACK_PACKAGE_VERSION             "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR       "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR       "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH       "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tesseract — cross-platform Matrix client")
set(CPACK_PACKAGE_INSTALL_DIRECTORY   "Tesseract")
set(CPACK_PACKAGE_CONTACT             "https://github.com/surakin/tesseract")

# GPLv3 license text. Setting this makes the NSIS installer show a license
# agreement page (and the macOS .dmg a license panel) before installing.
set(CPACK_RESOURCE_FILE_LICENSE       "${CMAKE_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_FILE_NAME           "Tesseract-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_NSIS_PACKAGE_NAME           "Tesseract")
    set(CPACK_NSIS_DISPLAY_NAME           "Tesseract")
    set(CPACK_NSIS_MUI_ICON               "${CMAKE_BINARY_DIR}/ui/windows/Tesseract.ico")
    set(CPACK_NSIS_MUI_UNIICON            "${CMAKE_BINARY_DIR}/ui/windows/Tesseract.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME    "bin\\\\Tesseract.exe")
    set(CPACK_NSIS_HELP_LINK              "https://github.com/surakin/tesseract")
    set(CPACK_NSIS_URL_INFO_ABOUT         "https://github.com/surakin/tesseract")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH            OFF)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Tesseract.lnk' '$INSTDIR\\\\bin\\\\Tesseract.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Tesseract.lnk'")

    # Installer switch /DEFAULT_HOMESERVER=<host> (works with /S): written to
    # HKLM\Software\Tesseract-Matrix\DefaultHomeserver, read by the app's login view
    # (client/include/tesseract/provisioning.h).
    #
    # Plain NSIS (no FileFunc macros, no ${...}): CPack expands ${...} as CMake
    # variables, and CPACK_NSIS_DEFINES is generator-owned so FileFunc.nsh can't
    # be included. CPack writes this value into CPackConfig.cmake inside a quoted
    # string without escaping, so it must hold no double quote (the value is
    # therefore taken unquoted, up to the next space) and every backslash must
    # already be doubled.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [==[
    Push $R0
    Push $R1
    Push $R2
    Push $R3
    StrCpy $R0 $CMDLINE
    StrLen $R1 $R0
    StrCpy $R2 0
    hs_find:
    IntCmp $R2 $R1 hs_done hs_cont hs_done
    hs_cont:
    StrCpy $R3 $R0 20 $R2
    StrCmp $R3 '/DEFAULT_HOMESERVER=' hs_found
    IntOp $R2 $R2 + 1
    Goto hs_find
    hs_found:
    IntOp $R2 $R2 + 20
    StrCpy $R0 $R0 '' $R2
    StrCpy $R2 0
    hs_scan:
    StrCpy $R3 $R0 1 $R2
    StrCmp $R3 '' hs_write
    StrCmp $R3 ' ' hs_write
    IntOp $R2 $R2 + 1
    Goto hs_scan
    hs_write:
    StrCpy $R0 $R0 $R2
    StrCmp $R0 '' hs_done
    SetRegView 64
    WriteRegStr HKLM 'Software\\Tesseract-Matrix' 'DefaultHomeserver' $R0
    SetRegView 32
    hs_done:
    Pop $R3
    Pop $R2
    Pop $R1
    Pop $R0
]==])
    # Silent uninstall (/S) is what an upgrade runs (uninstall-before-install)
    # and what managed tooling uses: keep the provisioning then, so an upgrade
    # does not silently reset the fleet to matrix.org. Only an interactive
    # uninstall removes it.
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "SetRegView 64
    IfSilent hs_keep
    DeleteRegValue HKLM 'Software\\\\Tesseract-Matrix' 'DefaultHomeserver'
    DeleteRegKey /ifempty HKLM 'Software\\\\Tesseract-Matrix'
    hs_keep:
    SetRegView 32")
endif()

if(APPLE)
    set(CPACK_GENERATOR        "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME  "Tesseract ${PROJECT_VERSION}")
    set(CPACK_DMG_FORMAT       "UDZO")
    # CMAKE_OSX_ARCHITECTURES may be a list ("arm64;x86_64") or empty if the
    # preset didn't set it — fall back to CMAKE_SYSTEM_PROCESSOR.
    if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" "-" _arch_tag "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_arch_tag "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(CPACK_PACKAGE_FILE_NAME "Tesseract-${PROJECT_VERSION}-${_arch_tag}")

    # Vendored subprojects (Corrosion's CMake helper scripts, for one) install
    # unconditionally into the staging prefix's include/, lib/, and share/
    # trees, with no install-disable option. Nothing the app itself ships lives
    # in those three directories, so deleting them right before CPack archives
    # the prefix keeps those dev artifacts out of the DMG without touching
    # vendored code. This install(CODE) is appended after every other
    # install() in the build (ui/macos's included last, via add_subdirectory
    # above), so it runs last during `cmake --install`.
    install(CODE "
        file(REMOVE_RECURSE
            \"\${CMAKE_INSTALL_PREFIX}/include\"
            \"\${CMAKE_INSTALL_PREFIX}/lib\"
            \"\${CMAKE_INSTALL_PREFIX}/share\")
    ")
    
    # Mac App Store .pkg (appstore-pkg target); no-op unless TESSERACT_MAC_APP_STORE.
    include(${CMAKE_CURRENT_LIST_DIR}/AppStore.cmake)

    # Optional notarization wrapper. Signs the .dmg CPack produces with the
    # Developer ID identity (the app inside is already signed by the install
    # step), runs `xcrun notarytool submit --wait`, then `xcrun stapler staple`.
    # Needs TESSERACT_MAC_CODESIGN_IDENTITY; TESSERACT_MAC_NOTARIZE_KEYCHAIN is
    # passed as --keychain when the profile lives outside the default keychain.
    # Invoke with:  cmake --build build/<preset> --target notarize
    if(TESSERACT_MAC_NOTARIZE_PROFILE)
        if(NOT TESSERACT_MAC_CODESIGN_IDENTITY)
            message(FATAL_ERROR
                "TESSERACT_MAC_NOTARIZE_PROFILE requires TESSERACT_MAC_CODESIGN_IDENTITY "
                "(notarization needs a Developer ID signed app and DMG)")
        endif()
        set(_dmg_path "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.dmg")
        set(_notary_keychain_args)
        if(TESSERACT_MAC_NOTARIZE_KEYCHAIN)
            set(_notary_keychain_args --keychain "${TESSERACT_MAC_NOTARIZE_KEYCHAIN}")
        endif()
        add_custom_target(notarize
            COMMAND codesign --force --timestamp
                    --sign "${TESSERACT_MAC_CODESIGN_IDENTITY}" "${_dmg_path}"
            COMMAND xcrun notarytool submit "${_dmg_path}"
                    --keychain-profile "${TESSERACT_MAC_NOTARIZE_PROFILE}"
                    ${_notary_keychain_args}
                    --wait
            COMMAND xcrun stapler staple "${_dmg_path}"
            COMMENT "Signing, notarizing and stapling ${_dmg_path}"
            VERBATIM
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM")

    # Avoid clashing with the unrelated Tesseract OCR engine (/usr/bin/tesseract).
    # The binary is renamed by install(CODE) in the UI CMakeLists; package names
    # follow the same convention.  The two UI variants conflict with each other
    # because only one Matrix client should provide the desktop entry.
    if(TESSERACT_UI STREQUAL "gtk")
        set(_cpack_pkg       "tesseract-matrix-gtk")
        set(_cpack_conflicts "tesseract-matrix")
        set(_cpack_summary   "Matrix chat client (GTK4)")
        set(_cpack_deb_deps  "libgtk-4-1, libopus0 (>= 1.1), gstreamer1.0-plugins-base, fonts-noto-color-emoji")
        set(_cpack_rpm_deps  "gtk4 >= 4.0, opus >= 1.1, gstreamer1 >= 1.0, google-noto-color-emoji-fonts")
    else()
        set(_cpack_pkg       "tesseract-matrix")
        set(_cpack_conflicts "tesseract-matrix-gtk")
        set(_cpack_summary   "Matrix chat client (Qt6)")
        set(_cpack_deb_deps
            "libqt6core6 (>= 6.2), libqt6gui6 (>= 6.2), libqt6widgets6 (>= 6.2), \
libqt6multimedia6 (>= 6.2), libqt6dbus6 (>= 6.2), libqt6network6 (>= 6.2), \
libqt6concurrent6 (>= 6.2), \
qt6-image-formats-plugins, libopus0 (>= 1.1), fonts-noto-color-emoji")
        set(_cpack_rpm_deps  "qt6-qtbase >= 6.2, qt6-qtmultimedia >= 6.2, qt6-qtimageformats, opus >= 1.1, google-noto-color-emoji-fonts")
    endif()
    if(TESSERACT_WITH_ENGINE)
        # The 3D engine library (cmake/Engine.cmake) links the window-system libraries and loads
        # the Vulkan loader at run time.
        string(APPEND _cpack_deb_deps ", libvulkan1, libwayland-client0, libxkbcommon0, libx11-6, libxcursor1, libxi6, libxfixes3")
        string(APPEND _cpack_rpm_deps ", vulkan-loader, libwayland-client, libxkbcommon, libX11, libXcursor, libXi, libXfixes")
    endif()
    set(_cpack_description
"Tesseract is a cross-platform Matrix chat client with end-to-end encryption, \
voice messages (MSC3245), image packs (MSC2545), replies, message editing, \
threads, spaces, and sticker support. This package provides the ${_cpack_summary}.")

    # DEB (Debian / Ubuntu)
    set(CPACK_DEBIAN_PACKAGE_NAME        "${_cpack_pkg}")
    set(CPACK_DEBIAN_PACKAGE_CONFLICTS   "${_cpack_conflicts}")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${_cpack_description}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER  "Tesseract <https://github.com/<TBD>>")
    set(CPACK_DEBIAN_PACKAGE_SECTION     "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY    "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS     "${_cpack_deb_deps}")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS   OFF)  # we list deps explicitly above

    # RPM (Fedora / RHEL / openSUSE)
    set(CPACK_RPM_PACKAGE_NAME           "${_cpack_pkg}")
    set(CPACK_RPM_PACKAGE_SUMMARY        "${_cpack_summary}")
    set(CPACK_RPM_PACKAGE_CONFLICTS      "${_cpack_conflicts}")
    set(CPACK_RPM_PACKAGE_DESCRIPTION    "${_cpack_description}")
    set(CPACK_RPM_PACKAGE_GROUP          "Applications/Internet")
    set(CPACK_RPM_PACKAGE_LICENSE        "GPLv3")
    set(CPACK_RPM_PACKAGE_REQUIRES       "${_cpack_rpm_deps}")
    set(CPACK_RPM_PACKAGE_AUTOREQ        OFF)   # we list requires explicitly above
endif()

include(CPack)
