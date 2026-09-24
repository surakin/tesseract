# MsvcAutoVcvars.cmake — locate and apply the MSVC developer environment on
# Windows so `cmake --preset windows-*` works from a plain terminal, without
# requiring a "Developer Command Prompt" / manually run vcvarsall.bat first.
#
# The Ninja generator (unlike the Visual Studio generators) does not activate
# MSVC itself: it needs cl.exe/link.exe/rc.exe/mt.exe reachable up front, or
# CMake's compiler probe silently falls back to whatever `cc`/`gcc` it can find
# (e.g. a bundled MinGW). Normally that environment comes from vcvarsall.bat.
#
# Must be include()-d before project()/enable_language() in the root
# CMakeLists.txt: this runs as plain script code in the same CMake process, so
# set(ENV{...}) here is visible to the compiler-detection step that follows.
# It only needs to run once per configure — the discovered compiler/linker
# paths are cached in CMakeCache.txt, and the rest of the project already
# bakes INCLUDE/LIB into explicit /I and /LIBPATH: flags (see CMakeLists.txt's
# MSVC block) and lets Cargo/cc-rs auto-detect MSVC on its own — so later
# `cmake --build` runs from a plain terminal keep working too.
#
# Skipped entirely off Windows, and a no-op if a VS dev environment (or a
# `cl.exe` already on PATH) is present, so this never fights an explicit
# Developer Command Prompt / CI's own vcvars setup (e.g. ilammy/msvc-dev-cmd).

if(NOT CMAKE_HOST_WIN32)
    return()
endif()

if(DEFINED ENV{VSCMD_VER})
    return()
endif()

find_program(_tesseract_cl_on_path cl.exe)
if(_tesseract_cl_on_path)
    unset(_tesseract_cl_on_path CACHE)
    return()
endif()
unset(_tesseract_cl_on_path CACHE)

# vswhere.exe ships with the Visual Studio Installer since VS2017 and lives at
# a fixed path regardless of which VS edition/version is installed. ARM64
# hosts install VS under "Program Files" rather than "Program Files (x86)".
set(_tesseract_vswhere "")
foreach(_pf "ProgramFiles(x86)" "ProgramFiles")
    if(DEFINED ENV{${_pf}})
        set(_candidate "$ENV{${_pf}}/Microsoft Visual Studio/Installer/vswhere.exe")
        if(EXISTS "${_candidate}")
            set(_tesseract_vswhere "${_candidate}")
            break()
        endif()
    endif()
endforeach()

if(NOT _tesseract_vswhere)
    message(WARNING
        "MsvcAutoVcvars: vswhere.exe not found — cannot auto-locate MSVC. "
        "Install Visual Studio / Build Tools, or run CMake from a Developer "
        "Command Prompt.")
    return()
endif()

execute_process(
    COMMAND "${_tesseract_vswhere}"
        -latest -products *
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
        -property installationPath
    OUTPUT_VARIABLE _tesseract_vs_install_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _tesseract_vswhere_result
)

if(NOT _tesseract_vswhere_result EQUAL 0 OR NOT _tesseract_vs_install_path)
    message(WARNING
        "MsvcAutoVcvars: no Visual Studio install with the C++ toolset found "
        "via vswhere — cannot auto-locate MSVC. Install the \"Desktop "
        "development with C++\" workload, or run CMake from a Developer "
        "Command Prompt.")
    return()
endif()

set(_tesseract_vcvarsall "${_tesseract_vs_install_path}/VC/Auxiliary/Build/vcvarsall.bat")
if(NOT EXISTS "${_tesseract_vcvarsall}")
    message(WARNING
        "MsvcAutoVcvars: vcvarsall.bat not found at '${_tesseract_vcvarsall}' "
        "— cannot auto-locate MSVC.")
    return()
endif()

