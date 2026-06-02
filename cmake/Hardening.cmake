# Hardening.cmake — security hardening flags applied to every C / C++ / ObjC++
# target in the build. Mirrors the hardening set Arch Linux bakes into its
# default makepkg.conf flags, so a plain `cmake --build` produces a binary
# hardened the same way the packaged build is — without relying on the distro
# to inject CXXFLAGS.
#
# Every flag is probed (compiler or linker) before use, so flags that a given
# arch or platform does not support are silently skipped rather than breaking
# the build:
#   * -fcf-protection is x86-only; -mbranch-protection is AArch64-only.
#   * -Wl,-z,relro / -Wl,-z,now are GNU-ld (ELF) only; macOS ld64 rejects them.
#
# These apply only to CMake-compiled targets. The Rust crates build under Cargo
# and are unaffected. MSVC has its own hardening model (/GS on by default,
# /guard:cf) and is intentionally left untouched.

option(TESSERACT_ENABLE_HARDENING
       "Enable security hardening compile/link flags (GCC/Clang)" ON)

if(NOT TESSERACT_ENABLE_HARDENING OR MSVC)
    return()
endif()

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

# --- compile-time flags (probed individually) ---
set(_hardening_flags "")
foreach(_flag
        -fstack-protector-strong   # stack canaries on at-risk frames
        -fstack-clash-protection   # defeat stack-clash attacks
        -fcf-protection            # x86 CET (indirect-branch tracking)
        -mbranch-protection=standard)  # AArch64 BTI + PAC
    string(MAKE_C_IDENTIFIER "TESSERACT_HARDEN${_flag}" _have)
    check_cxx_compiler_flag("${_flag}" ${_have})
    if(${_have})
        list(APPEND _hardening_flags "${_flag}")
    endif()
endforeach()

# -Wformat is a prerequisite for -Werror=format-security and is universally
# supported on GCC/Clang, so it is added unconditionally.
list(APPEND _hardening_flags -Wformat -Werror=format-security)

add_compile_options(${_hardening_flags})

# libstdc++ bounds/precondition assertions (cheap; valid at any -O level).
add_compile_definitions(_GLIBCXX_ASSERTIONS)

# _FORTIFY_SOURCE requires an optimizing build (it emits a #warning under -O0),
# so restrict it to non-Debug configurations. -U precedes -D so our value wins
# over any -D_FORTIFY_SOURCE already on the command line (e.g. when makepkg also
# injects it via CXXFLAGS), avoiding a macro-redefinition warning.
add_compile_options(
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
)

# --- link-time flags (probed; ELF/GNU-ld only) ---
foreach(_lflag "-Wl,-z,relro" "-Wl,-z,now")
    string(MAKE_C_IDENTIFIER "TESSERACT_HARDEN_LINK${_lflag}" _have)
    check_linker_flag(CXX "${_lflag}" ${_have})
    if(${_have})
        add_link_options("${_lflag}")
    endif()
endforeach()

message(STATUS "Hardening: ${_hardening_flags}")
