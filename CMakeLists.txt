cmake_minimum_required(VERSION 3.25)
project(Tesseract VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(TESSERACT_UI "auto" CACHE STRING
    "UI backend to build: auto | win32 | gtk | qt6")

# ---------------------------------------------------------------------------
# Corrosion – bridges CMake and Cargo
# ---------------------------------------------------------------------------
# Use FetchContent so the project is self-contained (no global install needed).
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5
)
FetchContent_MakeAvailable(Corrosion)

# Import the Cargo workspace.  Corrosion walks Cargo.toml and produces
# CMake targets mirroring each crate's [lib] / [[bin]] entries.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml)

# macOS: Homebrew ships OpenSSL as a keg-only formula (not on the default
# search path). Auto-detect the prefix for Apple Silicon and Intel.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_ossl_dir
            "/opt/homebrew/opt/openssl@3"
            "/usr/local/opt/openssl@3")
        if(IS_DIRECTORY "${_ossl_dir}")
            set(OPENSSL_ROOT_DIR "${_ossl_dir}" CACHE PATH "Homebrew OpenSSL root")
            break()
        endif()
    endforeach()
endif()

# Native C deps pulled in by the Rust crate (openssl-sys).
# SQLite is no longer needed here: matrix-sdk is built with the bundled-sqlite
# feature, so rusqlite compiles its own copy of SQLite internally.
find_package(OpenSSL REQUIRED)
target_link_libraries(tesseract_sdk_ffi-static INTERFACE
    OpenSSL::SSL OpenSSL::Crypto
)

# Apple system frameworks required by Rust crates (security-framework,
# core-foundation, native-tls). Attached to the Rust archive's interface so
# both the UI executable and the test executable pick them up.
if(APPLE)
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE
        "-framework Security"
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
    )
endif()

