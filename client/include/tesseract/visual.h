#pragma once

// Shared visual constants used by every Tesseract UI (Qt6, GTK4, Win32,
// AppKit). Centralising sizes, paddings, and image caps here is what
// keeps the four native renderers visually consistent. Font sizes and
// reaction-chip dimensions are runtime-tunable and live in
// tesseract::Settings (settings.h).
//
// Colour decisions live in docs/UI-PARITY.md as semantic tokens with
// canonical hex values; each platform translates those tokens to its
// own colour type (NSColor on macOS — using system semantic colours so
// dark mode keeps adapting; QColor / GdkRGBA / Gdiplus::Color elsewhere).

namespace tesseract::visual
{

// ── Sidebar / room list ─────────────────────────────────────────────────
inline constexpr int kSidebarWidth = 260;
inline constexpr int kRoomAvatarSize = 36;
inline constexpr int kRoomRowHeight = 48;

// Cache resolution for user and room avatars. Larger than the message-list
// render size (32 px) so the RoomInfoPanel and UserProfilePanel can display
// them at 72 px without upscaling artefacts.
inline constexpr int kAvatarCacheSize = 80;
inline constexpr int kUnreadBadgeMinWidth = 20;
inline constexpr int kUnreadBadgeHeight = 18;

// ── Message rows ────────────────────────────────────────────────────────
inline constexpr int kMsgAvatarSize = 32;
inline constexpr int kMsgAvatarGap = 8; // avatar ↔ body gap
inline constexpr int kMsgRowVerticalPad = 6;
inline constexpr int kMsgSenderNameHeight = 16;
inline constexpr int kMsgTimestampHeight = 14;

// ── Compose bar ────────────────────────────────────────────────────────
inline constexpr int kComposeMinHeight = 40;
inline constexpr int kComposeMaxHeight = 120;

// ── User strip (account row under the sidebar) ─────────────────────────
inline constexpr int kUserStripHeight = 48;

// ── Spacing scale ──────────────────────────────────────────────────────
inline constexpr int kSpaceXS = 4;
inline constexpr int kSpaceSM = 8;
inline constexpr int kSpaceMD = 12;
inline constexpr int kSpaceLG = 16;

// ── Corner radius scale ─────────────────────────────────────────────────
// Buttons, cards, popups, and hover/selection highlights share these two
// radii so the app reads as one consistent system. Smaller one-off radii
// (scrollbar thumbs, tooltips, tab pills) stay local where they're
// deliberately tighter than a card.
inline constexpr float kRadiusSM = 6.0f;
inline constexpr float kRadiusMD = 8.0f;

// ── Message bubble layout ──────────────────────────────────────────────
inline constexpr int kMsgMaxWidth = 520;

// ── Inline media caps ──────────────────────────────────────────────────
inline constexpr int kMaxInlineImageWidth = 320;
inline constexpr int kMaxInlineImageHeight = 200;
inline constexpr int kStickerSize = 256;

// Decode bound for the lightbox full-resolution viewer. Large enough that the
// 8x zoom cap still looks crisp; bounded so a huge source can't trip Qt's
// 256 MB QImage guard (4096^2 RGBA ~= 64 MB). decode_image_ only downscales
// when native exceeds this.
inline constexpr int kViewerFullresMax = 4096;

} // namespace tesseract::visual
