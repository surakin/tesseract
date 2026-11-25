# Temporary, for the 3D call room experiment (docs/3D-CALL-ROOMS.md): builds the
# 3D engine from its own repository as a separate CMake project and exposes its
# C API to Tesseract. Nothing in the app uses it yet.
#
# The engine is cloned into the build tree, configured as a shared library
# with Tesseract's own build type (Release when none is set) and only its `engine` target is built (not its own tests). Its SDK
# (the three public C headers and the library) is installed into
# <build>/engine-sdk. Tesseract loads the library at run time with
# engine_loader.h (dlopen/LoadLibrary), so no engine compile flag or symbol
# reaches Tesseract's targets.
#
# Follows TESSERACT_ENGINE_GIT_TAG (default: main); every build fetches it.

include(ExternalProject)

set(TESSERACT_ENGINE_GIT_REPOSITORY "https://github.com/surakin/vse.git"
    CACHE STRING "Git repository of the 3D engine (TESSERACT_WITH_ENGINE)")
set(TESSERACT_ENGINE_GIT_TAG "main"
    CACHE STRING "Branch, tag or commit of the 3D engine to build (TESSERACT_WITH_ENGINE)")

# The engine's default branch was renamed from master to main: a build directory configured
# before that still has the old default cached.
if(TESSERACT_ENGINE_GIT_TAG STREQUAL "master")
    message(STATUS "TESSERACT_ENGINE_GIT_TAG: the engine's branch is now main, not master")
    set(TESSERACT_ENGINE_GIT_TAG "main" CACHE STRING
        "Branch, tag or commit of the 3D engine to build (TESSERACT_WITH_ENGINE)" FORCE)
endif()

# The engine follows Tesseract's build type (debug or release).
if(CMAKE_BUILD_TYPE)
    set(_tesseract_engine_build_type "${CMAKE_BUILD_TYPE}")
else()
    set(_tesseract_engine_build_type Release)
endif()

set(_tesseract_engine_sdk "${CMAKE_BINARY_DIR}/engine-sdk")
if(WIN32)
    set(_tesseract_engine_library "${_tesseract_engine_sdk}/bin/engine.dll")
elseif(APPLE)
    set(_tesseract_engine_library "${_tesseract_engine_sdk}/lib/libengine.1.dylib")
else()
    set(_tesseract_engine_library "${_tesseract_engine_sdk}/lib/libengine.so.1")
endif()

# On Windows the engine's toolchain file finds MSVC, the Windows SDK and the
# Vulkan SDK itself: the engine configures at build time, outside the MSVC
# environment MsvcAutoVcvars.cmake applied to Tesseract's own configure.
set(_tesseract_engine_cmake_args "")
if(WIN32)
    list(APPEND _tesseract_engine_cmake_args
        "-DCMAKE_TOOLCHAIN_FILE=<SOURCE_DIR>/cmake/toolchains/windows-msvc.cmake")
endif()

ExternalProject_Add(tesseract_engine_external
    GIT_REPOSITORY  "${TESSERACT_ENGINE_GIT_REPOSITORY}"
    GIT_TAG         "${TESSERACT_ENGINE_GIT_TAG}"
    GIT_SHALLOW     TRUE
    GIT_PROGRESS    TRUE
    PREFIX          "${CMAKE_BINARY_DIR}/engine"
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=${_tesseract_engine_build_type}"
        -DENGINE_BUILD_SHARED=ON
        "-DCMAKE_INSTALL_PREFIX=${_tesseract_engine_sdk}"
        ${_tesseract_engine_cmake_args}
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build <BINARY_DIR> --target engine --config ${_tesseract_engine_build_type}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --install <BINARY_DIR> --component engine_sdk --config ${_tesseract_engine_build_type}
    BUILD_BYPRODUCTS "${_tesseract_engine_library}"
    USES_TERMINAL_DOWNLOAD  TRUE
    USES_TERMINAL_UPDATE    TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD     TRUE
)

# Headers plus the library's path, for code that loads the engine.
add_library(tesseract_engine INTERFACE)
add_dependencies(tesseract_engine tesseract_engine_external)
target_include_directories(tesseract_engine INTERFACE "${_tesseract_engine_sdk}/include")
target_compile_definitions(tesseract_engine INTERFACE
    TESSERACT_ENGINE_LIBRARY_PATH="${_tesseract_engine_library}"
    TESSERACT_WORLD_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/worlds")
target_link_libraries(tesseract_engine INTERFACE ${CMAKE_DL_LIBS})

# Installed and packaged builds carry the engine and the world assets they load (macOS
# bundles them in ui/macos/CMakeLists.txt). EngineScene finds them relative to the
# executable: Linux <prefix>/lib/tesseract and <prefix>/share/tesseract/worlds, Windows
# next to the exe. A build tree keeps using the absolute paths compiled in above.
if(NOT APPLE)
    if(WIN32)
        set(_tesseract_engine_install_lib bin)
        set(_tesseract_engine_install_worlds bin/worlds)
    else()
        set(_tesseract_engine_install_lib lib/tesseract)
        set(_tesseract_engine_install_worlds share/tesseract/worlds)
    endif()
    if(WIN32)
        install(FILES "${_tesseract_engine_library}" DESTINATION "${_tesseract_engine_install_lib}")
    else()
        # libengine.so.1 is a symlink to the versioned file: install the directory's
        # versioned names, which keeps the link and its target together.
        install(DIRECTORY "${_tesseract_engine_sdk}/lib/"
                DESTINATION "${_tesseract_engine_install_lib}"
                FILES_MATCHING PATTERN "cmake" EXCLUDE
                REGEX "/libengine\\.so\\.[0-9.]+$")
    endif()
    # Only what EngineScene loads: not the generator scripts, previews or unused models.
    install(FILES
        "${CMAKE_SOURCE_DIR}/assets/worlds/world.json"
        "${CMAKE_SOURCE_DIR}/assets/worlds/robot.glb"
        "${CMAKE_SOURCE_DIR}/assets/worlds/green_field.glb"
        "${CMAKE_SOURCE_DIR}/assets/worlds/player.lua"
        "${CMAKE_SOURCE_DIR}/assets/worlds/daylight.lua"
        "${CMAKE_SOURCE_DIR}/assets/worlds/robot.lua"
        "${CMAKE_SOURCE_DIR}/assets/worlds/world.lua"
        DESTINATION "${_tesseract_engine_install_worlds}")
endif()
