cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Catch2 – fetched automatically, no global install required
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

# ---------------------------------------------------------------------------
# C++ test executable
# ---------------------------------------------------------------------------
add_executable(tesseract_tests
    cpp/test_session_store.cpp
    cpp/test_types.cpp
)

target_compile_features(tesseract_tests PRIVATE cxx_std_20)

# All three archives are mutually dependent (same WHOLE_ARCHIVE pattern used
# by the UI executables) so every symbol is present regardless of link order.
target_link_libraries(tesseract_tests PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client,tesseract_sdk_ffi-static>
    Catch2::Catch2WithMain
)

# Register each Catch2 TEST_CASE as an individual ctest test.
include(Catch)
catch_discover_tests(tesseract_tests)
