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
set(CPACK_PACKAGE_CONTACT             "https://github.com/<TBD>")

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
    set(CPACK_NSIS_HELP_LINK              "https://github.com/<TBD>")
    set(CPACK_NSIS_URL_INFO_ABOUT         "https://github.com/<TBD>")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH            OFF)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Tesseract.lnk' '$INSTDIR\\\\bin\\\\Tesseract.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Tesseract.lnk'")
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

    # Optional notarization wrapper. Runs `xcrun notarytool submit --wait`
    # followed by `xcrun stapler staple` against the .dmg CPack produces.
    # Invoke with:  cmake --build build/<preset> --target notarize
    if(TESSERACT_MAC_NOTARIZE_PROFILE)
        set(_dmg_path "${CMAKE_BINARY_DIR}/${CPACK_PACKAGE_FILE_NAME}.dmg")
        add_custom_target(notarize
            COMMAND xcrun notarytool submit "${_dmg_path}"
                    --keychain-profile "${TESSERACT_MAC_NOTARIZE_PROFILE}"
                    --wait
            COMMAND xcrun stapler staple "${_dmg_path}"
            COMMENT "Notarizing and stapling ${_dmg_path}"
            VERBATIM
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM")

    # DEB (Debian / Ubuntu)
    set(CPACK_DEBIAN_PACKAGE_NAME        "tesseract")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER  "Tesseract <https://github.com/<TBD>>")
    set(CPACK_DEBIAN_PACKAGE_SECTION     "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY    "optional")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libqt6core6 (>= 6.2), libqt6gui6 (>= 6.2), libqt6widgets6 (>= 6.2), \
libqt6multimedia6 (>= 6.2), libqt6dbus6 (>= 6.2), libqt6network6 (>= 6.2), \
libopus0 (>= 1.1)")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS   OFF)  # we list deps explicitly above

    # RPM (Fedora / RHEL / openSUSE)
    set(CPACK_RPM_PACKAGE_NAME           "tesseract")
    set(CPACK_RPM_PACKAGE_GROUP          "Applications/Internet")
    set(CPACK_RPM_PACKAGE_LICENSE        "GPLv3")
    set(CPACK_RPM_PACKAGE_REQUIRES
        "qt6-qtbase >= 6.2, qt6-qtmultimedia >= 6.2, opus >= 1.1")
    set(CPACK_RPM_PACKAGE_AUTOREQ        OFF)   # we list requires explicitly above
endif()

include(CPack)
