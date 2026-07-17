cmake_minimum_required(VERSION 3.25)
project(Tesseract VERSION 0.8.16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate version.h from the project() version so C++ code can read it
# without any hardcoding.  The output lands in the build tree so it is
# never accidentally committed.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/client/include/tesseract/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/tesseract/version.h"
)

# Generate the Linux .desktop file with the project version substituted in,
# so X-AppImage-Version never drifts from project(... VERSION ...).  The Qt6
# and GTK shells install this generated copy (see their CMakeLists).
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/tesseract.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/tesseract.desktop"
    @ONLY
)

# RFC 2822 timestamp for the Debian changelog entry header.
string(TIMESTAMP PACKAGE_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)

# Debian changelog — generated to the build tree so the source tree stays
# clean; packaging/debian/rules copies it into debian/ during dpkg-buildpackage.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/changelog.in"
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/debian/changelog"
    @ONLY
)

# Arch PKGBUILD — generated in-source so `cp packaging/arch/PKGBUILD .` works
# without needing to reference the build tree.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/arch/PKGBUILD.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/arch/PKGBUILD"
    @ONLY
)

# PACKAGING.md — generated in-source so it stays readable in the repo without
# requiring a build.  Edit PACKAGING.md.in; this file is overwritten by CMake.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PACKAGING.md.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/PACKAGING.md"
    @ONLY
)

# OBJCXX must be enabled at root scope; enabling it from a subdirectory leaves
# CMAKE_OBJCXX_COMPILE_OBJECT unset in CMake 4.x.
if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(TESSERACT_UI "auto" CACHE STRING
    "UI backend to build: auto | win32 | gtk | qt6 | macos | linux (gtk+qt6 both)")

set(TESSERACT_GITHUB_REPO "surakin/tesseract" CACHE STRING
    "GitHub 'owner/repo' slug used for update checking (e.g. acme/tesseract). \
Leave empty to disable update checks.")

# Precompiled headers. A target's PCH is rebuilt (and every TU in it
# recompiled) whenever any header it contains changes, so the per-target PCH
# lists below hold ONLY stable, heavy system / third-party headers — never
# project or cxx-generated headers. Toggle off to debug PCH-related build
# issues or to profile minimal rebuilds.
option(TESSERACT_ENABLE_PCH "Use precompiled headers to speed up C++ builds" ON)

option(TESSERACT_ENABLE_LEGACY_LOGIN
    "Enable legacy username/password (m.login.password) login as a fallback for homeservers without OIDC/MAS"
    ON)

# Batches multiple .cpp files per translation unit (CMake's CMAKE_UNITY_BUILD)
# to cut redundant header parsing/instantiation across a full build. Must be
# set before the first add_subdirectory() below, since CMAKE_UNITY_BUILD is
# read at target-creation time. Toggle off to debug a build issue that might
# be caused by unity-batching (e.g. a newly introduced file-scope collision).
option(TESSERACT_UNITY_BUILD "Batch multiple .cpp files per translation unit to speed up full builds" ON)
if(TESSERACT_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
endif()

# Apply a precompiled-header set to `target`, honoring TESSERACT_ENABLE_PCH.
# Defined at root scope so every subdirectory's CMakeLists can call it. No
# language guards are needed: no target mixes C with C++/ObjC++ (the Wayland
# `.c` glue is its own target), and CMake emits a separate PCH per language for
# mixed C++/ObjC++ targets automatically.
function(tesseract_target_pch target)
    if(TESSERACT_ENABLE_PCH)
        target_precompile_headers(${target} PRIVATE ${ARGN})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# UI auto-detection. Runs before Corrosion so a preset's Rust_CARGO_TARGET
# is in place before the Cargo workspace is imported.
# ---------------------------------------------------------------------------
if(TESSERACT_UI STREQUAL "auto")
    if(WIN32)
        set(TESSERACT_UI "win32")
    elseif(APPLE)
        set(TESSERACT_UI "macos")
    else()
        find_package(Qt6 COMPONENTS Core Widgets QUIET)
        if(Qt6_FOUND)
            set(TESSERACT_UI "qt6")
        else()
            set(TESSERACT_UI "gtk")
        endif()
    endif()
    message(STATUS "TESSERACT_UI auto-selected: ${TESSERACT_UI}")
endif()

# ---------------------------------------------------------------------------
# macOS / AppKit deployment target. CMakePresets.json picks the architecture
# (arm64 or x86_64) and the matching Rust target triple via
# CMAKE_OSX_ARCHITECTURES + Rust_CARGO_TARGET. Both targets compile against
# the standard macOS SDK — no iOSSupport / Catalyst plumbing.
# ---------------------------------------------------------------------------
if(TESSERACT_UI STREQUAL "macos")
    if(NOT APPLE)
        message(FATAL_ERROR "macos UI only available on macOS")
    endif()
    # 11.0 = Big Sur. Earliest macOS that ships every Apple Silicon
    # frameworks we use (NSTrackingArea modes, NSTextField improvements,
    # CGImageSource APIs as we call them). Override at preset level.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    endif()
    # The CommandLineTools install ships a stale libc++ (version 8000, pre-C++20)
    # that lacks <ranges> and other C++20 standard library headers. The macOS SDK
    # carries a modern libc++ that does. Clang searches the CLT copy first, so we
    # strip the built-in C++ system include (-nostdinc++) and substitute the SDK's.
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE _MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
        $<$<COMPILE_LANGUAGE:CXX>:-isystem${_MACOS_SDK_PATH}/usr/include/c++/v1>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-nostdinc++>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-isystem${_MACOS_SDK_PATH}/usr/include/c++/v1>
    )
