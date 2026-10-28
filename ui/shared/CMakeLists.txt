cmake_minimum_required(VERSION 3.30)

# Four static libraries, layered tk <- platform <- views <- app (shells link
# tesseract_app):
#   tesseract_tk       - toolkit core: widget classes, layout, theme, the
#                        abstract Canvas interface (see tk/canvas.h).
#   tesseract_platform - OS services (capture, decode, location); platform/.
#   tesseract_views    - shared views built on tk.
#   tesseract_app      - ShellBase and the other platform-agnostic shell logic.
#
# Per-platform backend translation units (Direct2D on Win32, CoreGraphics on
# macOS, QPainter on Qt6, Cairo+Pango on GTK4) live in each ui/<platform>/tk/
# instead, compiled directly into that platform's own executable target.

set(_tk_sources
    tk/audio.h
    tk/audio_capture.h
    tk/video.h
    tk/canvas.h
    tk/canvas_common.cpp
    tk/font_config.h
    tk/font_config.cpp
    tk/emoji_segmentation.h
    tk/emoji_segmentation.cpp
    tk/image_sniff.h
    tk/image_sniff.cpp
    tk/emoji_support.h
    tk/emoji_support.cpp
    tk/host.h
    tk/surface.h
    tk/host.cpp
    tk/theme.h
    tk/theme.cpp
    tk/color_contrast.h
    tk/color_contrast.cpp
    tk/i18n.h
    tk/i18n.cpp
    tk/cache_key.h
    tk/cache_key.cpp
    tk/anim_image_cache.h
    tk/anim_image_cache.cpp
    tk/pixmap_cache.h
    tk/pixmap_cache.cpp
    tk/pill.h
    tk/pill.cpp
    tk/widget.h
    tk/widget.cpp
    tk/access_tree.h
    tk/access_tree.cpp
    tk/layout.h
    tk/layout.cpp
    tk/form_layout.h
    tk/form_layout.cpp
    tk/combobox.h
    tk/combobox.cpp
    tk/combo_button.h
    tk/combo_button.cpp
    tk/searchable_picker.h
    tk/searchable_picker.cpp
    tk/controls.h
    tk/controls.cpp
    tk/radio.h
    tk/radio.cpp
    tk/slider.cpp
    tk/group_box.h
    tk/group_box.cpp
    tk/badge_flow.h
    tk/badge_flow.cpp
    tk/text_field.h
    tk/text_field.cpp
    tk/text_area.h
    tk/text_area.cpp
    tk/image_view.h
    tk/image_view.cpp
    tk/scrollable_base.h
    tk/scrollable_base.cpp
    tk/scroll_view.h
    tk/scroll_view.cpp
    tk/list_view.h
    tk/list_view.cpp
    tk/tab_bar.h
    tk/tab_bar.cpp
    tk/side_tab_view.h
    tk/side_tab_view.cpp
    tk/tab_view.h
    tk/tab_view.cpp
    tk/svg.cpp
    tk/status_icons.h
    tk/status_icons.cpp
    tk/blurhash.h
    tk/blurhash.cpp
    tk/media_disk_cache.h
    tk/media_disk_cache.cpp
    tk/compressed_image_cache.h
    tk/compressed_image_cache.cpp
    tk/interval_timer.h
    tk/hover_area.h
    tk/keyboard_target.h
    tk/keyboard_target.cpp
    tk/key_chord.h
    tk/key_chord.cpp
    tk/row_cursor.h
    tk/row_cursor.cpp
    tk/tooltip.h
    tk/tooltip.cpp
    tk/toast.h
    tk/toast.cpp
    tk/animator.h
    tk/kinetic_scroller.h
    tk/kinetic_scroller.cpp
)

