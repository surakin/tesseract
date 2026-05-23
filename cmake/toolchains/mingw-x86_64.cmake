set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Statically embed the MinGW C/C++ runtime so .exe files need no
# libgcc_s_seh-1.dll / libstdc++-6.dll at runtime.  This keeps cross-compiled
# binaries self-contained and makes Wine test runs work without extra DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Use Wine to run cross-compiled .exe files (e.g. catch_discover_tests,
# ctest) on the Linux build host.  Install with: pacman -S wine
find_program(WINE wine)
if(WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE}")
endif()