endif()

# ---------------------------------------------------------------------------
# Security hardening flags (GCC/Clang). Included before any add_subdirectory so
# the directory-scoped compile/link options propagate to every C++ target.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/Hardening.cmake)

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

# nlohmann/json – header-only JSON library used by tesseract_client
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Import the Cargo workspace.  Corrosion walks Cargo.toml and produces
# CMake targets mirroring each crate's [lib] / [[bin]] entries.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml)

# matrix-sdk 0.17 is rustls-only (native-tls feature removed), so no system
# OpenSSL is required. SQLite is bundled via the `bundled-sqlite` feature, so
# rusqlite compiles its own copy of SQLite internally.
#
# rustls pulls in aws-lc-sys (via aws-lc-rs) as its crypto provider. The
# cc-based builder in aws-lc-sys 0.40.0 has a bug: its Linux x86_64 source list
# omits `crypto/hrss/asm/poly_rq_mul.S`, while `hrss.c` still emits a call to
# `poly_Rq_mul` (POLY_RQ_MUL_ASM is auto-defined for x86_64 Linux). This leaves
# the symbol undefined at link time. Force the full CMake-based aws-lc build,
# which compiles every asm source aws-lc lists. Requires go, perl, and cmake
# (already in the prereqs).
corrosion_set_env_vars(tesseract_sdk_ffi
    AWS_LC_SYS_CMAKE_BUILDER=1
    TESSERACT_UI=${TESSERACT_UI})

# Apple system frameworks required by Rust crates' platform integrations
# (DNS resolver, proxy lookup, keychain). Attached to the Rust archive's
# interface so both the UI executable and the test executable pick them up.
if(APPLE)
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE
        "-framework Security"
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
    )
    # libwebrtc (bundled in livekit-rs) requires these frameworks:
    # CoreAudio    — audio device module (AudioDevice*/AudioObject* API)
    # CoreMedia    — H264 codec + CMSampleBuffer/CMTime paths
    # CoreVideo    — CVPixelBuffer handling
    # IOKit        — hardware device enumeration (IOServiceMatching etc.)
    # IOSurface    — desktop capture frame transport
    # VideoToolbox — hardware H264/H265 encode & decode (VTCompression*)
    # ScreenCaptureKit — screen sharing (SCStream/SCShareableContent, macOS 12.3+)
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE
        "-framework CoreAudio"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework IOKit"
        "-framework IOSurface"
        "-framework Metal"
        "-framework MetalKit"
        "-framework VideoToolbox"
        "-weak_framework ScreenCaptureKit"
    )
    # Pin Cargo's C/asm objects to the same macOS deployment target as CMake so
    # the linker doesn't warn about version mismatches in libtesseract_sdk_ffi.a.
    # Also mirror the -nostdinc++ / SDK-path fix from add_compile_options above so
    # Cargo-driven C++ builds (webrtc-sys via the cc crate) use the SDK's modern
    # libc++ instead of the stale CommandLineTools copy which lacks <ranges>.
    corrosion_set_env_vars(tesseract_sdk_ffi
        MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        "CXXFLAGS=-nostdinc++ -isystem${_MACOS_SDK_PATH}/usr/include/c++/v1")
endif()

# audiopus_sys emits cargo:rustc-link-lib=opus but Corrosion may place it
# before the Rust archive, causing GNU ld to discard symbols before seeing
# the undefined references.  Attach opus to the interface so it always
# follows the archive in the link command.  MSVC CI builds bundle opus into
# the Rust archive via OPUS_STATIC=1; MinGW and non-Windows need it explicitly.
if(NOT WIN32 OR MINGW)
    if(MINGW)
        # Prefer the static archive over the import library (.dll.a) so the
        # binary is self-contained and does not need libopus-0.dll at runtime.
        set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dll.a)
    endif()
    find_library(OPUS_LIB NAMES opus
        HINTS "$ENV{OPUS_LIB_DIR}" /opt/homebrew/lib /usr/local/lib
        REQUIRED)
    if(MINGW)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
    endif()
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE ${OPUS_LIB})
endif()

