# Patches Catch2's vendored test-discovery script (extras/CatchAddTests.cmake)
# to decode the test executable's --list-tests output as UTF-8 explicitly.
#
# catch_discover_tests() runs the compiled test binary and captures its test
# names via execute_process(... OUTPUT_VARIABLE) with no ENCODING option.
# CMake >= 3.15 defaults that to AUTO, which falls back to the ANSI code page
# whenever no console is attached — always true here, since discovery always
# runs as a piped subprocess of a POST_BUILD custom command. That silently
# mangles any non-ASCII test name (the arrows/dashes several regression
# tests use) into mojibake before it's ever written into the generated
# CTest file, so `ctest` reports "No test cases matched" even though the
# real UTF-8 test name Catch2 printed was entirely correct. This runs as
# this project's PATCH_COMMAND for the Catch2 FetchContent_Declare (see
# tests/CMakeLists.txt) so a fresh fetch always applies it, on every
# platform — the ENCODING option is simply ignored outside Windows.
#
# Invoked via `cmake -P` with the working directory set to the fetched
# Catch2 source root (ExternalProject/FetchContent's PATCH_COMMAND default).

set(_file "extras/CatchAddTests.cmake")
file(READ "${_file}" _contents)

string(REPLACE
    "OUTPUT_VARIABLE output\n    RESULT_VARIABLE result"
    "OUTPUT_VARIABLE output\n    RESULT_VARIABLE result\n    ENCODING UTF8"
    _contents "${_contents}")

string(REPLACE
    "OUTPUT_VARIABLE reporter_check_output\n      RESULT_VARIABLE reporter_check_result"
    "OUTPUT_VARIABLE reporter_check_output\n      RESULT_VARIABLE reporter_check_result\n      ENCODING UTF8"
    _contents "${_contents}")

file(WRITE "${_file}" "${_contents}")
