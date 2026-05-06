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

# Generate the cxx C++ glue from sdk/src/lib.rs and expose it as a CMake
# interface target named "tesseract_sdk_bridge_cxx".
corrosion_add_cxxbridge(tesseract_sdk_bridge_cxx
    CRATE  tesseract-sdk-ffi
    FILES  src/lib.rs
)

# ---------------------------------------------------------------------------
# Common client library
# ---------------------------------------------------------------------------
add_subdirectory(client)

# ---------------------------------------------------------------------------
# UI backend selection
# ---------------------------------------------------------------------------
if(TESSERACT_UI STREQUAL "auto")
    if(WIN32)
        set(TESSERACT_UI "win32")
    else()
        # Prefer Qt6 on Linux/macOS; fall back to gtk.
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

elseif(TESSERACT_UI STREQUAL "gtk")
    add_subdirectory(ui/linux-gtk)

elseif(TESSERACT_UI STREQUAL "qt6")
    add_subdirectory(ui/linux-qt)

else()
    message(FATAL_ERROR "Unknown TESSERACT_UI value: ${TESSERACT_UI}")
endif()
