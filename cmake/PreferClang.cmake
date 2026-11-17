# PreferClang.cmake — default to clang/clang++ over GCC on Linux/BSD hosts when
# both are installed.
#
# Must be include()-d before project() in the root CMakeLists.txt: the compiler
# is chosen at project() time, and set() here is plain script code in the same
# CMake process.
#
# An explicit choice always wins, so this stays out of the way when:
#   - the host is Windows (MSVC) or macOS (Apple clang is already the default),
#   - a toolchain file is in use (xwin, Sailfish, ...),
#   - CMAKE_CXX_COMPILER / CMAKE_C_COMPILER is already set (-D, preset, cache),
#   - the CXX / CC environment variables are set,
#   - TESSERACT_PREFER_CLANG is OFF.
#
# An existing build directory keeps the compiler it cached: reconfigure with
# `cmake --preset <name> --fresh` to switch.

option(TESSERACT_PREFER_CLANG
    "Prefer clang/clang++ over the system default compiler when both are installed" ON)

if(NOT TESSERACT_PREFER_CLANG
        OR CMAKE_HOST_WIN32 OR CMAKE_HOST_APPLE
        OR CMAKE_TOOLCHAIN_FILE
        OR CMAKE_CXX_COMPILER OR CMAKE_C_COMPILER
        OR DEFINED ENV{CXX} OR DEFINED ENV{CC})
    return()
endif()

find_program(_tesseract_clangxx NAMES clang++)
find_program(_tesseract_clang NAMES clang)
# Both or neither: the root project is C++ only, but ui/linux-qt enables C too,
# and mixing a clang C++ compiler with gcc for C is asking for trouble.
if(_tesseract_clangxx AND _tesseract_clang)
    set(CMAKE_CXX_COMPILER "${_tesseract_clangxx}" CACHE FILEPATH "C++ compiler")
    set(CMAKE_C_COMPILER "${_tesseract_clang}" CACHE FILEPATH "C compiler")
    message(STATUS "Preferring clang: ${_tesseract_clangxx} (disable with -DTESSERACT_PREFER_CLANG=OFF)")
endif()
unset(_tesseract_clangxx CACHE)
unset(_tesseract_clang CACHE)