# ---------------------------------------------------------------------------
# MSVC environment forwarding via .cargo/config.toml
# ---------------------------------------------------------------------------
# Ninja spawns Cargo via `cmake -E env KEY=VAL cargo rustc ...`, forwarding
# only the explicitly listed variables.  Crates that compile C++ (cxx,
# rusqlite/bundled, openssl-sys) call cc-rs → cl.exe; without INCLUDE/LIB
# cl.exe aborts with "no include path set".
#
# corrosion_set_env_vars cannot carry semicolons in values because CMake
# stores properties as semicolon-delimited lists and Corrosion later expands
# them unquoted, turning "INCLUDE=a;b" into three separate arguments.
#
# Solution: write a .cargo/config.toml [env] table from CMake.  TOML strings
# are plain strings; semicolons are just characters.  Cargo reads the file
# before invoking any build script, so INCLUDE/LIB are set for cc-rs.
if(MSVC)
    # Rust's MSVC target always links against the release CRT (/MD, MSVCRT.dll),
    # even in debug Rust builds.  Force every C++ target in this project to use
    # the same runtime so the linker does not raise LNK2038 (mismatch detected
    # for 'RuntimeLibrary' / '_ITERATOR_DEBUG_LEVEL').
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

    # --- derive MSVC tools dir from cl.exe path ---
    # cl.exe lives at <VCToolsDir>/bin/Host<arch>/<arch>/cl.exe
    get_filename_component(_cc "${CMAKE_CXX_COMPILER}" DIRECTORY)   # …/bin/Hostx64/x64
    get_filename_component(_cc "${_cc}"                DIRECTORY)   # …/bin/Hostx64
    get_filename_component(_cc "${_cc}"                DIRECTORY)   # …/bin
    get_filename_component(_msvc_tools "${_cc}"        DIRECTORY)   # …/VC/Tools/MSVC/14.x.y

    set(_inc_list "${_msvc_tools}/include")
    set(_lib_list "${_msvc_tools}/lib/x64")

    # --- Windows SDK via registry ---
    cmake_host_system_information(RESULT _winsdk_root
        QUERY WINDOWS_REGISTRY
            "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10" VIEW BOTH)

    if(_winsdk_root)
        file(GLOB _sdk_ver_dirs LIST_DIRECTORIES true
             "${_winsdk_root}include/10.*")
        list(SORT _sdk_ver_dirs ORDER DESCENDING)
        list(GET  _sdk_ver_dirs 0 _sdk_ver_dir)
        get_filename_component(_sdk_ver "${_sdk_ver_dir}" NAME)

        list(APPEND _inc_list
            "${_winsdk_root}include/${_sdk_ver}/ucrt"
            "${_winsdk_root}include/${_sdk_ver}/um"
            "${_winsdk_root}include/${_sdk_ver}/shared"
            "${_winsdk_root}include/${_sdk_ver}/winrt"
        )
        list(APPEND _lib_list
            "${_winsdk_root}lib/${_sdk_ver}/ucrt/x64"
            "${_winsdk_root}lib/${_sdk_ver}/um/x64"
        )
    else()
        message(WARNING
            "Windows SDK not found in registry – Cargo C++ crates may fail. "
            "Install the Windows SDK or run CMake from a Developer Command Prompt.")
    endif()

    # Build semicolon-separated strings via string(APPEND) – NOT list(JOIN) –
    # so CMake never re-parses the semicolons as list separators.
    # Also normalise to forward slashes: backslashes are escape chars in TOML.
    set(_inc_str "")
    foreach(_d IN LISTS _inc_list)
        file(TO_CMAKE_PATH "${_d}" _d)   # backslash → forward slash
        if(_inc_str)
            string(APPEND _inc_str ";")
        endif()
        string(APPEND _inc_str "${_d}")
    endforeach()

    set(_lib_str "")
    foreach(_d IN LISTS _lib_list)
        file(TO_CMAKE_PATH "${_d}" _d)
        if(_lib_str)
            string(APPEND _lib_str ";")
        endif()
        string(APPEND _lib_str "${_d}")
    endforeach()

    # Write .cargo/config.toml into the workspace root.
    # This file is machine-specific; add it to .gitignore.
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/.cargo")
    file(WRITE "${CMAKE_SOURCE_DIR}/.cargo/config.toml"
"# Generated by CMake – do not edit, do not commit (see .gitignore).
# Sets INCLUDE and LIB for cc-rs so Cargo-invoked C++ compilers (cxx,
# rusqlite/bundled, openssl-sys) find the MSVC and Windows SDK headers.
[env]
INCLUDE = \"${_inc_str}\"
LIB     = \"${_lib_str}\"

# Build with a single codegen unit so the staticlib archive contains only
# one object file.  Multiple CGUs each embed their own Windows import-library
# thunks (__IMPORT_DESCRIPTOR_kernel32, etc.); with WHOLE_ARCHIVE linking
# those duplicates trigger LNK2005/LNK1319.  One CGU = no duplicates.
# Only applied to the dev profile (debug builds); release is unchanged.
[profile.dev]
codegen-units = 1
")
    message(STATUS "Wrote .cargo/config.toml with MSVC include/lib paths")

    # Also tell CMake's own compiler and linker where to search.
    # Ninja+MSVC doesn't source vcvarsall.bat, so INCLUDE and LIB are not set
    # in the build environment.  include_directories(SYSTEM) emits /I flags for
    # every target; link_directories() emits /LIBPATH: flags for the linker.
    include_directories(SYSTEM ${_inc_list})
    link_directories(${_lib_list})
endif()

# Generate the cxx C++ glue from sdk/src/lib.rs and expose it as a CMake
# interface target named "tesseract_sdk_bridge_cxx".
corrosion_add_cxxbridge(tesseract_sdk_bridge_cxx
    CRATE  tesseract_sdk_ffi
    FILES  bridge.rs
)

# The generated cxx bridge code includes tesseract/event_handler_bridge.hpp.
target_include_directories(tesseract_sdk_bridge_cxx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/client/include
)

# ---------------------------------------------------------------------------
# Common client library
# ---------------------------------------------------------------------------
add_subdirectory(client)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# UI backend selection
# ---------------------------------------------------------------------------
if(TESSERACT_UI STREQUAL "auto")
    if(WIN32)
        set(TESSERACT_UI "win32")
    elseif(APPLE)
        set(TESSERACT_UI "macos")
    else()
        # Prefer Qt6 on Linux; fall back to gtk.
        find_package(Qt6 COMPONENTS Core Widgets QUIET)
        if(Qt6_FOUND)
            set(TESSERACT_UI "qt6")
        else()
            set(TESSERACT_UI "gtk")
        endif()
    endif()
    message(STATUS "TESSERACT_UI auto-selected: ${TESSERACT_UI}")
endif()

if(TESSERACT_UI STREQUAL "win32")
    if(NOT WIN32)
        message(FATAL_ERROR "win32 UI only available on Windows")
    endif()
    add_subdirectory(ui/windows)

elseif(TESSERACT_UI STREQUAL "macos")
    if(NOT APPLE)
        message(FATAL_ERROR "macos UI only available on macOS")
    endif()
    add_subdirectory(ui/macos)

elseif(TESSERACT_UI STREQUAL "gtk")
    add_subdirectory(ui/linux-gtk)

elseif(TESSERACT_UI STREQUAL "qt6")
    add_subdirectory(ui/linux-qt)

else()
    message(FATAL_ERROR "Unknown TESSERACT_UI value: ${TESSERACT_UI}")
endif()
