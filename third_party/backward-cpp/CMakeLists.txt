cmake_minimum_required(VERSION 3.25)

project(backward_cpp LANGUAGES CXX)

# Vendored single-header stack-trace/signal-handling library (MIT license, see
# LICENSE.txt). Integration follows the same "vendor + our own CMakeLists.txt"
# pattern as third_party/bettertext, rather than relying on upstream's own
# build files, whose target names/API vary across releases.
#
# backward.hpp is used header-only: upstream's own backward.cpp is NOT
# compiled here. It instantiates a global `backward::SignalHandling sh;` at
# namespace scope, which would install the crash handler unconditionally at
# static-init time — before tesseract::install_crash_handler() ever runs and
# regardless of whether the user has opted in. client/src/crash_handler.cpp
# constructs backward::SignalHandling itself, only when appropriate.
add_library(backward INTERFACE)
target_include_directories(backward INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(backward INTERFACE cxx_std_20)

if(WIN32)
    # No BACKWARD_HAS_* define needed: backward.hpp auto-detects _WIN32 and
    # uses DbgHelp (StackWalk64 + SymFromAddr) for both stack capture and the
    # SEH-based unhandled-exception filter. Just link DbgHelp.
    target_link_libraries(backward INTERFACE dbghelp)
elseif(APPLE)
    # macOS: no libdw/libbfd/libdwarf available by default. Use the built-in
    # backtrace()/dladdr() backend (execinfo.h), which backward-cpp supports
    # out of the box on Darwin. Gives symbol names but not file:line without
    # a matching dSYM (see the dsymutil step wired up in ui/macos/CMakeLists.txt).
    target_compile_definitions(backward INTERFACE BACKWARD_HAS_BACKTRACE=1)
else() # Linux / *BSD
    # Prefer libdw (elfutils) for source-line-accurate DWARF resolution; fall
    # back to the libc backtrace() backend (address/symbol only, no line
    # numbers) if libdw-dev isn't present, mirroring the optional-dependency
    # pattern client/CMakeLists.txt already uses for libsecret.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDW QUIET libdw)
    endif()
    if(LIBDW_FOUND)
        target_compile_definitions(backward INTERFACE BACKWARD_HAS_DW=1)
        target_include_directories(backward INTERFACE ${LIBDW_INCLUDE_DIRS})
        target_link_libraries(backward INTERFACE ${LIBDW_LIBRARIES})
        message(STATUS "tesseract: libdw found — crash traces will include file:line")
    else()
        target_compile_definitions(backward INTERFACE BACKWARD_HAS_BACKTRACE=1)
        message(STATUS "tesseract: libdw not found — crash traces will be symbol-only (no file:line)")
    endif()
endif()
