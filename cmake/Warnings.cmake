# Warnings.cmake — warnings are errors for first-party C/C++/ObjC targets, in
# every configuration. Third-party code (third_party/, FetchContent deps) is
# left alone: tesseract_enable_warnings_as_errors() only touches targets whose
# sources live under client/, ui/ or tests/ of this repository.
#
#   GCC / Clang / AppleClang : -Wall -Wextra -Werror
#                              (minus the missing-field-initializers pair)
#   MSVC / clang-cl          : /W4 /WX
#
# Rust is covered separately by [workspace.lints.rust] in Cargo.toml.
#
# TESSERACT_WARNINGS_AS_ERRORS=OFF turns only the "-Werror" half off (warnings
# stay visible) for toolchains newer than the ones this tree is kept clean on.

option(TESSERACT_WARNINGS_AS_ERRORS
       "Treat warnings in first-party C/C++ targets as errors" ON)

# Recursively collects every target defined in <dir> and its subdirectories.
function(_tesseract_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _tesseract_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Call once, after every first-party add_subdirectory().
function(tesseract_enable_warnings_as_errors)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
        set(_flags /W4)
        # C4099: the cxx-generated bridge header mixes class/struct (see below).
        list(APPEND _flags /wd4099)
        if(TESSERACT_WARNINGS_AS_ERRORS)
            list(APPEND _flags /WX)
        endif()
    else()
        # Partial aggregate / designated initializers are idiomatic here (test
        # fixtures, option structs) and flagging them is far too noisy.
        set(_flags -Wall -Wextra -Wno-missing-field-initializers
                   -Wno-missing-designated-field-initializers)
        if(TESSERACT_WARNINGS_AS_ERRORS)
            list(APPEND _flags -Werror)
        endif()
    endif()

    _tesseract_collect_targets("${CMAKE_SOURCE_DIR}" _all)
    foreach(_t IN LISTS _all)
        get_target_property(_type ${_t} TYPE)
        if(NOT _type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(_src ${_t} SOURCE_DIR)
        # Only first-party trees; skips third_party/, FetchContent _deps and
        # the Corrosion-generated targets.
        if(NOT (_src MATCHES "^${CMAKE_SOURCE_DIR}/(client|ui|tests)(/|$)"))
            continue()
        endif()
        target_compile_options(${_t} PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${_flags}>)
        # getenv/sscanf/localtime are portable and used on purpose; the MSVC
        # "use the _s variant" deprecation (C4996) is not actionable here.
        if(MSVC)
            target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        # The cxx-generated bridge header declares opaque types as `class`
        # and defines them as `struct`; harmless outside the MS ABI and not
        # ours to edit.
        target_compile_options(${_t} PRIVATE
            $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wno-mismatched-tags>)
    endforeach()
endfunction()
