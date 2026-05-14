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
    cpp/test_paths.cpp
    cpp/test_emoji.cpp
    cpp/test_client_recent.cpp
    cpp/test_tk_canvas.cpp
    cpp/test_tk_widgets.cpp
    cpp/test_tk_lists.cpp
    cpp/test_tk_emoji_picker.cpp
    cpp/test_tk_recovery_banner.cpp
    cpp/test_tk_verification_banner.cpp
    cpp/test_tk_compose_bar.cpp
    cpp/test_tk_user_info.cpp
    cpp/test_tk_account_picker.cpp
    cpp/test_tk_login_view.cpp
    cpp/test_tk_sticker_picker.cpp
    cpp/test_tk_image_viewer.cpp
    cpp/test_tk_video_viewer.cpp
    cpp/test_tk_reply.cpp
    cpp/test_prefs.cpp
)

# tk_test_surface implementation is per-backend; only the one matching
# the active TESSERACT_UI is compiled in so the test binary has exactly
# one TestSurface::create symbol.
if(TESSERACT_UI STREQUAL "qt6")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_qt.cpp)
elseif(TESSERACT_UI STREQUAL "gtk")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_gtk.cpp)
elseif(TESSERACT_UI STREQUAL "win32")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_win32.cpp)
elseif(TESSERACT_UI STREQUAL "macos")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_cg.cpp)
    target_link_libraries(tesseract_tests PRIVATE "-framework CoreGraphics")
endif()

target_compile_features(tesseract_tests PRIVATE cxx_std_20)

target_include_directories(tesseract_tests PRIVATE cpp)

# All three archives are mutually dependent (same WHOLE_ARCHIVE pattern used
# by the UI executables) so every symbol is present regardless of link order.
target_link_libraries(tesseract_tests PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client>
    tesseract_sdk_ffi-static
    tesseract_tk
    Catch2::Catch2WithMain
)

# Register each Catch2 TEST_CASE as an individual ctest test.
include(Catch)
catch_discover_tests(tesseract_tests)
