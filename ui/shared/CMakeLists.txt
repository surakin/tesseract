cmake_minimum_required(VERSION 3.25)

# tesseract_tk — the shared UI toolkit core. Cross-platform widget classes,
# layout, theme, and the abstract Canvas interface (see tk/canvas.h).
#
# Per-platform backend translation units (Direct2D on Win32, CoreGraphics on
# macOS, QPainter on Qt6, Cairo+Pango on GTK4) live in each ui/<platform>/tk/
# instead, compiled directly into that platform's own executable target —
# see the TESSERACT_UI conditional below for the one exception (GStreamer,
# shared between the Qt6 and GTK4 backends).

set(_tk_sources
    app/ShellBase.h
    app/ShellBase.cpp
    app/AccountManager.h
    app/AccountManager.cpp
    app/EventHandlerBase.h
    app/EventHandlerBase.cpp
    app/RoomWindowBase.h
    app/RoomWindowBase.cpp
    app/SettingsController.h
    app/SettingsController.cpp
    app/PresenceTracker.h
    app/PresenceTracker.cpp
    app/ThreadPanelController.h
    app/ThreadPanelController.cpp
    app/SlashCommands.h
    app/SlashCommands.cpp
    app/status_links.h
    app/status_links.cpp
    app/UpdateChecker.h
    app/GithubUpdateChecker.h
    app/GithubUpdateChecker.cpp
    tk/audio.h
    tk/audio_capture.h
    tk/video.h
    tk/canvas.h
    tk/canvas_common.cpp
    tk/host.h
    tk/host.cpp
    tk/theme.h
    tk/theme.cpp
    tk/i18n.h
    tk/i18n.cpp
    tk/anim_image_cache.h
    tk/anim_image_cache.cpp
    tk/pixmap_cache.h
    tk/pixmap_cache.cpp
    tk/widget.h
    tk/widget.cpp
    tk/layout.h
    tk/layout.cpp
    tk/form_layout.h
    tk/form_layout.cpp
    tk/combobox.h
    tk/combobox.cpp
    tk/controls.h
    tk/controls.cpp
    tk/text_field.h
    tk/text_field.cpp
    tk/text_area.h
    tk/text_area.cpp
    tk/image_view.h
    tk/image_view.cpp
    tk/scrollable_base.h
    tk/scrollable_base.cpp
    tk/list_view.h
    tk/list_view.cpp
    tk/tab_bar.h
    tk/tab_bar.cpp
    tk/side_tab_view.h
    tk/side_tab_view.cpp
    tk/svg.cpp
    tk/blurhash.h
    tk/blurhash.cpp
    tk/media_disk_cache.h
    tk/media_disk_cache.cpp
    tk/tooltip.h
    tk/tooltip.cpp
    tk/toast.h
    tk/toast.cpp
    views/BrandView.h
    views/BrandView.cpp
    views/LoginView.h
    views/LoginView.cpp
    views/QRGrantView.h
    views/QRGrantView.cpp
    views/RoomListView.h
    views/RoomListView.cpp
    views/MessageListView.h
    views/MessageListView.cpp
    views/TimelineMediaController.h
    views/TimelineMediaController.cpp
    views/UrlPreviewCardDisplay.h
    views/UrlPreviewCardDisplay.cpp
    views/TimelineVideoPlaylist.h
    views/TimelineVideoPlaylist.cpp
    views/LocationMapPanner.h
    views/LocationMapPanner.cpp
    views/RoomSwitchGateKeeper.h
    views/RoomSwitchGateKeeper.cpp
    views/PinnedBanner.h
    views/PinnedBanner.cpp
    views/html_spans.h
    views/html_spans.cpp
    views/TabbedGridPicker.h
    views/TabbedGridPicker.cpp
    views/EmojiPicker.h
    views/EmojiPicker.cpp
    views/EncryptionSetupOverlay.h
    views/EncryptionSetupOverlay.cpp
    views/StickerPicker.h
    views/StickerPicker.cpp
    views/image_pack_order.h
    views/image_pack_order.cpp
    views/ThreadListView.h
    views/ThreadListView.cpp
    views/ThreadView.h
    views/ThreadView.cpp
    views/VerificationBanner.h
    views/VerificationBanner.cpp
    views/DatePickerView.h
    views/DatePickerView.cpp
    views/RoomHeader.h
    views/RoomHeader.cpp
    views/RoomView.h
    views/RoomView.cpp
    views/ComposeBar.h
    views/ComposeBar.cpp
    views/media_drop.h
    views/media_drop.cpp
    views/UserInfo.h
    views/UserInfo.cpp
    views/AccountPicker.h
    views/AccountPicker.cpp
    views/MediaOverlayBase.h
    views/MediaOverlayBase.cpp
    views/CameraWidget.h
    views/CameraWidget.cpp
    views/ImageViewerOverlay.h
    views/ImageViewerOverlay.cpp
    views/VideoViewerOverlay.h
    views/VideoViewerOverlay.cpp
    views/QuickSwitcher.h
    views/QuickSwitcher.cpp
    views/ForwardRoomPicker.h
    views/ForwardRoomPicker.cpp
    views/MessageSearchView.h
    views/MessageSearchView.cpp
    views/ImagePackTileGridBase.h
    views/ImagePackTileGridBase.cpp
    views/ImagePackEditorView.h
    views/ImagePackEditorView.cpp
    views/RoomMediaView.h
    views/RoomMediaView.cpp
    views/RoomSearchBar.h
    views/RoomSearchBar.cpp
    views/ShortcodeEngine.h
    views/ShortcodeEngine.cpp
    views/SlashCommandEngine.h
    views/SlashCommandEngine.cpp
    views/ListPopupBase.h
    views/ListPopupBase.cpp
    views/SlashCommandPopup.h
    views/SlashCommandPopup.cpp
    views/SlashCommandController.h
    views/SlashCommandController.cpp
    views/PopupMenu.h
    views/PopupMenu.cpp
    views/ShortcodePopup.h
    views/ShortcodePopup.cpp
    views/ShortcodeController.h
    views/ShortcodeController.cpp
    views/GifEngine.h
    views/GifEngine.cpp
    views/GifPopup.h
    views/GifPopup.cpp
    views/GifController.h
    views/GifController.cpp
    views/MentionEngine.h
    views/MentionEngine.cpp
    views/MentionPopup.h
    views/MentionPopup.cpp
    views/MentionController.h
    views/MentionController.cpp
    views/ComposePopups.h
    views/JoinRoomView.h
    views/JoinRoomView.cpp
    views/UserProfilePanel.h
    views/UserProfilePanel.cpp
    views/InviteCard.h
    views/InviteCard.cpp
    views/RoomPreviewView.h
    views/RoomPreviewView.cpp
    views/SpaceRootView.h
    views/SpaceRootView.cpp
    views/AvatarEditControl.h
    views/AvatarEditControl.cpp
    views/RoomInfoPanel.h
    views/RoomInfoPanel.cpp
    views/RoomSettingsView.h
    views/RoomSettingsView.cpp
    views/AlertDialog.h
    views/AlertDialog.cpp
    views/ConfirmDialog.h
    views/ConfirmDialog.cpp
    views/MainAppWidget.h
    views/MainAppWidget.cpp
    views/PopoutRoomWidget.h
    views/PopoutRoomWidget.cpp
    views/SettingsView.h
    views/SettingsView.cpp
    views/settings/SettingsPage.h
    views/settings/SettingsPage.cpp
    views/settings/SettingsGroup.h
    views/settings/SettingsGroup.cpp
    views/settings/AccountSection.h
    views/settings/AccountSection.cpp
    views/settings/AppearanceSection.h
    views/settings/AppearanceSection.cpp
    views/settings/LanguageSection.h
    views/settings/LanguageSection.cpp
    views/settings/NotificationsSection.h
    views/settings/NotificationsSection.cpp
    views/settings/MediaSection.h
    views/settings/MediaSection.cpp
    views/settings/ServerSection.h
    views/settings/ServerSection.cpp
    views/settings/DevicesSection.h
    views/settings/DevicesSection.cpp
    views/settings/AboutSection.h
    views/settings/AboutSection.cpp
    views/settings/AdvancedSection.h
    views/settings/AdvancedSection.cpp
    views/settings/PrivacySection.h
    views/settings/PrivacySection.cpp
    views/settings/RoomGeneralSection.h
    views/settings/RoomGeneralSection.cpp
    views/settings/RoomMediaSection.h
    views/settings/RoomMediaSection.cpp
    views/settings/RoomSecuritySection.h
    views/settings/RoomSecuritySection.cpp
    views/settings/RoomPermissionsSection.h
    views/settings/RoomPermissionsSection.cpp
    views/settings/UserPackEditor.h
    views/settings/UserPackEditor.cpp
    views/settings/KnownPacksList.h
    views/settings/KnownPacksList.cpp
    views/settings/ImagePacksSection.h
    views/settings/ImagePacksSection.cpp
)

