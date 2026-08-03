# ---------------------------------------------------------------------------
# MSIX packaging for Windows — Store and direct-distribution editions built
# from the same MSVC release binary. NSIS (cmake/Installers.cmake) remains
# the primary Windows installer; this is an additional, side-by-side output.
#
# MSVC-only, matching the existing WinRT-only guard around toast
# notifications (ui/windows/src/Win32Notifier.cpp gets a no-op stub under
# MinGW) — MakeAppx/SignTool and the packaged runtime paths this enables
# (Win32PackageContext, Win32Autostart, Win32Taskbar) only make sense for a
# real MSVC release build. MakeAppx.exe is located via the registry (see
# TESSERACT_MAKEAPPX below), so unlike SignTool it does not require a
# Developer Command Prompt / vcvarsall to already be on PATH.
#
# Targets:
#   msix-stage                     - stage both editions' payload + manifest
#   msix-pack-store / msix-pack-direct / msix-pack
#   msix-sign                      - SignTool the direct edition (reuses the
#                                     NSIS installer's PFX cache vars)
#   msix-validate                  - MakeAppx unpack round-trip both editions
#
# Build with:
#   cmake --build build/<preset> --target msix-stage msix-pack msix-validate
# ---------------------------------------------------------------------------

if(NOT WIN32 OR NOT MSVC)
    return()
endif()

set(TESSERACT_MSIX_STORE_IDENTITY_NAME "Tesseract" CACHE STRING
    "MSIX Identity/@Name for the Microsoft Store edition")
set(TESSERACT_MSIX_DIRECT_IDENTITY_NAME "TesseractDirect" CACHE STRING
    "MSIX Identity/@Name for the direct-distribution edition")
set(TESSERACT_MSIX_STORE_PUBLISHER "CN=TesseractStorePlaceholder" CACHE STRING
    "MSIX Identity/@Publisher subject for the Store edition (Partner Center assigns the real value on reservation)")
set(TESSERACT_MSIX_PUBLISHER "CN=Tesseract Dev, O=Tesseract, C=US" CACHE STRING
    "MSIX Identity/@Publisher subject for the direct-distribution edition (must match the signing cert's subject)")
set(TESSERACT_MSIX_PUBLISHER_DISPLAY_NAME "Tesseract" CACHE STRING
    "MSIX Properties/PublisherDisplayName shown to users")
set(TESSERACT_MSIX_UPDATE_URI
    "https://github.com/${TESSERACT_GITHUB_REPO}/releases/latest/download"
    CACHE STRING "Base URL App Installer uses for direct-edition update checks/downloads")

