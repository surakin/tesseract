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
    cpp/test_account_manager.cpp
    cpp/test_session_store.cpp
    cpp/test_types.cpp
    cpp/test_paths.cpp
    cpp/test_emoji.cpp
    cpp/test_client_recent.cpp
    cpp/test_url_preview.cpp
    cpp/test_tk_canvas.cpp
    cpp/test_tk_widgets.cpp
    cpp/test_tk_lists.cpp
    cpp/test_room_list_sections.cpp
    cpp/test_room_list_autoscroll.cpp
    cpp/test_unread_prefetch.cpp
    cpp/test_tk_emoji_picker.cpp
    cpp/test_tk_verification_banner.cpp
    cpp/test_tk_compose_bar.cpp
    cpp/test_tk_user_info.cpp
    cpp/test_tk_user_profile_panel.cpp
    cpp/test_tk_account_picker.cpp
    cpp/test_tk_login_view.cpp
    cpp/test_tk_sticker_picker.cpp
    cpp/test_image_pack_order.cpp
    cpp/test_image_pack_shortcode_suggest.cpp
    cpp/test_shortcode_engine.cpp
    cpp/test_tk_shortcode_popup.cpp
    cpp/test_mention_engine.cpp
    cpp/test_tk_image_viewer.cpp
    cpp/test_tk_video_viewer.cpp
    cpp/test_timeline_video_playlist.cpp
    cpp/test_tk_room_media_view.cpp
    cpp/test_tk_reply.cpp
    cpp/test_tk_side_tab_view.cpp
    cpp/test_tk_quick_switcher.cpp
    cpp/test_html_spans.cpp
    cpp/test_prefs.cpp
    cpp/test_settings.cpp
    cpp/test_pending_send.cpp
    cpp/test_compose_bar_media.cpp
    cpp/test_media_drop.cpp
    cpp/test_media_preview_policy.cpp
    cpp/test_tk_form_layout.cpp
    cpp/test_tk_media_section.cpp
    cpp/test_gif_engine.cpp
    cpp/test_tk_gif_popup.cpp
    cpp/test_map_tiles.cpp
    cpp/test_location_events.cpp
    cpp/test_markdown.cpp
    cpp/test_mentions.cpp
    cpp/test_picker_image_cache.cpp
    cpp/test_anim_image_cache.cpp
    cpp/test_pixmap_cache.cpp
    cpp/test_session_store_migration.cpp
    cpp/test_session_persist_queue.cpp
    cpp/test_server_info.cpp
    cpp/test_tk_account_section.cpp
    cpp/test_room_settings_diff.cpp
    cpp/test_tk_room_settings_view.cpp
    cpp/test_tk_image_pack_editor_view.cpp
    cpp/test_tk_room_media_section.cpp
    cpp/test_tk_room_security_section.cpp
    cpp/test_tk_room_permissions_section.cpp
    cpp/test_settings_controller.cpp
    cpp/test_tk_devices_section.cpp
    cpp/test_tk_settings_page.cpp
    cpp/test_presence_tracker.cpp
    cpp/test_tray_unread.cpp
    cpp/test_find_existing_dm.cpp
    cpp/test_shell_thread_transition.cpp
    cpp/test_shell_thread_panel.cpp
    cpp/test_shell_nav_history.cpp
    cpp/test_shell_relayout_coalesce.cpp
    cpp/test_shell_alive_guard.cpp
    cpp/test_shell_message_inserted.cpp
    cpp/test_shell_dispatch_room_send.cpp
    cpp/test_shell_media_priority.cpp
    cpp/test_shell_media_view_pagination.cpp
    cpp/test_shell_roster_invalidation.cpp
    cpp/test_shell_forward_async.cpp
    cpp/test_roomlist_unread.cpp
    cpp/test_message_list_pinning.cpp
    cpp/test_message_list_membership.cpp
    cpp/test_tk_message_list_search_highlight.cpp
    cpp/test_tk_pinned_banner.cpp
    cpp/test_tk_message_list_threads.cpp
    cpp/test_tk_message_list_layout_cache.cpp
    cpp/test_room_switch_gate.cpp
    cpp/test_shell_warm_subscriptions.cpp
    cpp/test_tk_thread_list_view.cpp
    cpp/test_tk_thread_view.cpp
    cpp/test_event_handler_bridge.cpp
    cpp/test_tk_room_header.cpp
    cpp/test_tk_room_search_bar.cpp
    cpp/test_code_highlight.cpp
    cpp/test_tk_room_view.cpp
    cpp/test_tk_room_view_thread_callbacks.cpp
    cpp/test_slash_commands.cpp
    cpp/test_slash_command_engine.cpp
    cpp/test_i18n.cpp
    cpp/test_media_disk_cache.cpp
    cpp/test_tk_about_section.cpp
    cpp/test_tk_settings_view.cpp
    cpp/test_encryption_setup_overlay.cpp
    cpp/test_shell_encryption_setup.cpp
    cpp/test_tk_host_pointer.cpp
    cpp/test_tk_host_file_drop.cpp
    cpp/test_tk_tooltip.cpp
    cpp/test_timeline_media_controller.cpp
    cpp/test_status_links.cpp
    cpp/test_tk_room_preview.cpp
)

