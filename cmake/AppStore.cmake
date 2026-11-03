# Mac App Store .pkg. Included from Installers.cmake (APPLE only). The app
# bundle itself is signed by ui/macos/CMakeLists.txt; this adds the installer
# package target, defined only when both identities and the profile are set.
# CMake stops at a signed .pkg: validation and upload happen in CI.
if(TESSERACT_MAC_APP_STORE AND TESSERACT_MAC_APPSTORE_APP_IDENTITY
   AND TESSERACT_MAC_APPSTORE_INSTALLER_IDENTITY AND TESSERACT_MAC_PROVISIONING_PROFILE
   AND TESSERACT_MAC_TEAM_ID AND TARGET tesseract_macos)
    set(_appstore_pkg "${CMAKE_BINARY_DIR}/Tesseract-${PROJECT_VERSION}-appstore.pkg")
    # Apple .symbols for App Store Connect's server-side crash symbolication,
    # generated from the dSYM (passed explicitly: CI runners may lack Spotlight)
    # and embedded via productbuild's undocumented --symbolication option.
    # Skipped when TESSERACT_MAC_STRIP is off (local builds), which makes no dSYM.
    set(_appstore_symbols "${CMAKE_BINARY_DIR}/appstore-symbols")
    set(_appstore_symbols_cmds "")
    set(_appstore_symbols_args "")
    if(TESSERACT_MAC_STRIP)
        set(_appstore_symbols_cmds
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${_appstore_symbols}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_appstore_symbols}"
            COMMAND xcrun symbols -noTextInSOD -noDaemon -arch all
                    -symbolsPackageDir "${_appstore_symbols}"
                    "$<TARGET_BUNDLE_DIR:tesseract_macos>.dSYM")
        set(_appstore_symbols_args --symbolication "${_appstore_symbols}")
    endif()
    add_custom_target(appstore-pkg
        DEPENDS tesseract_macos
        COMMAND ${CMAKE_COMMAND} -E remove -f "${_appstore_pkg}"
        ${_appstore_symbols_cmds}
        COMMAND productbuild --component "$<TARGET_BUNDLE_DIR:tesseract_macos>" /Applications
                ${_appstore_symbols_args}
                --sign "${TESSERACT_MAC_APPSTORE_INSTALLER_IDENTITY}" "${_appstore_pkg}"
        COMMAND pkgutil --check-signature "${_appstore_pkg}"
        COMMENT "Building ${_appstore_pkg}"
        VERBATIM
    )
endif()