# MSIX requires a 4-component version; project(... VERSION ...) is always
# 3-component (see root CMakeLists.txt), so padding with a literal ".0" is
# sufficient here — this mirrors win32::package_context::msix_version() at
# the CMake level rather than calling into that C++ helper.
set(TESSERACT_MSIX_VERSION "${PROJECT_VERSION}.0")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Locate MakeAppx.exe via the registry (same Windows Kits root lookup the
# MSVC env-forwarding block in the root CMakeLists.txt uses for SDK headers)
# instead of requiring a Developer Command Prompt / vcvarsall to have put it
# on PATH. Falls back to the bare command name — relying on PATH — if the
# registry lookup or the file itself can't be found, so an unusual SDK
# layout doesn't hard-fail configure.
set(TESSERACT_MAKEAPPX "" CACHE FILEPATH
    "Path to MakeAppx.exe. Empty = auto-detect via the Windows Kits registry root, falling back to PATH.")
if(NOT TESSERACT_MAKEAPPX)
    cmake_host_system_information(RESULT _winsdk_root QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10" VIEW BOTH)
    if(_winsdk_root)
        file(GLOB _winsdk_bin_dirs LIST_DIRECTORIES true "${_winsdk_root}bin/10.*")
        list(SORT _winsdk_bin_dirs ORDER DESCENDING)
        foreach(_dir IN LISTS _winsdk_bin_dirs)
            if(EXISTS "${_dir}/x64/makeappx.exe")
                set(TESSERACT_MAKEAPPX "${_dir}/x64/makeappx.exe")
                break()
            endif()
        endforeach()
    endif()
    if(NOT TESSERACT_MAKEAPPX)
        message(WARNING
            "MakeAppx.exe not found under the Windows Kits registry root — "
            "falling back to PATH. Run from a Developer Command Prompt, or "
            "set -DTESSERACT_MAKEAPPX=<path> explicitly, if msix-pack/msix-validate fail.")
        set(TESSERACT_MAKEAPPX "MakeAppx")
    endif()
endif()
message(STATUS "MSIX: using MakeAppx at ${TESSERACT_MAKEAPPX}")

set(_msix_src_dir   "${CMAKE_CURRENT_SOURCE_DIR}/ui/windows/msix")
set(_msix_build_dir "${CMAKE_BINARY_DIR}/msix")
set(_assets_script  "${CMAKE_CURRENT_SOURCE_DIR}/ui/icons/generate_msix_assets.py")
set(_assets_svg     "${CMAKE_CURRENT_SOURCE_DIR}/ui/icons/tesseract.svg")
set(_assets_dir     "${_msix_build_dir}/Assets")

add_custom_command(
    OUTPUT "${_assets_dir}/Square44x44Logo.png"
           "${_assets_dir}/Square150x150Logo.png"
           "${_assets_dir}/StoreLogo.png"
           "${_assets_dir}/Wide310x150Logo.png"
           "${_assets_dir}/SplashScreen.png"
    COMMAND ${Python3_EXECUTABLE} "${_assets_script}" "${_assets_dir}" "${_assets_svg}"
    DEPENDS "${_assets_script}" "${_assets_svg}"
    COMMENT "Generating MSIX visual assets from tesseract.svg"
    VERBATIM
)
add_custom_target(msix-assets DEPENDS "${_assets_dir}/Square44x44Logo.png")

# Stages one edition (store|direct): configures its manifest, copies the
# shared payload (exe, assets, i18n, license) into its own directory — MakeAppx
# packs a single directory tree, so the two editions each need their own copy
# rather than sharing one staging dir with two manifests.
function(_tesseract_msix_edition edition identity_name publisher)
    set(_stage "${_msix_build_dir}/${edition}")
    # Kept as a sibling of the staging dir, not inside it — MakeAppx packs
    # every file under _stage, so a marker file living there would end up
    # as a stray ".staged" payload entry in the .msix.
    set(_staged_marker "${_msix_build_dir}/${edition}.staged")

    # Local shadows of the manifest's @-substitution variable names — scoped
    # to this function so the two edition calls don't clobber each other.
    set(TESSERACT_MSIX_IDENTITY_NAME "${identity_name}")
    set(TESSERACT_MSIX_PUBLISHER "${publisher}")
    configure_file(
        "${_msix_src_dir}/AppxManifest.xml.in"
        "${_stage}/AppxManifest.xml"
        @ONLY
    )

    set(_mo_dep "")
    if(DEFINED _mo_files AND _mo_files)
        set(_mo_dep ${_mo_files})
    endif()

    if(_mo_dep)
        add_custom_command(
            OUTPUT "${_staged_marker}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage}/Assets" "${_stage}/i18n"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:tesseract_win32>" "${_stage}/Tesseract.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/LICENSE" "${_stage}/LICENSE"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_assets_dir}" "${_stage}/Assets"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mo_dep} "${_stage}/i18n/"
            COMMAND ${CMAKE_COMMAND} -E touch "${_staged_marker}"
            DEPENDS tesseract_win32 msix-assets "${_stage}/AppxManifest.xml" ${_mo_dep}
            COMMENT "Staging MSIX payload (${edition})"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT "${_staged_marker}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage}/Assets" "${_stage}/i18n"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:tesseract_win32>" "${_stage}/Tesseract.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/LICENSE" "${_stage}/LICENSE"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_assets_dir}" "${_stage}/Assets"
            COMMAND ${CMAKE_COMMAND} -E touch "${_staged_marker}"
            DEPENDS tesseract_win32 msix-assets "${_stage}/AppxManifest.xml"
            COMMENT "Staging MSIX payload (${edition})"
            VERBATIM
        )
    endif()
    add_custom_target(msix-stage-${edition} DEPENDS "${_staged_marker}")

    set(_msix_out "${_msix_build_dir}/Tesseract-${edition}-x64.msix")
    add_custom_command(
        OUTPUT "${_msix_out}"
        COMMAND "${TESSERACT_MAKEAPPX}" pack /o /d "${_stage}" /p "${_msix_out}"
        DEPENDS msix-stage-${edition}
        COMMENT "Packing MSIX (${edition})"
        VERBATIM
    )
    add_custom_target(msix-pack-${edition} DEPENDS "${_msix_out}")

    add_custom_target(msix-validate-${edition}
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_msix_build_dir}/${edition}-validate"
        COMMAND "${TESSERACT_MAKEAPPX}" unpack /p "${_msix_out}" /d "${_msix_build_dir}/${edition}-validate"
        DEPENDS msix-pack-${edition}
        COMMENT "Validating MSIX (${edition}) via round-trip unpack"
        VERBATIM
    )
endfunction()

_tesseract_msix_edition(store  "${TESSERACT_MSIX_STORE_IDENTITY_NAME}"  "${TESSERACT_MSIX_STORE_PUBLISHER}")
_tesseract_msix_edition(direct "${TESSERACT_MSIX_DIRECT_IDENTITY_NAME}" "${TESSERACT_MSIX_PUBLISHER}")

add_custom_target(msix-stage    DEPENDS msix-stage-store    msix-stage-direct)
add_custom_target(msix-pack     DEPENDS msix-pack-store     msix-pack-direct)
add_custom_target(msix-validate DEPENDS msix-validate-store msix-validate-direct)

# Direct-edition signing reuses the NSIS installer's PFX/SignTool cache vars
# (see ui/windows/CMakeLists.txt) — no separate signing configuration for
# MSIX. The Store package is intentionally left unsigned; Partner Center
# signs it on ingestion.
if(TESSERACT_WIN_SIGN_CERT)
    add_custom_target(msix-sign
        COMMAND signtool sign
                /f "${TESSERACT_WIN_SIGN_CERT}"
                /p "${TESSERACT_WIN_SIGN_PASS}"
                /tr http://timestamp.digicert.com /td sha256 /fd sha256
                "${_msix_build_dir}/Tesseract-direct-x64.msix"
        DEPENDS msix-pack-direct
        COMMENT "Signing direct-distribution MSIX"
        VERBATIM
    )
endif()

# .appinstaller — direct edition only; Store builds are updated by the Store,
# not App Installer. Just a configure_file: no packing step needed.
configure_file(
    "${_msix_src_dir}/Tesseract.appinstaller.in"
    "${_msix_build_dir}/Tesseract-x64.appinstaller"
    @ONLY
)