# tk_test_surface implementation is per-backend; only the one matching
# the active TESSERACT_UI is compiled in so the test binary has exactly
# one TestSurface::create symbol. TESSERACT_UI=linux (dual gtk+qt6 dev
# build) exercises the qt6 test backend, matching CI's qt6-only testing.
set(_TESSERACT_TEST_UI "${TESSERACT_UI}")
if(_TESSERACT_TEST_UI STREQUAL "linux")
    set(_TESSERACT_TEST_UI "qt6")
endif()

if(_TESSERACT_TEST_UI STREQUAL "qt6")
    # The Qt6 backend (canvas_qpainter.cpp) now lives in ui/linux-qt rather
    # than tesseract_tk, so the test surface must compile and link it itself.
    # It #includes "canvas.h" from ui/shared/tk, uses QtWidgets, and defines a
    # Q_OBJECT (BlankTextObjectInterface) that needs AUTOMOC — mirror the
    # tesseract_qt executable's Qt setup here.
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
    set_target_properties(tesseract_tests PROPERTIES AUTOMOC ON)
    target_sources(tesseract_tests PRIVATE
        cpp/tk_test_surface_qt.cpp
        "${CMAKE_SOURCE_DIR}/ui/linux-qt/tk/canvas_qpainter.cpp")
    # Self-includes its own AUTOMOC .moc file; excluded from unity batching
    # defensively here too (see the same exclusion in ui/linux-qt/CMakeLists.txt).
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/ui/linux-qt/tk/canvas_qpainter.cpp"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_include_directories(tesseract_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ui/linux-qt"
        "${CMAKE_SOURCE_DIR}/ui/shared/tk")
    target_link_libraries(tesseract_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
elseif(_TESSERACT_TEST_UI STREQUAL "gtk")
    target_sources(tesseract_tests PRIVATE cpp/tk_test_surface_gtk.cpp)
    # canvas_cairo.h lives in ui/linux-gtk/tk and bare-#includes "canvas.h"
    # from ui/shared/tk; add shared/tk so it resolves (mirrors tesseract_gtk).
    target_include_directories(tesseract_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ui/linux-gtk"
        "${CMAKE_SOURCE_DIR}/ui/shared/tk")
elseif(_TESSERACT_TEST_UI STREQUAL "win32")
    if(MSVC)
        target_compile_options(tesseract_tests PRIVATE /utf-8)
    endif()
    # video_capture_win32.cpp supplies tk::make_video_capture_win32(), which
    # tk::VideoCapture::create() (still in tesseract_tk) calls unconditionally
    # on _WIN32 — it moved out of tesseract_tk like canvas_d2d.cpp, so linking
    # tests pulls in libtesseract_tk's reference with no definition unless
    # compiled here (mirrors the macOS branch below).
    target_sources(tesseract_tests PRIVATE
        cpp/tk_test_surface_win32.cpp
        "${CMAKE_SOURCE_DIR}/ui/windows/tk/canvas_d2d.cpp"
        "${CMAKE_SOURCE_DIR}/ui/windows/tk/video_capture_win32.cpp")
    target_include_directories(tesseract_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ui/windows"
        "${CMAKE_SOURCE_DIR}/ui/shared/tk")
    target_compile_definitions(tesseract_tests PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(tesseract_tests PRIVATE
        d2d1
        d3d11
        dxgi
        dwrite
        mf
        mfplat
        mfreadwrite
        mfuuid)
    # tk::ScreenCapture::create()'s _WIN32 branch (and screen_capture.cpp as a
    # whole) only exists behind TESSERACT_ENABLE_CALLS; same move-out gap as
    # video capture above, so mirror it only when calls are enabled (mirrors
    # the macOS branch below).
    if(TESSERACT_ENABLE_CALLS)
        target_sources(tesseract_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/ui/windows/tk/screen_capture_win32.cpp")
    endif()
elseif(_TESSERACT_TEST_UI STREQUAL "macos")
    # canvas_cg.cpp (tk::cg::make_factory/make_canvas) now lives in ui/macos
    # rather than tesseract_tk, so the test surface must compile it itself.
    # canvas_cg.h bare-#includes "canvas.h" from ui/shared/tk; add that so it
    # resolves (mirrors tesseract_macos's own include wiring).
    #
    # video_capture_macos.mm supplies tk::make_video_capture_macos(), which
    # tk::VideoCapture::create() (still in tesseract_tk) calls unconditionally
    # on __APPLE__ — it also moved out of tesseract_tk, so linking tests pulls
    # in libtesseract_tk.a's reference with no definition unless compiled here.
    target_sources(tesseract_tests PRIVATE
        cpp/tk_test_surface_cg.cpp
        "${CMAKE_SOURCE_DIR}/ui/macos/tk/canvas_cg.cpp"
        "${CMAKE_SOURCE_DIR}/ui/macos/tk/video_capture_macos.mm")
    # SKIP_PRECOMPILE_HEADERS: the target's ObjC++ PCH is built without ARC
    # (tesseract_tests has no target-wide -fobjc-arc), which conflicts with
    # the ARC this one file needs.
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/ui/macos/tk/video_capture_macos.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc" SKIP_PRECOMPILE_HEADERS ON)
    # tk_test_surface_cg.cpp and canvas_cg.cpp bare-#include CoreGraphics.h,
    # whose transitive MacTypes.h declares global-namespace `Point`/`Rect`
    # (legacy QuickDraw types). Many test_tk_*.cpp files have a file-scope
    # `using namespace tk;`, which under unity batching stays in effect for
    # the rest of that translation unit — if the batch places one of those
    # files before either CG file, `Point`/`Rect` become ambiguous between
    # the global and tk:: versions. Exclude both defensively so they always
    # compile standalone (mirrors the same-shaped exclusion in the qt6
    # branch above for canvas_qpainter.cpp).
    set_source_files_properties(
        cpp/tk_test_surface_cg.cpp
        "${CMAKE_SOURCE_DIR}/ui/macos/tk/canvas_cg.cpp"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(tesseract_tests PRIVATE
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
        "-framework AVFoundation"
        "-framework CoreVideo"
        "-framework CoreMedia"    # video_capture_macos.mm: CMSampleBufferGetImageBuffer
        "-framework Foundation")  # video_capture_macos.mm: NSString/NSNumber
    target_include_directories(tesseract_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ui/macos"
        "${CMAKE_SOURCE_DIR}/ui/shared/tk")

    # tk::ScreenCapture::create()'s __APPLE__ branch (and screen_capture.cpp
    # as a whole) only exists behind TESSERACT_CALLS_ENABLED; same move-out
    # gap as video capture above, so mirror it only when calls are enabled.
    if(TESSERACT_ENABLE_CALLS)
        target_sources(tesseract_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/ui/macos/tk/screen_capture_macos.mm")
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/ui/macos/tk/screen_capture_macos.mm"
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc" SKIP_PRECOMPILE_HEADERS ON)
        target_link_libraries(tesseract_tests PRIVATE
            "-weak_framework ScreenCaptureKit")
    endif()
endif()

target_compile_features(tesseract_tests PRIVATE cxx_std_20)

# Catch2's test macros are the dominant per-file cost across ~63 of the test
# TUs; bundle them with the common std headers used by the test harness.
tesseract_target_pch(tesseract_tests
    <catch2/catch_test_macros.hpp>
    <catch2/catch_approx.hpp>
    <string>
    <memory>
    <vector>
    <cstdint>
    <filesystem>
    <optional>
    <functional>
)

target_include_directories(tesseract_tests
    PRIVATE
        cpp
        # Allow tests to exercise the FFI conversion helpers that are
        # otherwise PRIVATE to tesseract_client. The functions are header-only
        # (inline) so no extra link is needed; just the include path.
        ${CMAKE_CURRENT_SOURCE_DIR}/../client/src)

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
# Keychain-touching tests share the OS credential store (libsecret /
# Keychain / CredManager) and must not run concurrently; give them a
# shared RESOURCE_LOCK so ctest serialises them even under -jN.
catch_discover_tests(tesseract_tests
    TEST_SPEC "[keychain]"
    PROPERTIES RESOURCE_LOCK keychain
)
# All other tests have no shared global state and can run in parallel.
catch_discover_tests(tesseract_tests
    TEST_SPEC "~[keychain]"
)