# Per-platform backend sources (canvas_*, host_*, audio/video/*_<platform>,
# …) live in each ui/<platform>/tk/ and are added directly to that
# platform's own executable target by its own CMakeLists.txt. The exception
# is GStreamer-based video decode/capture, genuinely shared between the two
# Linux backends (qt6, gtk) rather than single-platform, which stays here.
if(TESSERACT_UI STREQUAL "qt6" OR TESSERACT_UI STREQUAL "gtk" OR TESSERACT_UI STREQUAL "linux")
    list(APPEND _tk_sources
        tk/video_decode_gst.cpp
        tk/gst_hw_probe.cpp
    )
endif()

# VideoCapture — unconditional so CameraWidget (/selfie) works without calls.
list(APPEND _tk_sources
    tk/video_capture.cpp
    tk/video_capture_gst.cpp
)
if(TESSERACT_UI STREQUAL "macos")
    # video_capture_macos.mm lives in ui/macos/tk/ and is added by
    # ui/macos/CMakeLists.txt instead — this target has no macos sources.
    list(REMOVE_ITEM _tk_sources tk/video_capture_gst.cpp)
elseif(TESSERACT_UI STREQUAL "win32")
    # video_capture_win32.cpp lives in ui/windows/tk/ and is added by
    # ui/windows/CMakeLists.txt instead — this target has no win32 sources.
    list(REMOVE_ITEM _tk_sources tk/video_capture_gst.cpp)
