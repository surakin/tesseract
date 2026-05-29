# cmake/I18n.cmake
# Unified i18n build infrastructure for Tesseract.
#
# Targets:
#   i18n-extract  — run xgettext over all C++/ObjC++ sources → i18n/tesseract.pot
#   i18n-pseudo   — generate i18n/pseudo.po pseudolocale from the .pot
#   i18n-compile  — compile all i18n/*.po → <build>/i18n/tesseract.<lang>.mo  (ALL)
#
# Function:
#   tesseract_bundle_mo_files(<target>)  — macOS only: embed .mo into app bundle

# ---------------------------------------------------------------------------
# i18n-extract: xgettext over all C++/ObjC++ source trees
# ---------------------------------------------------------------------------
find_program(XGETTEXT_EXECUTABLE xgettext)
if(XGETTEXT_EXECUTABLE)
    file(GLOB_RECURSE _i18n_sources
        "${CMAKE_SOURCE_DIR}/ui/shared/*.cpp"
        "${CMAKE_SOURCE_DIR}/ui/shared/*.mm"
        "${CMAKE_SOURCE_DIR}/ui/windows/*.cpp"
        "${CMAKE_SOURCE_DIR}/ui/windows/*.mm"
        "${CMAKE_SOURCE_DIR}/ui/macos/*.cpp"
        "${CMAKE_SOURCE_DIR}/ui/macos/*.mm"
        "${CMAKE_SOURCE_DIR}/ui/linux-qt/*.cpp"
        "${CMAKE_SOURCE_DIR}/ui/linux-qt/*.mm"
        "${CMAKE_SOURCE_DIR}/ui/linux-gtk/*.cpp"
        "${CMAKE_SOURCE_DIR}/ui/linux-gtk/*.mm"
    )

    add_custom_target(i18n-extract
        COMMAND ${XGETTEXT_EXECUTABLE}
            --language=C++ --from-code=UTF-8
            --keyword=tr:1 --keyword=trn:1,2 --keyword=trf:1
            --add-comments=TRANSLATORS:
            --package-name=tesseract
            --output=${CMAKE_SOURCE_DIR}/i18n/tesseract.pot
            ${_i18n_sources}
        COMMENT "Extract translatable strings to i18n/tesseract.pot"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# i18n-pseudo: generate a pseudolocale .po for QA
# ---------------------------------------------------------------------------
find_program(PYTHON3_EXECUTABLE python3 python)
if(PYTHON3_EXECUTABLE AND XGETTEXT_EXECUTABLE)
    add_custom_target(i18n-pseudo
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/i18n/gen_pseudo.py
            ${CMAKE_SOURCE_DIR}/i18n/tesseract.pot
            ${CMAKE_SOURCE_DIR}/i18n/pseudo.po
        DEPENDS i18n-extract
        COMMENT "Generate pseudolocale i18n/pseudo.po"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# i18n-compile: msgfmt .po → .mo  (included in ALL so .mo files are always fresh)
# ---------------------------------------------------------------------------
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    file(GLOB _po_files "${CMAKE_SOURCE_DIR}/i18n/*.po")
    set(_mo_files "")
    foreach(_po ${_po_files})
        get_filename_component(_lang "${_po}" NAME_WE)   # e.g. "es", "pseudo"
        set(_mo "${CMAKE_BINARY_DIR}/i18n/tesseract.${_lang}.mo")
        add_custom_command(
            OUTPUT "${_mo}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/i18n"
            COMMAND ${MSGFMT_EXECUTABLE} -o "${_mo}" "${_po}"
            DEPENDS "${_po}"
            COMMENT "Compile i18n/${_lang}.po -> tesseract.${_lang}.mo"
            VERBATIM
        )
        list(APPEND _mo_files "${_mo}")
    endforeach()
    if(_mo_files)
        add_custom_target(i18n-compile ALL DEPENDS ${_mo_files})
    endif()
endif()

# ---------------------------------------------------------------------------
# Install rules (platform-specific)
# ---------------------------------------------------------------------------
if(MSGFMT_EXECUTABLE AND _mo_files)
    if(TESSERACT_UI STREQUAL "qt6" OR TESSERACT_UI STREQUAL "gtk")
        install(FILES ${_mo_files}
                DESTINATION share/tesseract/i18n)
    elseif(TESSERACT_UI STREQUAL "win32")
        install(FILES ${_mo_files}
                DESTINATION bin/i18n)
    elseif(TESSERACT_UI STREQUAL "macos")
        # macOS: .mo files are embedded into the app bundle via
        # tesseract_bundle_mo_files() called from ui/macos/CMakeLists.txt.
    endif()
endif()

# ---------------------------------------------------------------------------
# tesseract_bundle_mo_files(<target>)
# macOS helper: embed compiled .mo files into the app bundle under
# Resources/i18n so NSBundle can locate them at runtime.
# ---------------------------------------------------------------------------
function(tesseract_bundle_mo_files target)
    if(MSGFMT_EXECUTABLE AND _mo_files)
        foreach(_mo ${_mo_files})
            set_source_files_properties("${_mo}" PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources/i18n")
        endforeach()
        target_sources(${target} PRIVATE ${_mo_files})
        add_dependencies(${target} i18n-compile)
    endif()
endfunction()
