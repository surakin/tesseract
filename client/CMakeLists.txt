cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# tesseract_client – common C++20 static library
# ---------------------------------------------------------------------------
add_library(tesseract_client STATIC
    src/client.cpp
    src/media_source.cpp
    src/markdown.cpp
    src/mentions.cpp
    src/event_handler_bridge.cpp
    src/session_store.cpp
    src/paths.cpp
    src/emoji.cpp
    src/prefs.cpp
    src/settings.cpp
    src/waveform_cache.cpp
    src/highlight.cpp
)

# ── SecretStore: platform credential backend ────────────────────────────────
if(WIN32)
    target_sources(tesseract_client PRIVATE src/secret_store_windows.cpp)
elseif(APPLE)
    target_sources(tesseract_client PRIVATE src/secret_store_macos.cpp)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET libsecret-1)
    endif()
    if(LIBSECRET_FOUND)
        target_sources(tesseract_client PRIVATE src/secret_store_linux.cpp)
        target_include_directories(tesseract_client
            PRIVATE ${LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(tesseract_client
            PRIVATE ${LIBSECRET_LIBRARIES})
    else()
        target_sources(tesseract_client PRIVATE src/secret_store_stub.cpp)
        message(STATUS "tesseract: libsecret not found — using plaintext token fallback")
    endif()
endif()

target_compile_features(tesseract_client PUBLIC cxx_std_20)

target_include_directories(tesseract_client
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against the Rust static library produced by Corrosion.
# The cxx bridge (tesseract-sdk-ffi-bridge) is compiled as a separate object
# archive by the Rust build script and made available via corrosion_add_cxxbridge.
target_link_libraries(tesseract_client
    PRIVATE
        tesseract_sdk_ffi          # Corrosion target (Rust)
        tesseract_sdk_bridge_cxx   # cxx generated C++ glue (from corrosion_add_cxxbridge)
        nlohmann_json::nlohmann_json
)

# Platform link requirements for the Rust static lib + the browser launcher.
if(WIN32)
    # Use lowercase names: MinGW on Linux stores import libs as libws2_32.a etc.
    # (case-sensitive filesystem), while MSVC on Windows is case-insensitive.
    target_link_libraries(tesseract_client PUBLIC
        ws2_32 bcrypt userenv ntdll
        shell32                # ShellExecuteA in open_in_browser()
        advapi32)              # CredWriteW / CredReadW for SecretStore
endif()

if(APPLE)
    target_link_libraries(tesseract_client PRIVATE "-framework Security")
endif()