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
    cpp/test_room_list_sections.cpp
    cpp/test_tk_emoji_picker.cpp
    cpp/test_tk_recovery_banner.cpp
    cpp/test_tk_verification_banner.cpp
    cpp/test_tk_compose_bar.cpp
    cpp/test_tk_user_info.cpp
    cpp/test_tk_account_picker.cpp
    cpp/test_tk_login_view.cpp
    cpp/test_tk_sticker_picker.cpp
    cpp/test_shortcode_engine.cpp
    cpp/test_tk_shortcode_popup.cpp
    cpp/test_tk_image_viewer.cpp
    cpp/test_tk_video_viewer.cpp
    cpp/test_tk_reply.cpp
    cpp/test_tk_side_tab_view.cpp
    cpp/test_html_spans.cpp
    cpp/test_prefs.cpp
    cpp/test_settings.cpp
    cpp/test_pending_send.cpp
    cpp/test_compose_bar_media.cpp
    cpp/test_map_tiles.cpp
    cpp/test_location_events.cpp
    cpp/test_markdown.cpp
    cpp/test_picker_image_cache.cpp
    cpp/test_session_store_migration.cpp
    cpp/test_server_info.cpp
    cpp/test_tk_account_section.cpp
    cpp/test_settings_controller.cpp
    cpp/test_tk_devices_section.cpp
    cpp/test_presence_tracker.cpp
    cpp/test_tray_unread.cpp
    cpp/test_find_existing_dm.cpp
    cpp/test_code_highlight.cpp
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
    target_link_libraries(tesseract_tests PRIVATE d3d11 dxgi)
elseif(TESSERACT_UI STREQUAL "macos")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_cg.cpp)
    target_link_libraries(tesseract_tests PRIVATE "-framework CoreGraphics")
endif()

target_compile_features(tesseract_tests PRIVATE cxx_std_20)

target_include_directories(tesseract_tests PRIVATE cpp)

# All three archives are mutually dependent (same WHOLE_ARCHIVE pattern used
# by the UI executables) so every symbol is present regardless of link order.
# Catch2::Catch2WithMain must come before tesseract_sdk_ffi-static on Windows:
# audiopus_sys embeds a prebuilt opus.lib (containing opus_compare's main())
# into the Rust staticlib.  MSVC resolves 'main' from the first lib that
# provides it, so Catch2 must win the race before opus_compare.obj is loaded.
target_link_libraries(tesseract_tests PRIVATE
    Catch2::Catch2WithMain
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client>
    tesseract_sdk_ffi-static
    tesseract_tk
)
# Register each Catch2 TEST_CASE as an individual ctest test.
include(Catch)
catch_discover_tests(tesseract_tests)