set(_views_sources
    views/font_prefs.h
    views/font_prefs.cpp
    views/launch_baseline.h
    views/launch_baseline.cpp
    views/update_checks.h
    views/settings/SettingsController.h
    views/settings/SettingsController.cpp
    views/SlashCommands.h
    views/image_animation.h
    views/image_animation.cpp
    views/SlashCommands.cpp
    views/BrandView.h
    views/BrandView.cpp
    views/LoginView.h
    views/LoginView.cpp
    views/QRGrantView.h
    views/QRGrantView.cpp
    views/RoomListView.h
    views/RoomListView.cpp
    views/sidebar_metrics.h
    views/MessageActions.h
    views/MessageActions.cpp
    views/MessageListView.h
    views/MessageListView.cpp
    views/MessageRowGeometry.h
    views/TimelineMediaController.h
    views/TimelineMediaController.cpp
    views/UrlPreviewCardDisplay.h
    views/UrlPreviewCardDisplay.cpp
    views/poll_logic.h
    views/poll_logic.cpp
    views/PollCardDisplay.h
    views/PollCardDisplay.cpp
    views/CreatePollDialog.h
    views/CreatePollDialog.cpp
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
    views/sdk_text.h
    views/sdk_text.cpp
    views/send_block.h
    views/send_block.cpp
    views/send_block.h
    views/send_block.cpp
    views/table_layout.h
    views/table_layout.cpp
    views/call_tile_layout.h
    views/call_tile_layout.cpp
    views/TabbedGridPicker.h
    views/TabbedGridPicker.cpp
    views/ReceiptGridPopup.h
    views/ReceiptGridPopup.cpp
    views/EmojiPicker.h
    views/EmojiPicker.cpp
    views/EncryptionSetupOverlay.h
    views/EncryptionSetupOverlay.cpp
    views/StickerPicker.h
    views/StickerPicker.cpp
    views/image_pack_order.h
    views/image_pack_order.cpp
    views/ActivityMonitorView.h
    views/ActivityMonitorView.cpp
    views/ThreadListView.h
    views/ThreadListView.cpp
    views/ThreadView.h
    views/ThreadView.cpp
    views/EncryptionReminderBanner.h
    views/EncryptionReminderBanner.cpp
    views/RoomReplacedBanner.h
    views/RoomReplacedBanner.cpp
    views/IdentityChangeBanner.h
    views/IdentityChangeBanner.cpp
    views/sas_emoji_tiles.h
    views/sas_emoji_tiles.cpp
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
    views/SpacePickerView.h
    views/SpacePickerView.cpp
    views/MediaOverlayBase.h
    views/MediaOverlayBase.cpp
    views/CameraWidget.h
    views/CameraWidget.cpp
    views/MediaViewerItem.h
    views/MediaViewerPage.h
    views/media_viewer_items.h
    views/media_viewer_items.cpp
    views/ImageViewerPage.h
    views/ImageViewerPage.cpp
    views/VideoViewerPage.h
    views/VideoViewerPage.cpp
    views/MediaTransportBar.h
    views/MediaTransportBar.cpp
    views/AudioViewerPage.h
    views/AudioViewerPage.cpp
    views/FileViewerPage.h
    views/FileViewerPage.cpp
    views/MediaViewerOverlay.h
    views/MediaViewerOverlay.cpp
    views/QuickSwitcher.h
    views/QuickSwitcher.cpp
    views/MruSwitcher.h
    views/MruSwitcher.cpp
    views/room_chip_strip.h
    views/room_chip_strip.cpp
    views/ForwardRoomPicker.h
    views/ForwardRoomPicker.cpp
    views/InviteDialog.h
    views/InviteDialog.cpp
    views/InviteFieldModel.h
    views/InviteFieldModel.cpp
    views/SpaceAddRoomList.h
    views/SpaceAddRoomList.cpp
    views/SpaceChildRoomGrid.h
    views/SpaceChildRoomGrid.cpp
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
    views/SkinTonePopover.h
    views/SkinTonePopover.cpp
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
    views/CreateRoomView.h
    views/CreateRoomView.cpp
    views/RoomDirectoryView.h
    views/RoomDirectoryView.cpp
    views/ServerPicker.h
    views/ServerPicker.cpp
    views/AddRoomView.h
    views/AddRoomView.cpp
    views/UserProfilePanel.h
    views/UserProfilePanel.cpp
    views/InviteCard.h
    views/InviteCard.cpp
    views/KnockStatusCard.h
    views/KnockStatusCard.cpp
    views/KnockRequestsPanel.h
    views/KnockRequestsPanel.cpp
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
    views/BusyDialog.h
    views/BusyDialog.cpp
    views/ConfirmDialog.h
    views/ConfirmDialog.cpp
    views/KeyboardShortcutsOverlay.h
    views/KeyboardShortcutsOverlay.cpp
    views/ServerBrowserOverlay.h
    views/ServerBrowserOverlay.cpp
    views/shortcut_registry.h
    views/shortcut_registry.cpp
    views/ExportHistoryDialog.h
    views/ExportHistoryDialog.cpp
    views/MainAppWidget.h
    views/MainAppWidget.cpp
    views/PopoutRoomWidget.h
    views/PopoutRoomWidget.cpp
    views/SettingsView.h
    views/SettingsView.cpp
    views/settings/SettingBinder.h
    views/settings/SettingsPage.h
    views/settings/SettingsPage.cpp
    views/settings/SettingsGroup.h
    views/settings/SettingsGroup.cpp
    views/settings/AccountSection.h
    views/settings/AccountSection.cpp
    views/settings/LanguagePicker.h
    views/settings/LanguagePicker.cpp
    views/settings/FontFamilyPicker.h
    views/settings/FontFamilyPicker.cpp
    views/settings/FontsEditor.h
    views/settings/FontsEditor.cpp
    views/settings/TimezonePicker.h
    views/settings/TimezonePicker.cpp
    views/settings/PronounsEditor.h
    views/settings/PronounsEditor.cpp
    views/settings/StatusEditor.h
    views/settings/StatusEditor.cpp
    views/settings/GeneralSection.h
    views/settings/GeneralSection.cpp
    views/settings/AppearanceSection.h
    views/settings/AppearanceSection.cpp
    views/settings/MessageLayoutPreview.h
    views/settings/MessageLayoutPreview.cpp
    views/settings/RoomListPreview.h
    views/settings/RoomListPreview.cpp
    views/settings/LanguageSection.h
    views/settings/LanguageSection.cpp
    views/settings/NetworkSection.h
    views/settings/NetworkSection.cpp
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
    views/settings/RoomBridgeSection.h
    views/settings/RoomBridgeSection.cpp
    views/settings/RoomModerationSection.h
    views/settings/RoomModerationSection.cpp
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

set(_app_sources
    app/ShellBase.h
    app/ShellBase.cpp
    app/ShellBase_accounts.cpp
    app/ShellBase_calls.cpp
    app/ShellBase_encryption.cpp
    app/ShellBase_media_fetch.cpp
    app/ShellBase_media_images.cpp
    app/ShellBase_navigation.cpp
    app/ShellBase_profile.cpp
    app/ShellBase_rooms.cpp
    app/ShellBase_search.cpp
    app/ShellBase_settings.cpp
    app/ShellBase_spaces.cpp
    app/ShellBase_timeline.cpp
    app/ShellBase_windows.cpp
    app/ShellBase_wiring.cpp
    app/DeferredTeardown.h
    app/AccountLifecycleTypes.h
    app/AccountManager.h
    app/MediaTypes.h
    app/AccountManager.cpp
    app/Launch.h
    app/Launch.cpp
    app/EventHandlerBase.h
    app/EventHandlerBase.cpp
    app/RoomWindowBase.h
    app/RoomWindowBase.cpp
    app/RoomPane.h
    app/RoomPane.cpp
    app/ComposerPopups.h
    app/ComposerPopups.cpp
    app/HistoryExportController.h
    app/HistoryExportController.cpp
    app/PresenceTracker.h
    app/PresenceTracker.cpp
    app/ActivityRegistry.h
    app/ActivityRegistry.cpp
    app/AuxWindowBase.h
    app/PowerPolicy.h
    app/PowerPolicy.cpp
    app/ThreadPanelController.h
    app/ThreadPanelController.cpp
    app/EncryptionFlowController.h
    app/EncryptionFlowController.cpp
    app/SearchBackend.h
    app/SearchBackend.cpp
    app/MediaPlaybackHub.h
    app/MediaPlaybackHub.cpp
    app/shell_helpers.h
    app/shell_helpers.cpp
    app/jxl_decode.h
    app/jxl_decode.cpp
    app/image_send.h
    app/image_send.cpp
    app/SendPipeline.h
    app/SendPipeline.cpp
    app/WorkerPool.h
    app/WorkerPool.cpp
    app/status_links.h
    app/status_links.cpp
    app/UpdateChecker.h
    app/GithubUpdateChecker.h
    app/GithubUpdateChecker.cpp
    app/AurUpdateChecker.h
    app/AurUpdateChecker.cpp
)

if(TESSERACT_ENABLE_SCREENSHOT_MODE)
    list(APPEND _app_sources
        app/ScreenshotDirector.h
        app/ScreenshotDirector.cpp
        app/ScreenshotFixture.h
        app/ScreenshotFixture.cpp)
endif()

# Per-platform backend sources (canvas_*, host_*, audio/video/*_<platform>,
# …) live in each ui/<platform>/tk/ and are added directly to that
# platform's own executable target by its own CMakeLists.txt. The OS service
# layer (capture, decode, location, single-instance) is tesseract_platform.

list(APPEND _views_sources
    views/CallBanner.h
    views/CallBanner.cpp
    views/ParticipantTile.h
    views/ParticipantTile.cpp
    views/CallOverlayWidget.h
    views/CallOverlayWidget.cpp
    views/CallLobbyView.h
    views/CallLobbyView.cpp
    views/ScreenPickerWidget.h
    views/ScreenPickerWidget.cpp
)
list(APPEND _app_sources
    app/CallWindowBase.h
    app/CallWindowBase.cpp
)
add_library(tesseract_tk    STATIC ${_tk_sources})
add_subdirectory(platform)
add_library(tesseract_views STATIC ${_views_sources})
add_library(tesseract_app   STATIC ${_app_sources})

# svg.cpp is a single-header-library implementation TU (NANOSVG_IMPLEMENTATION /
# NANOSVGRAST_IMPLEMENTATION) — it gains nothing from unity-batch merging and
# excluding it removes any risk from those un-#undef'd implementation macros.
set_source_files_properties(tk/svg.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# status_icons.cpp #includes the generated icons.h; keep it out of the unity
# blobs so its include set can't reorder a neighbour's (some view headers lean
# on unity include-leakage for tk::PaintCtx etc.).
set_source_files_properties(tk/status_icons.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# emoji_segmentation.cpp is a small self-contained TU (canvas.h only); keeping
# it out of the unity blobs avoids reordering a neighbour's include set (same
# tk::PaintCtx leakage reason as status_icons.cpp).
set_source_files_properties(tk/emoji_segmentation.cpp tk/image_sniff.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

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
    target_compile_definitions(tesseract_views PRIVATE TESSERACT_HAS_BRAND_ICON=1)
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
    "${_lucide_dir}/lucide-send-horizontal.svg"
    "${_lucide_dir}/lucide-reply.svg"
    "${_lucide_dir}/lucide-thread.svg"
    "${_lucide_dir}/lucide-edit.svg"
    "${_lucide_dir}/lucide-more.svg"
    "${_lucide_dir}/lucide-redact.svg"
    "${_lucide_dir}/lucide-pin.svg"
    "${_lucide_dir}/lucide-pin-off.svg"
    "${_lucide_dir}/lucide-house.svg"
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
    "${_lucide_dir}/lucide-external-link.svg"
    "${_lucide_dir}/lucide-app-window.svg"
    "${_lucide_dir}/lucide-log-out.svg"
    "${_lucide_dir}/lucide-x.svg"
    "${_lucide_dir}/lucide-minus.svg"
    "${_lucide_dir}/lucide-square.svg"
    "${_lucide_dir}/lucide-battery-low.svg"
    "${_lucide_dir}/lucide-list-checks.svg"
    "${_lucide_dir}/lucide-check-check.svg"
    "${_lucide_dir}/lucide-check.svg"
    "${_lucide_dir}/lucide-lock-keyhole.svg"
    "${_lucide_dir}/lucide-lock-keyhole-open.svg"
    "${_lucide_dir}/lucide-eye.svg"
    "${_lucide_dir}/lucide-cable.svg"
    "${_lucide_dir}/lucide-user-round-plus.svg"
    "${_lucide_dir}/lucide-user-round.svg"
    "${_lucide_dir}/lucide-user-round-minus.svg"
    "${_lucide_dir}/lucide-ban.svg"
    "${_lucide_dir}/lucide-languages.svg"
    "${_lucide_dir}/lucide-chevron-right.svg"
    "${_lucide_dir}/lucide-chevron-left.svg"
    "${_lucide_dir}/lucide-music.svg"
    "${_lucide_dir}/lucide-file.svg"
    "${_lucide_dir}/lucide-arrow-down.svg"
    "${_lucide_dir}/lucide-arrow-up.svg"
    "${_lucide_dir}/lucide-shield-check.svg"
    "${_lucide_dir}/lucide-shield-alert.svg"
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
add_dependencies(tesseract_views tesseract_icons)

# icons.h (tk/status_icons.cpp, many views) and brand_icon.h (BrandView).
target_include_directories(tesseract_tk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(tesseract_views PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Update-checker configuration. Exactly one of TESSERACT_GITHUB_REPO /
# TESSERACT_AUR_PACKAGE should be set. TESSERACT_UPDATE_CHECKS is the
# umbrella macro UI code checks to gate the "check for updates" toggle,
# independent of which backend is active. When neither is set (or on a
# redundant AUR configure, TESSERACT_GITHUB_REPO cleared), trigger_update_check_()
# is a no-op and no network request is made.
if(TESSERACT_GITHUB_REPO)
    target_compile_definitions(tesseract_views PUBLIC
        TESSERACT_GITHUB_REPO="${TESSERACT_GITHUB_REPO}"
    )
endif()
if(TESSERACT_AUR_PACKAGE)
    target_compile_definitions(tesseract_views PUBLIC
        TESSERACT_AUR_PACKAGE="${TESSERACT_AUR_PACKAGE}"
    )
endif()
if(TESSERACT_GITHUB_REPO OR TESSERACT_AUR_PACKAGE)
    target_compile_definitions(tesseract_views PUBLIC TESSERACT_UPDATE_CHECKS)
endif()

target_compile_features(tesseract_tk PUBLIC cxx_std_20)

target_include_directories(tesseract_tk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# The toolkit interacts with shared visual constants and (eventually) the
# client event-handler interface. Layering: tk <- platform <- views <- app.
target_link_libraries(tesseract_tk PUBLIC
    tesseract_client
)
target_link_libraries(tesseract_views PUBLIC tesseract_platform)
target_link_libraries(tesseract_app   PUBLIC tesseract_views)

# tesseract_client links nlohmann_json PRIVATE, so it isn't propagated here —
# link it directly (header-only, so this is purely an include-path grant).
# views/SlashCommands.cpp (MSC4391 bot-command `arguments`) and
# app/shell_helpers.cpp use it.
foreach(_t tesseract_tk tesseract_views tesseract_app)
    target_link_libraries(${_t} PRIVATE nlohmann_json::nlohmann_json)
endforeach()

# Precompiled headers: the cross-platform std base shared by ~90 TUs. The
# active backend appends its heavy framework umbrella headers (the GStreamer
# ones for tesseract_platform are in platform/CMakeLists.txt). (Project
# headers like tk/theme.h are intentionally excluded — see the root option.)
foreach(_t tesseract_tk tesseract_views tesseract_app)
    tesseract_target_pch(${_t}
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
endforeach()

# The platform-service backends (GStreamer, Media Foundation, AVFoundation,
# CoreLocation, WinRT geolocation) and their libraries live on tesseract_platform
# (platform/CMakeLists.txt). Only the UI-toolkit deps (Qt6/GTK4/AppKit, plus the
# Host-vended audio/video players) stay in each ui/<platform>/CMakeLists.txt.
