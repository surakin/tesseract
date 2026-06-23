cmake_minimum_required(VERSION 3.25)

# tesseract_tk — the shared UI toolkit core. Cross-platform widget classes,
# layout, theme, and the abstract Canvas interface (see tk/canvas.h).
#
# Backend translation units (Direct2D on Win32, CoreGraphics on macOS,
# QPainter on Qt6, Cairo+Pango on GTK4) live in this same target but are
# compiled conditionally based on TESSERACT_UI.

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
    tk/combobox.h
    tk/combobox.cpp
    tk/controls.h
    tk/controls.cpp
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
    views/RoomInfoPanel.h
    views/RoomInfoPanel.cpp
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
    views/settings/PrivacySection.h
    views/settings/PrivacySection.cpp
)

if(TESSERACT_UI STREQUAL "win32")
    list(APPEND _tk_sources
        tk/canvas_d2d.h
        tk/canvas_d2d.cpp
        tk/host_win32.h
        tk/host_win32.cpp
        tk/audio_win32.cpp
        tk/audio_capture_win32.cpp
        tk/audio_playback_win32.cpp
        tk/video_win32.cpp
        tk/video_decode_mf.cpp
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    list(APPEND _tk_sources
        tk/canvas_qpainter.h
        tk/canvas_qpainter.cpp
        tk/host_qt.h
        tk/host_qt.cpp
        tk/audio_qt.cpp
        tk/audio_capture_qt.cpp
        tk/audio_playback_qt.cpp
        tk/video_qt.cpp
        tk/video_decode_gst.cpp
        tk/gst_hw_probe.cpp
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    list(APPEND _tk_sources
        tk/canvas_cairo.h
        tk/canvas_cairo.cpp
        tk/host_gtk.h
        tk/host_gtk.cpp
        tk/audio_gtk.cpp
        tk/audio_capture_gtk.cpp
        tk/audio_playback_gtk.cpp
        tk/video_gtk.cpp
        tk/video_decode_gst.cpp
        tk/gst_hw_probe.cpp
    )
elseif(TESSERACT_UI STREQUAL "macos")
    list(APPEND _tk_sources
        tk/canvas_cg.h
        tk/canvas_cg.cpp
        tk/host_macos.h
        tk/host_macos.mm
        tk/audio_macos.mm
        tk/audio_capture_macos.mm
        tk/audio_playback_macos.mm
        tk/video_macos.mm
        tk/video_decode_av.mm
    )
endif()

if(TESSERACT_ENABLE_CALLS)
    list(APPEND _tk_sources
        views/IncomingCallBanner.h
        views/IncomingCallBanner.cpp
        views/ParticipantTile.h
        views/ParticipantTile.cpp
        views/CallOverlayWidget.h
        views/CallOverlayWidget.cpp
        tk/video_capture.cpp
        tk/video_capture_gst.cpp
        app/CallWindowBase.h
        app/CallWindowBase.cpp
    )
    if(TESSERACT_UI STREQUAL "macos")
        list(REMOVE_ITEM _tk_sources tk/video_capture_gst.cpp)
        list(APPEND _tk_sources tk/video_capture_macos.mm)
    elseif(TESSERACT_UI STREQUAL "win32")
        list(REMOVE_ITEM _tk_sources tk/video_capture_gst.cpp)
        list(APPEND _tk_sources tk/video_capture_win32.cpp)
    endif()
endif()

add_library(tesseract_tk STATIC ${_tk_sources})

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

if(TESSERACT_UI STREQUAL "win32")
    if(MSVC)
        target_compile_options(tesseract_tk PRIVATE /utf-8)
    endif()
    target_compile_definitions(tesseract_tk PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        # GetDpiForWindow requires Windows 10 (1607); D2D/DWrite/WIC are
        # available from 8.1, so 0x0A00 is the floor for the toolkit.
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(tesseract_tk PUBLIC
        d2d1
        d3d11            # Direct3D device backing the D2D swap-chain surface
        dxgi             # DXGI swap-chain (flip-model presentation)
        dwrite
        windowscodecs    # WIC: PNG/JPEG decode for tk::Image
        ole32
        comctl32         # SetWindowSubclass, EM_SETCUEBANNER, etc.
        user32
        imm32            # ImmGetContext/ImmReleaseContext — Win32RichEditArea IME support
        mf               # Media Foundation: IMFMediaEngine (audio_win32.cpp)
        mfplat
        mfuuid
        shlwapi          # SHCreateMemStream (also used in host_win32.cpp)
    )
    # WIN32_LEAN_AND_MEAN / NOMINMAX are set as target definitions above, so the
    # PCH compiles windows.h the same way the TUs do.
    tesseract_target_pch(tesseract_tk
        <windows.h>
        <wrl/client.h>
        <d2d1.h>
        <dwrite.h>
    )
elseif(TESSERACT_UI STREQUAL "qt6")
    # Multimedia is used by audio_qt.cpp + video_qt.cpp (QMediaPlayer).
    # GStreamer is used by video_decode_gst.cpp for off-thread MP4 frame
    # extraction in the GIF strip (the same libraries as the GTK4 backend).
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TK_GST_QT REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gio-2.0
    )
    target_link_libraries(tesseract_tk PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Multimedia
        PkgConfig::TK_GST_QT
        ${CMAKE_DL_LIBS}
    )
    tesseract_target_pch(tesseract_tk
        <QtCore/QtCore>
        <QtGui/QtGui>
        <QtMultimedia/QtMultimedia>
    )
elseif(TESSERACT_UI STREQUAL "gtk")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TK_GTK4 REQUIRED IMPORTED_TARGET
        gtk4
        pangocairo
        gdk-pixbuf-2.0
    )
    # GStreamer + gstreamer-app for MSC3245 voice playback (audio_gtk.cpp).
    # The opus decoder is provided by gst-plugins-base; package this runtime
    # dependency in installer manifests.
    pkg_check_modules(TK_GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-app-1.0
        gio-2.0
    )
    target_link_libraries(tesseract_tk PUBLIC
        PkgConfig::TK_GTK4
        PkgConfig::TK_GST
        ${CMAKE_DL_LIBS}
    )
    tesseract_target_pch(tesseract_tk
        <gtk/gtk.h>
        <gst/gst.h>
        <cairo.h>
        <glib.h>
    )
elseif(TESSERACT_UI STREQUAL "macos")
    target_link_libraries(tesseract_tk PUBLIC
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
        "-framework AppKit"
        "-framework Foundation"
        "-framework UniformTypeIdentifiers"
        # MSC3245 voice playback (audio_macos.mm) uses AVAudioPlayer.
        "-framework AVFoundation"
        "-framework AudioToolbox"
    )
endif()