if(APPLE)
    # audiopus_sys on macOS always uses the "dynamic" Cargo feature, which calls
    # pkg-config to locate libopus. Set PKG_CONFIG_PATH to whichever directory
    # holds the opus.pc file (Homebrew lib/pkgconfig on arm64, or the pkgconfig/
    # subdirectory we create alongside a source-built dylib on x86_64).
    # PKG_CONFIG_ALLOW_CROSS lets pkg-config run during cross-compilation (arm64
    # runner building an x86_64 target).
    # NOTE: do NOT set OPUS_STATIC here. audiopus_sys build.rs checks
    # env::var("OPUS_STATIC").is_ok() — presence, not value — so even
    # OPUS_STATIC=0 would trigger the static branch, overriding the feature.
    get_filename_component(_opus_pkgconfig_dir "${OPUS_LIB}" DIRECTORY)
    corrosion_set_env_vars(tesseract_sdk_ffi
        PKG_CONFIG_ALLOW_CROSS=1
        "PKG_CONFIG_PATH=${_opus_pkgconfig_dir}/pkgconfig")
endif()

# MinGW cross-compilation on a case-sensitive host (Linux): Rust crates emit
# Windows-style mixed-case link library names (e.g. "Bcrypt", "Ws2_32") via
# `cargo:rustc-link-lib=`. MinGW's import stubs are lowercase on disk
# (libbcrypt.a, libws2_32.a), so the linker fails on Linux.
# Fix: lowercase every entry in the Rust target's INTERFACE_LINK_LIBRARIES
# that looks like a bare library name or a -l flag.
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    get_target_property(_rust_iface tesseract_sdk_ffi-static INTERFACE_LINK_LIBRARIES)
    if(_rust_iface)
        set(_fixed "")
        foreach(_entry IN LISTS _rust_iface)
            if(_entry MATCHES "^-l(.+)$")
                string(TOLOWER "${CMAKE_MATCH_1}" _lower)
                list(APPEND _fixed "-l${_lower}")
            elseif(_entry MATCHES "^[A-Za-z][A-Za-z0-9_]+$")
                string(TOLOWER "${_entry}" _lower)
                list(APPEND _fixed "${_lower}")
            else()
                list(APPEND _fixed "${_entry}")
            endif()
        endforeach()
        set_target_properties(tesseract_sdk_ffi-static PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_fixed}")
    endif()
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
    # The pre-built livekit/libwebrtc static library for Windows is compiled with
    # /MT (MT_StaticRelease).  Every C++ target and the Rust crate must also use
    # the static CRT so the linker does not raise LNK2038 ('RuntimeLibrary'
    # mismatch).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

    # Debug executables WHOLE_ARCHIVE-link tesseract_client + the Rust bridge
    # (see ui/windows/CMakeLists.txt), which merges enough debug type info to
    # blow past the linker's full-PDB size limit (LNK1140). FASTLINK keeps
    # each object's debug info in place and builds a small index PDB instead.
    add_link_options("$<$<CONFIG:Debug>:/DEBUG:FASTLINK>")

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
    # webrtc.lib (livekit/libwebrtc prebuilt) is compiled with /MT (static CRT).
    # crt-static makes rustc and cc-rs also use /MT so every object in the
    # final link agrees on RuntimeLibrary and the linker does not raise LNK2038.
    set(_crt_static_toml
"
# livekit/libwebrtc prebuilt is /MT; match it to avoid LNK2038.
[target.x86_64-pc-windows-msvc]
rustflags = [\"-C\", \"target-feature=+crt-static\"]
")
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
${_crt_static_toml}")
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
# MatrixRTC voice/video calls (livekit/libwebrtc)
# ---------------------------------------------------------------------------
# livekit-webrtc (bundled libwebrtc) on Windows:
if(WIN32)
    # webrtc-sys emits cargo:rustc-link-lib for these, but Corrosion does
    # not always propagate transitive build-script link-libs to every CMake
    # consumer (tests, shells).  Attach them to the Rust archive's own
    # interface (like the Apple frameworks / Opus library above) so they
    # always follow libtesseract_sdk_ffi.a in the link command — GNU ld
    # resolves undefined symbols in a single left-to-right pass, so a
    # library needed by symbols *inside* the archive must be positioned
    # at or after it, not merely attached to a sibling target.
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE
        msdmo wmcodecdspuuid dmoguids strmiids
        winmm iphlpapi shcore dwmapi
        crypt32 ole32 secur32 ws2_32 d3d11 gdi32 dxgi)
endif()

# livekit-webrtc (bundled libwebrtc) on Linux:
if(UNIX AND NOT APPLE)
    # (1) References XTest symbols for its screen-grab suppression path.
    #     Attach libXtst to the Rust archive's own interface (like the
    #     Apple frameworks / Opus library above) so it always follows
    #     libtesseract_sdk_ffi.a in the link command.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE PkgConfig::XTST)

    # (2) libwebrtc bundles protobuf codegen tool objects (main.o,
    #     cppgen_plugin.o, protozero_plugin.o) that each define `main`.
    #     Under --whole-archive they collide with the application main.
    #     --allow-multiple-definition tells ld to use the first definition;
    #     main.cpp.o always appears before libtesseract_sdk_ffi.a in the
    #     link order, so the application main wins and the tool mains are
    #     dead code that never executes.
    add_link_options(-Wl,--allow-multiple-definition)
endif()

# livekit-webrtc (bundled libwebrtc) on macOS: webrtc-sys's build script
# emits cargo:rustc-link-lib for these frameworks, but Corrosion does not
# propagate transitive build-script link-libs to every CMake consumer.
# Attach them to the Rust archive's own interface (like the other Apple
# frameworks / Opus library above) so they always follow
# libtesseract_sdk_ffi.a in the link command.
if(APPLE)
    target_link_libraries(tesseract_sdk_ffi-static INTERFACE
        "-framework Foundation"
        "-framework AppKit"
        "-framework AudioToolbox")
endif()

# ---------------------------------------------------------------------------
# TESSERACT_ENABLE_LEGACY_LOGIN — m.login.password fallback for non-OIDC servers
# ---------------------------------------------------------------------------
if(TESSERACT_ENABLE_LEGACY_LOGIN)
    message(STATUS "tesseract: legacy username/password login enabled")
    # Activate the `legacy_login` Cargo feature so the Rust SDK compiles its
    # password-login module. Unlike `calls`, this feature has no optional
    # native dependencies to pull in.
    corrosion_set_features(tesseract_sdk_ffi FEATURES legacy_login)
    # Propagate the flag to all C++ targets so #ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    # guards compile their password-login-related code.
    add_compile_definitions(TESSERACT_LEGACY_LOGIN_ENABLED)
endif()

# ---------------------------------------------------------------------------
# Common client library
# ---------------------------------------------------------------------------
add_subdirectory(client)

# Expose the generated version.h to every target that links tesseract_client.
target_include_directories(tesseract_client PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include")

# ---------------------------------------------------------------------------
# Shared UI toolkit (tesseract_tk) — widget tree, theme, abstract Canvas,
# and the per-platform Canvas/TextLayout backend implementations selected
# by TESSERACT_UI. Always compiled; each platform UI links against it.
# ---------------------------------------------------------------------------
add_subdirectory(ui/shared)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# i18n: unified catalog build (xgettext → .pot; msgfmt .po → .mo).
# Must precede add_subdirectory(ui/*) so tesseract_bundle_mo_files()
# is defined when the platform CMakeLists files call it.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/I18n.cmake)

# ---------------------------------------------------------------------------
# UI backend dispatch (TESSERACT_UI was already resolved up top — auto-
# detection and any preset overrides ran before Corrosion).
# ---------------------------------------------------------------------------
if(TESSERACT_UI STREQUAL "win32")
    if(NOT WIN32)
        message(FATAL_ERROR "win32 UI only available on Windows")
    endif()

    # BetterText — vendored Win32 DirectWrite/Direct2D text control backing
    # tk::NativeTextField / tk::NativeTextArea on Windows (see
    # ui/windows/tk/host_win32_bettertext.cpp). Windows-only; its own
    # CMakeLists.txt already guards non-WIN32 with a FATAL_ERROR.
    set(BETTERTEXT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(BETTERTEXT_BUILD_TESTS ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/bettertext)

    add_subdirectory(ui/windows)

elseif(TESSERACT_UI STREQUAL "macos")
    add_subdirectory(ui/macos)

elseif(TESSERACT_UI STREQUAL "gtk")
    add_subdirectory(ui/linux-gtk)

elseif(TESSERACT_UI STREQUAL "qt6")
    add_subdirectory(ui/linux-qt)

elseif(TESSERACT_UI STREQUAL "linux")
    add_subdirectory(ui/linux-gtk)
    add_subdirectory(ui/linux-qt)

else()
    message(FATAL_ERROR "Unknown TESSERACT_UI value: ${TESSERACT_UI}")
endif()

# ---------------------------------------------------------------------------
# Installer packaging (CPack). Windows → NSIS .exe, macOS → DragNDrop .dmg,
# Linux → DEB + RPM (Qt6 backend only; GTK4 uses the same rules).
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/Installers.cmake)