endif()

list(APPEND _tk_sources
    views/IncomingCallBanner.h
    views/IncomingCallBanner.cpp
    views/ParticipantTile.h
    views/ParticipantTile.cpp
    views/CallOverlayWidget.h
    views/CallOverlayWidget.cpp
    views/ScreenPickerWidget.h
    views/ScreenPickerWidget.cpp
    app/CallWindowBase.h
    app/CallWindowBase.cpp
    tk/screen_capture.cpp
    tk/screen_capture_gst.cpp
)
if(TESSERACT_UI STREQUAL "macos")
    # screen_capture_macos.mm lives in ui/macos/tk/ and is added by
    # ui/macos/CMakeLists.txt instead — this target has no macos
    # sources.
    list(REMOVE_ITEM _tk_sources tk/screen_capture_gst.cpp)
elseif(TESSERACT_UI STREQUAL "win32")
    # screen_capture_win32.cpp lives in ui/windows/tk/ and is added by
    # ui/windows/CMakeLists.txt instead — this target has no win32
    # sources.
    list(REMOVE_ITEM _tk_sources tk/screen_capture_gst.cpp)
else()
    list(APPEND _tk_sources tk/screen_capture_portal.cpp)
endif()

add_library(tesseract_tk STATIC ${_tk_sources})