# Run vcvarsall.bat and dump the resulting environment with `set`, so it can
# be parsed back out and applied to this CMake process. Always x64: this
# project only targets x86_64-pc-windows-msvc (see .cargo/config.toml
# generation below in CMakeLists.txt).
execute_process(
    COMMAND cmd.exe /c call "${_tesseract_vcvarsall}" x64 && set
    OUTPUT_VARIABLE _tesseract_vcvars_env
    RESULT_VARIABLE _tesseract_vcvars_result
)

if(NOT _tesseract_vcvars_result EQUAL 0)
    message(WARNING
        "MsvcAutoVcvars: vcvarsall.bat x64 failed — cannot auto-locate MSVC.")
    return()
endif()

# Only apply variables vcvarsall actually sets/changes for the toolchain;
# blindly re-exporting every inherited variable (USERPROFILE, TEMP, ...) is
# unnecessary and risks clobbering something CMake itself expects to control.
set(_tesseract_vcvars_keys PATH INCLUDE LIB LIBPATH
    VCINSTALLDIR VCTOOLSINSTALLDIR VCTOOLSVERSION
    WINDOWSSDKDIR WINDOWSSDKVERSION WINDOWSSDKLIBVERSION
    UNIVERSALCRTSDKDIR UCRTVERSION
    VSINSTALLDIR VISUALSTUDIOVERSION)

# Escape literal semicolons (e.g. every directory separator inside PATH)
# before turning newlines into the list separator below — otherwise PATH's
# own semicolons get parsed as element boundaries and PATH is silently
# truncated to just its first directory.
string(REPLACE ";" "\\;" _tesseract_vcvars_env "${_tesseract_vcvars_env}")
string(REPLACE "\r\n" "\n" _tesseract_vcvars_env "${_tesseract_vcvars_env}")
string(REPLACE "\n" ";" _tesseract_vcvars_lines "${_tesseract_vcvars_env}")

foreach(_line IN LISTS _tesseract_vcvars_lines)
    if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_()]*)=(.*)$")
        set(_name "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_2}")
        string(TOUPPER "${_name}" _name_upper)
        if(_name_upper IN_LIST _tesseract_vcvars_keys)
            if(_name_upper STREQUAL "PATH")
                set(ENV{PATH} "${_value}")
            else()
                set(ENV{${_name}} "${_value}")
            endif()
        endif()
    endif()
endforeach()

message(STATUS "MsvcAutoVcvars: applied MSVC x64 environment from ${_tesseract_vs_install_path}")

# CMake's MSVC support does not search for rc.exe the way it does for cl.exe:
# CMAKE_RC_COMPILER simply defaults to the bare literal "rc", trusting
# vcvarsall to have put it on PATH for the *build*, not just the configure,
# step. Ninja bakes that literal into every target's link rule (manifest
# embedding needs it even for targets with no .rc source), so it must be
# resolved to an absolute path now, while PATH still has the Windows SDK bin
# directory this script just added — otherwise linking only works if rc.exe
# also happens to be on PATH again whenever `cmake --build` is later run.
# FORCE so a stale "rc" cached by an earlier, pre-fix configure gets replaced.
if(NOT CMAKE_RC_COMPILER OR CMAKE_RC_COMPILER STREQUAL "rc")
    find_program(_tesseract_rc_compiler NAMES rc.exe)
    if(_tesseract_rc_compiler)
        set(CMAKE_RC_COMPILER "${_tesseract_rc_compiler}" CACHE FILEPATH "RC compiler" FORCE)
    else()
        message(WARNING "MsvcAutoVcvars: rc.exe not found on PATH after applying vcvars.")
    endif()
    unset(_tesseract_rc_compiler CACHE)
endif()

unset(_tesseract_vswhere)
unset(_tesseract_vs_install_path)
unset(_tesseract_vswhere_result)
unset(_tesseract_vcvarsall)
unset(_tesseract_vcvars_env)
unset(_tesseract_vcvars_result)
unset(_tesseract_vcvars_keys)
unset(_tesseract_vcvars_lines)