# svg.cpp is a single-header-library implementation TU (NANOSVG_IMPLEMENTATION /
# NANOSVGRAST_IMPLEMENTATION) — it gains nothing from unity-batch merging and
# excluding it removes any risk from those un-#undef'd implementation macros.
set_source_files_properties(tk/svg.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# Embed the application icon PNG as a C byte array so BrandView can decode it
# cross-platform without per-shell resource loading. Runs at configure time.
set(_icon_png "${CMAKE_SOURCE_DIR}/ui/icons/tesseract_160.png")
if(EXISTS "${_icon_png}")
    file(READ "${_icon_png}" _icon_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _icon_bytes "${_icon_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/brand_icon.h"
        "#pragma once\n"
        "#include <cstdint>\n\n"
        "static constexpr std::uint8_t kBrandIconPng[] = { ${_icon_bytes} };\n"
    )
    target_compile_definitions(tesseract_tk PRIVATE TESSERACT_HAS_BRAND_ICON=1)
endif()

# Embed Lucide SVG icons as C byte arrays (rasterized + tinted at runtime by
# nanosvg). All app icons live under ui/icons/lucide/lucide-<name>.svg and are
# exposed to the shared views as static constexpr k*Svg arrays in icons.h.
# icons.h is regenerated at build time whenever any source SVG changes.
set(_lucide_dir "${CMAKE_SOURCE_DIR}/ui/icons/lucide")
set(_icons_h   "${CMAKE_CURRENT_BINARY_DIR}/icons.h")
set(_gen_script "${CMAKE_SOURCE_DIR}/cmake/generate_icons.cmake")

set(_icon_svgs
    "${_lucide_dir}/lucide-laugh.svg"
    "${_lucide_dir}/lucide-sticker.svg"
    "${_lucide_dir}/lucide-mic.svg"
    "${_lucide_dir}/lucide-stop.svg"
    "${_lucide_dir}/lucide-reply.svg"
    "${_lucide_dir}/lucide-thread.svg"
    "${_lucide_dir}/lucide-edit.svg"
    "${_lucide_dir}/lucide-more.svg"
    "${_lucide_dir}/lucide-redact.svg"
    "${_lucide_dir}/lucide-pin.svg"
    "${_lucide_dir}/lucide-download.svg"
    "${_lucide_dir}/lucide-close.svg"
    "${_lucide_dir}/lucide-copy.svg"
    "${_lucide_dir}/lucide-play.svg"
    "${_lucide_dir}/lucide-join.svg"
    "${_lucide_dir}/lucide-jump-to-date.svg"
    "${_lucide_dir}/lucide-threadlist.svg"
    "${_lucide_dir}/lucide-search.svg"
    "${_lucide_dir}/lucide-chevron-up.svg"
    "${_lucide_dir}/lucide-chevron-down.svg"
    "${_lucide_dir}/lucide-forward.svg"
    "${_lucide_dir}/lucide-phone.svg"
    "${_lucide_dir}/lucide-phone-off.svg"
    "${_lucide_dir}/lucide-mic-off.svg"
    "${_lucide_dir}/lucide-video.svg"
    "${_lucide_dir}/lucide-video-off.svg"
    "${_lucide_dir}/lucide-expand.svg"
    "${_lucide_dir}/lucide-minimize.svg"
    "${_lucide_dir}/lucide-pip.svg"
    "${_lucide_dir}/lucide-monitor.svg"
    "${_lucide_dir}/lucide-wrench.svg"
    "${_lucide_dir}/lucide-plus.svg"
)

add_custom_command(
    OUTPUT  "${_icons_h}"
    COMMAND "${CMAKE_COMMAND}"
            "-DLUCIDE_DIR=${_lucide_dir}"
            "-DICONS_H_OUT=${_icons_h}"
            -P "${_gen_script}"
    DEPENDS ${_icon_svgs} "${_gen_script}"
    COMMENT "Generating icons.h from Lucide SVGs"
    VERBATIM
)

add_custom_target(tesseract_icons DEPENDS "${_icons_h}")
add_dependencies(tesseract_tk tesseract_icons)

target_include_directories(tesseract_tk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Update-checker configuration. TESSERACT_GITHUB_REPO is the "owner/repo"
# slug passed to the GitHub Releases API. When left empty (the default),
# trigger_update_check_() is a no-op and no network request is made.
if(TESSERACT_GITHUB_REPO)
    target_compile_definitions(tesseract_tk PUBLIC
        TESSERACT_GITHUB_REPO="${TESSERACT_GITHUB_REPO}"
    )
endif()

target_compile_features(tesseract_tk PUBLIC cxx_std_20)

target_include_directories(tesseract_tk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The toolkit interacts with shared visual constants and (eventually) the
# client event-handler interface.
target_link_libraries(tesseract_tk PUBLIC
    tesseract_client
)

# Precompiled headers: the cross-platform std base shared by ~90 TUs. The
# active backend appends its heavy framework umbrella headers below. (Project
# headers like tk/theme.h are intentionally excluded — see the root option.)
tesseract_target_pch(tesseract_tk
    <algorithm>
    <memory>
    <utility>
    <vector>
    <string>
    <string_view>
    <cstring>
    <cstdint>
    <cmath>
    <chrono>
    <unordered_map>
    <unordered_set>
    <mutex>
    <atomic>
    <functional>
    <optional>
)

# All platform-UI-toolkit (Qt6/GTK4/AppKit) and platform-media (Media
# Foundation/AVFoundation) dependencies live in each ui/<platform>/
# CMakeLists.txt now, alongside the tk:: backend sources that need them
# (see tk/canvas_d2d.* etc.'s move out of this directory). The one exception
# is GStreamer: video_decode_gst.cpp and gst_hw_probe.cpp are genuinely
# shared between the two Linux backends (qt6, gtk) rather than
# single-platform, and stay compiled into tesseract_tk here — its PUBLIC
# link propagates GStreamer to whichever of ui/linux-qt / ui/linux-gtk
# links this library.
if(TESSERACT_UI STREQUAL "qt6" OR TESSERACT_UI STREQUAL "gtk" OR TESSERACT_UI STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TK_GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-video-1.0
        gio-2.0
    )
    target_link_libraries(tesseract_tk PUBLIC
        PkgConfig::TK_GST
        ${CMAKE_DL_LIBS}
    )
    tesseract_target_pch(tesseract_tk
        <gst/gst.h>
        <glib.h>
    )
endif()
