#include "MessageListView.h"
#include "html_spans.h"
#include "media_utils.h"

#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tesseract::views {

MessageRowData make_row_data(const tesseract::Event& ev, const std::string& my_user_id) {
    using Kind = MessageRowData::Kind;
    MessageRowData row;
    row.event_id          = ev.event_id;
    row.sender            = ev.sender;
    row.sender_name       = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body              = ev.body;
    row.formatted_body    = ev.formatted_body;
    row.timestamp_ms      = ev.timestamp;
    row.is_own            = !my_user_id.empty() && ev.sender == my_user_id;
    row.reactions         = ev.reactions;
    row.read_receipts     = ev.read_receipts;

    row.in_reply_to_id          = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    row.in_reply_to_body        = ev.in_reply_to_body;
    row.is_edited               = ev.is_edited;

    switch (ev.type) {
        case tesseract::EventType::Text:    row.kind = Kind::Text;    break;
        case tesseract::EventType::Image: {
            row.kind = Kind::Image;
            const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
            row.media_url            = img.image_url;
            row.media_w              = static_cast<int>(img.width);
            row.media_h              = static_cast<int>(img.height);
            row.has_filename_caption = !img.filename.empty();
            break;
        }
        case tesseract::EventType::Sticker: {
            row.kind = Kind::Sticker;
            const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
            row.media_url = s.image_url;
            row.media_w   = static_cast<int>(s.width);
            row.media_h   = static_cast<int>(s.height);
            break;
        }
        case tesseract::EventType::File: {
            row.kind = Kind::File;
            const auto& f = static_cast<const tesseract::FileEvent&>(ev);
            row.file_name = f.file_name;
            row.file_size = f.file_size;
            row.media_url = f.file_url;
            break;
        }
        case tesseract::EventType::Voice: {
            row.kind = Kind::Voice;
            const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
            row.audio_source = v.audio_source;
            row.audio_mime   = v.mime_type;
            row.duration_ms  = v.duration_ms;
            row.waveform     = v.waveform;
            break;
        }
        case tesseract::EventType::Video: {
            row.kind = Kind::Video;
            const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
            row.media_url            = vid.video_url;
            row.video_thumb_url      = vid.thumbnail_url.empty()
                                       ? ("thumb::" + ev.event_id)
                                       : vid.thumbnail_url;
            row.video_mime           = vid.mime_type;
            row.media_w              = static_cast<int>(vid.width);
            row.media_h              = static_cast<int>(vid.height);
            row.duration_ms          = vid.duration_ms;
            row.has_filename_caption = !vid.filename.empty();
            row.video_autoplay       = vid.autoplay;
            row.video_loop           = vid.loop;
            row.video_no_audio       = vid.no_audio;
            row.video_hide_controls  = vid.hide_controls;
            row.video_gif            = vid.gif;
            break;
        }
        case tesseract::EventType::Redacted:      row.kind = Kind::Redacted;      break;
        case tesseract::EventType::Unhandled:     row.kind = Kind::Unhandled;     break;
        case tesseract::EventType::DaySeparator:  row.kind = Kind::DaySeparator;  break;
        case tesseract::EventType::ReadMarker:    row.kind = Kind::ReadMarker;    break;
        case tesseract::EventType::TimelineStart: row.kind = Kind::TimelineStart; break;
    }

    // Extract the first URL from text messages for preview card display.
    if (row.kind == Kind::Text || row.kind == Kind::Unhandled) {
        if (!row.formatted_body.empty())
            row.first_url = first_url_from_html(row.formatted_body);
        if (row.first_url.empty() && !row.body.empty())
            row.first_url = first_url_from_plain(row.body);
    }

    return row;
}

namespace {

constexpr float kPadX        = tesseract::visual::kSpaceMD;          // 12
constexpr float kPadY        = tesseract::visual::kMsgRowVerticalPad; // 6
constexpr float kAvatarSize  = tesseract::visual::kMsgAvatarSize;    // 32
constexpr float kAvatarGap   = tesseract::visual::kMsgAvatarGap;     // 8
constexpr float kSenderH     = tesseract::visual::kMsgSenderNameHeight; // 16
constexpr float kTimestampH  = tesseract::visual::kMsgTimestampHeight;  // 14
constexpr float kChipPadX    = 10.0f;

// Read-receipt avatar cluster — painted at the bottom-right of each row,
// inside the existing bounds. Discs overlap by (kReceiptSize - kReceiptStride)
// so a busy room's receipts stay narrow. Anything above `kReceiptCap` collapses
// into a small "+N" pill anchored to the left of the cluster.
constexpr float kReceiptSize    = 16.0f;
constexpr float kReceiptStride  = 11.0f;          // 5 px overlap
constexpr std::size_t kReceiptCap = 5;
constexpr float kReceiptOverflowGap = 4.0f;       // gap between "+N" pill and discs

inline float chip_h()      { return static_cast<float>(tesseract::Settings::instance().reaction_chip_height); }
inline float chip_gap()    { return static_cast<float>(tesseract::Settings::instance().reaction_chip_gap); }
inline float chip_radius() { return chip_h() * 0.5f; }
constexpr float kImageMaxW   = tesseract::visual::kMaxInlineImageWidth;  // 320
constexpr float kImageMaxH   = tesseract::visual::kMaxInlineImageHeight; // 200
constexpr float kStickerSize = tesseract::visual::kStickerSize;          // 256
constexpr float kFileCardH   = 56.0f;
constexpr float kFileCardW   = 280.0f;

// URL preview card dimensions.
constexpr float kPreviewCardH      =  72.0f;
constexpr float kPreviewCardW      = 280.0f;
constexpr float kPreviewThumbSide  =  56.0f;
constexpr float kPreviewCardPad    =  10.0f;
constexpr float kPreviewCardGapTop =   6.0f;
constexpr float kFileIconSize = 36.0f;
constexpr float kFileIconPadL = 10.0f;
constexpr float kFileTextOffX = kFileIconPadL + kFileIconSize + 8.0f; // 54px

// MSC3245 voice card. Same width as the file card so the timeline stays
// visually aligned, slightly shorter because there's no second line of
// metadata. Play button is a 32 px circle on the left; remaining width
// is split between the waveform strip and the right-justified duration
// label.
constexpr float kVoiceCardH       = 48.0f;
constexpr float kVoiceCardW       = 280.0f;
constexpr float kVoicePlayBtnSize = 32.0f;
constexpr float kVoiceCardPadX    = 8.0f;
constexpr float kVoiceBarW        = 3.0f;
constexpr float kVoiceBarGap      = 2.0f;
constexpr float kVoiceBarMinH     = 3.0f;   // placeholder bar height
constexpr float kVoiceDurationW   = 40.0f;  // reserved for "0:00" label
constexpr float kVoiceSpeedPillW  = 30.0f;  // "1×" / "1.5×" / "2×"
constexpr float kVoiceSpeedPillH  = 20.0f;

// Reply quote block — painted above the body block when m.has_reply().
constexpr float kQuoteBlockH   = 44.0f;  // total height of the quote band
constexpr float kQuoteAccentW  =  3.0f;  // left accent stripe width
constexpr float kQuotePadX     =  8.0f;
constexpr float kQuoteGapAfter =  4.0f;  // gap between quote and body
// Reply hover button ("↩") — right-aligned in the chip strip.
constexpr float kReplyBtnW     = 28.0f;
constexpr float kReplyBtnPadX  =  8.0f;
// Edit hover button ("✏") — left of the reply button in the chip strip.
constexpr float kEditBtnW      = 28.0f;
// "(edited)" badge — appended after the body text of an edited message.
constexpr float kEditedBadgeGap =  4.0f;
// Reduced top padding for continuation rows (no avatar/sender chrome).
constexpr float kContPadY       =  2.0f;

// Virtual timeline item heights.
constexpr float kDaySepH        = 28.0f;
constexpr float kReadMarkerH    = 20.0f;
constexpr float kTimelineStartH = 20.0f;

std::string format_mmss(std::uint64_t ms) {
    if (ms == 0) return "0:00";
    std::uint64_t total_s = ms / 1000;
    std::uint64_t mm      = total_s / 60;
    std::uint64_t ss      = total_s % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return std::string(buf);
}

std::string format_hhmm(std::uint64_t timestamp_ms) {
    if (timestamp_ms == 0) return {};
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm     local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                   local.tm_hour, local.tm_min);
    return std::string(buf);
}

std::string format_size(std::uint64_t bytes) {
    if (bytes < 1024)              return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)       return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ull * 1024 * 1024)
                                    return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
}

struct FileIconInfo {
    tk::Color   color;
    std::string label;  // uppercase extension, ≤4 chars
};

static FileIconInfo file_icon_info(std::string_view filename) {
    auto dot = filename.rfind('.');
    std::string ext;
    if (dot != std::string_view::npos) {
        ext = std::string(filename.substr(dot + 1));
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::string label = ext.empty() ? "FILE" : ext;
    for (auto& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (label.size() > 4) label.resize(4);

    static const std::unordered_map<std::string, tk::Color> map = {
        // images
        {"png",  tk::Color::rgb(0x22A062)}, {"jpg",  tk::Color::rgb(0x22A062)},
        {"jpeg", tk::Color::rgb(0x22A062)}, {"webp", tk::Color::rgb(0x22A062)},
        {"gif",  tk::Color::rgb(0x22A062)}, {"bmp",  tk::Color::rgb(0x22A062)},
        {"svg",  tk::Color::rgb(0x22A062)}, {"avif", tk::Color::rgb(0x22A062)},
        // documents
        {"pdf",  tk::Color::rgb(0x2B7DD4)}, {"doc",  tk::Color::rgb(0x2B7DD4)},
        {"docx", tk::Color::rgb(0x2B7DD4)}, {"odt",  tk::Color::rgb(0x2B7DD4)},
        {"txt",  tk::Color::rgb(0x2B7DD4)}, {"rtf",  tk::Color::rgb(0x2B7DD4)},
        {"epub", tk::Color::rgb(0x2B7DD4)},
        // spreadsheets
        {"xls",  tk::Color::rgb(0x2E8B4A)}, {"xlsx", tk::Color::rgb(0x2E8B4A)},
        {"ods",  tk::Color::rgb(0x2E8B4A)}, {"csv",  tk::Color::rgb(0x2E8B4A)},
        // presentations
        {"ppt",  tk::Color::rgb(0xC0572B)}, {"pptx", tk::Color::rgb(0xC0572B)},
        {"odp",  tk::Color::rgb(0xC0572B)}, {"key",  tk::Color::rgb(0xC0572B)},
        // archives
        {"zip",  tk::Color::rgb(0xE07B1E)}, {"tar",  tk::Color::rgb(0xE07B1E)},
        {"gz",   tk::Color::rgb(0xE07B1E)}, {"bz2",  tk::Color::rgb(0xE07B1E)},
        {"xz",   tk::Color::rgb(0xE07B1E)}, {"7z",   tk::Color::rgb(0xE07B1E)},
        {"rar",  tk::Color::rgb(0xE07B1E)}, {"zst",  tk::Color::rgb(0xE07B1E)},
        // audio
        {"mp3",  tk::Color::rgb(0x7C54C8)}, {"wav",  tk::Color::rgb(0x7C54C8)},
        {"ogg",  tk::Color::rgb(0x7C54C8)}, {"flac", tk::Color::rgb(0x7C54C8)},
        {"m4a",  tk::Color::rgb(0x7C54C8)}, {"aac",  tk::Color::rgb(0x7C54C8)},
        {"opus", tk::Color::rgb(0x7C54C8)},
        // video
        {"mp4",  tk::Color::rgb(0xD13030)}, {"mkv",  tk::Color::rgb(0xD13030)},
        {"avi",  tk::Color::rgb(0xD13030)}, {"mov",  tk::Color::rgb(0xD13030)},
        {"webm", tk::Color::rgb(0xD13030)}, {"m4v",  tk::Color::rgb(0xD13030)},
        // code
        {"py",   tk::Color::rgb(0x1A8F8A)}, {"js",   tk::Color::rgb(0x1A8F8A)},
        {"ts",   tk::Color::rgb(0x1A8F8A)}, {"cpp",  tk::Color::rgb(0x1A8F8A)},
        {"h",    tk::Color::rgb(0x1A8F8A)}, {"rs",   tk::Color::rgb(0x1A8F8A)},
        {"java", tk::Color::rgb(0x1A8F8A)}, {"json", tk::Color::rgb(0x1A8F8A)},
        {"xml",  tk::Color::rgb(0x1A8F8A)}, {"html", tk::Color::rgb(0x1A8F8A)},
        {"css",  tk::Color::rgb(0x1A8F8A)}, {"go",   tk::Color::rgb(0x1A8F8A)},
        {"sh",   tk::Color::rgb(0x1A8F8A)}, {"rb",   tk::Color::rgb(0x1A8F8A)},
    };
    constexpr tk::Color kGeneric = tk::Color::rgb(0x7A7A8E);
    auto it = map.find(ext);
    return { it != map.end() ? it->second : kGeneric, std::move(label) };
}

float body_text_max_width(float row_width) {
    return std::max(0.0f,
                     row_width - kPadX - kAvatarSize - kAvatarGap - kPadX);
}

std::string format_day_label(std::uint64_t timestamp_ms) {
    if (timestamp_ms == 0) return {};
    std::time_t now_t = std::time(nullptr);
    std::time_t t     = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm now_tm{}, sep_tm{};
#if defined(_WIN32)
    localtime_s(&now_tm, &now_t);
    localtime_s(&sep_tm, &t);
#else
    localtime_r(&now_t, &now_tm);
    localtime_r(&t,     &sep_tm);
#endif
    if (sep_tm.tm_year == now_tm.tm_year &&
        sep_tm.tm_mon  == now_tm.tm_mon  &&
        sep_tm.tm_mday == now_tm.tm_mday)
        return "Today";
    std::time_t yest_t = now_t - 86400;
    std::tm yest_tm{};
#if defined(_WIN32)
    localtime_s(&yest_tm, &yest_t);
#else
    localtime_r(&yest_t, &yest_tm);
#endif
    if (sep_tm.tm_year == yest_tm.tm_year &&
        sep_tm.tm_mon  == yest_tm.tm_mon  &&
        sep_tm.tm_mday == yest_tm.tm_mday)
        return "Yesterday";
    if (now_t > t &&
        static_cast<std::uint64_t>(now_t - t) < 7u * 86400u) {
        constexpr const char* kDays[] = {
            "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
        };
        return kDays[sep_tm.tm_wday];
    }
    constexpr const char* kMonths[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %d, %d",
                  kMonths[sep_tm.tm_mon], sep_tm.tm_mday,
                  sep_tm.tm_year + 1900);
    return buf;
}

} // namespace

class MessageListView::Adapter : public tk::ListAdapter {
public:
    explicit Adapter(MessageListView& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.messages_.size(); }

    // True when `index` is a continuation of the previous row: same
    // sender, not a reply, within the grouping window. Continuation
    // rows suppress the avatar and sender name.
    bool is_cont(std::size_t index) const {
        if (index == 0) return false;
        const auto& curr = owner_.messages_[index];
        // Virtual rows are never continuations; nor is the row after one.
        using Kind = MessageRowData::Kind;
        if (curr.kind == Kind::DaySeparator ||
            curr.kind == Kind::ReadMarker   ||
            curr.kind == Kind::TimelineStart) return false;
        if (curr.has_reply()) return false;
        const auto& prev = owner_.messages_[index - 1];
        if (prev.kind == Kind::DaySeparator ||
            prev.kind == Kind::ReadMarker   ||
            prev.kind == Kind::TimelineStart) return false;
        if (prev.sender != curr.sender) return false;
        int interval_s = tesseract::Settings::instance().message_group_interval_s;
        if (interval_s <= 0) return false;
        if (curr.timestamp_ms < prev.timestamp_ms) return false;
        return (curr.timestamp_ms - prev.timestamp_ms)
                   <= static_cast<std::uint64_t>(interval_s) * 1000;
    }

    float measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                              float width) override {
        if (index >= owner_.messages_.size()) return 0;
        const auto& m = owner_.messages_[index];
        using Kind = MessageRowData::Kind;
        if (m.kind == Kind::DaySeparator)  return kDaySepH;
        if (m.kind == Kind::ReadMarker)    return kReadMarkerH;
        if (m.kind == Kind::TimelineStart) return kTimelineStartH;
        bool  cont    = is_cont(index);
        float body_w  = body_text_max_width(width);
        float body_h  = measure_body_block_height(m, ctx, body_w);
        float chips_h   = !m.reactions.empty() ? chip_h() : 0.0f;
        float receipt_h = !m.read_receipts.empty() && m.reactions.empty()
                              ? kReceiptSize + kPadY : 0.0f;
        float top_pad  = cont ? kContPadY : kPadY;
        float header_h = cont ? 0.0f      : kAvatarSize;
        return top_pad + header_h + body_h + chips_h + receipt_h + kPadY;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool /*selected*/, bool hovered) override {
        if (index >= owner_.messages_.size()) return;
        const auto& m = owner_.messages_[index];

        // Virtual items get their own minimal rendering — no avatar/body layout.
        using Kind = MessageRowData::Kind;
        if (m.kind == Kind::DaySeparator) {
            paint_day_separator(m, ctx, bounds);
            return;
        }
        if (m.kind == Kind::ReadMarker) {
            paint_read_marker(ctx, bounds);
            return;
        }
        if (m.kind == Kind::TimelineStart) {
            paint_timeline_start(ctx, bounds);
            return;
        }

        bool cont = is_cont(index);

        if (hovered) {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.subtle_hover);
            owner_.hovered_row_geom_.row_index      = index;
            owner_.hovered_row_geom_.row_bounds     = bounds;
            owner_.hovered_row_geom_.chips.clear();
            owner_.hovered_row_geom_.receipt_discs.clear();
            owner_.hovered_row_geom_.add_button     = tk::Rect{};
            owner_.hovered_row_geom_.add_visible    = false;
            owner_.hovered_row_geom_.reply_button   = tk::Rect{};
            owner_.hovered_row_geom_.edit_button    = tk::Rect{};
        }

        // Avatar column centre — used both for painting and for the
        // hover timestamp (continuation rows skip the avatar itself).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + kPadY + kAvatarSize * 0.5f;

        // Right-of-avatar column (same indent for cont + non-cont so
        // body text aligns with the row above in a continuation group).
        float col_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float col_w = std::max(0.0f, bounds.x + bounds.w - col_x - kPadX);

        if (!cont) {
            // Avatar disc / initials.
            const tk::Image* avatar = nullptr;
            if (owner_.avatar_provider_ && !m.sender_avatar_url.empty())
                avatar = owner_.avatar_provider_(m.sender_avatar_url);
            if (avatar) {
                ctx.canvas.draw_circle_image(*avatar,
                                              { avatar_cx, avatar_cy },
                                              kAvatarSize);
            } else {
                ctx.canvas.draw_initials_circle(
                    m.sender_name.empty() ? m.sender : m.sender_name,
                    { avatar_cx, avatar_cy },
                    kAvatarSize,
                    ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);
            }

            // Sender name — vertically centered against the avatar disc.
            float sender_y = bounds.y + kPadY + (kAvatarSize - kSenderH) * 0.5f;
            tk::TextStyle s{};
            s.role      = tk::FontRole::SenderName;
            s.trim      = tk::TextTrim::Ellipsis;
            s.max_width = col_w;
            auto layout = ctx.factory.build_text(
                m.sender_name.empty() ? m.sender : m.sender_name, s);
            if (layout)
                ctx.canvas.draw_text(*layout, { col_x, sender_y },
                                      ctx.theme.palette.text_secondary);
        }

        // Body block: below avatar for full rows, tight to top for continuations.
        float cursor = cont ? (bounds.y + kContPadY)
                            : (bounds.y + kPadY + kAvatarSize);
        cursor = paint_body_block(m, ctx, col_x, cursor, col_w);

        // ── Hover-button overlay (no reactions / no receipts) ───────────────
        // When there is nothing to permanently show below the body, the row
        // is compact (chips_h == 0). In that case the +/↩/✏ hover buttons
        // float right-aligned at the avatar-band height so they never push
        // the row taller. Built right-to-left so the cluster stays flush with
        // the right edge regardless of which buttons are present.
        if (hovered && m.reactions.empty()) {
            // Vertical centre: inside the avatar band for full rows, top of
            // body for continuation rows (no avatar band).
            float btn_y = cont
                ? (bounds.y + kContPadY)
                : (bounds.y + kPadY + (kAvatarSize - chip_h()) * 0.5f);
            // Leave room on the right for any read-receipt cluster.
            float btn_right = bounds.x + bounds.w - kPadX;
            if (!m.read_receipts.empty()) {
                const std::size_t n = std::min(m.read_receipts.size(), kReceiptCap);
                float cluster_w = kReceiptSize
                                  + static_cast<float>(n - 1) * kReceiptStride;
                btn_right -= cluster_w + chip_gap();
            }

            auto paint_btn = [&](const char* glyph, tk::Rect& geom_out) {
                tk::TextStyle st{};
                st.role = tk::FontRole::Title;
                auto l = ctx.factory.build_text(glyph, st);
                if (!l) return;
                float w = std::max(l->measure().w + kReplyBtnPadX * 2,
                                    chip_h() + 4.0f);
                tk::Rect pill{ btn_right - w, btn_y, w, chip_h() };
                ctx.canvas.fill_rounded_rect(pill, chip_radius(),
                                              ctx.theme.palette.subtle_hover);
                ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                                                ctx.theme.palette.border, 1.0f);
                ctx.canvas.draw_text(*l,
                    { pill.x + kReplyBtnPadX,
                      pill.y + (pill.h - l->measure().h) * 0.5f },
                    ctx.theme.palette.text_secondary);
                geom_out = pill;
                btn_right -= w + chip_gap();
            };

            // Right-to-left: edit (rightmost), reply, add-reaction.
            if (m.is_own && m.kind == MessageRowData::Kind::Text)
                paint_btn("\xE2\x9C\x8F", owner_.hovered_row_geom_.edit_button); // ✏
            paint_btn("\xE2\x86\xA9", owner_.hovered_row_geom_.reply_button);    // ↩
            {
                // "+" — needs HoverTarget::AddButton tracking.
                tk::TextStyle st{};
                st.role = tk::FontRole::Title;
                auto l = ctx.factory.build_text("+", st);
                if (l) {
                    float w = std::max(l->measure().w + kChipPadX * 2,
                                        chip_h() + 8.0f);
                    tk::Rect pill{ btn_right - w, btn_y, w, chip_h() };
                    bool add_hov = owner_.hover_target_ == HoverTarget::AddButton;
                    ctx.canvas.fill_rounded_rect(pill, chip_radius(),
                        add_hov ? ctx.theme.palette.subtle_pressed
                                : ctx.theme.palette.subtle_hover);
                    ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                        add_hov ? ctx.theme.palette.accent
                                : ctx.theme.palette.border,
                        add_hov ? 1.5f : 1.0f);
                    ctx.canvas.draw_text(*l,
                        { pill.x + kChipPadX,
                          pill.y + (pill.h - l->measure().h) * 0.5f },
                        ctx.theme.palette.text_secondary);
                    owner_.hovered_row_geom_.add_button  = pill;
                    owner_.hovered_row_geom_.add_visible = true;
                }
            }
        }

        // Disc centre Y for receipts. When reactions are present the disc shares
        // their chip strip (overridden inside that block). When only receipts are
        // present they get their own strip immediately below the body block.
        float receipt_disc_cy = (!m.read_receipts.empty() && m.reactions.empty())
            ? cursor + kReceiptSize * 0.5f
            : cursor - kReceiptSize * 0.5f;

        // ── Bottom chip strip (reactions) ────────────────────────────────────
        // Only created when there are persistent reaction chips to show.
        if (!m.reactions.empty()) {
            float chip_y = cursor;
            float chip_x = col_x;
            receipt_disc_cy = chip_y + chip_h() * 0.5f;
            for (std::size_t ri = 0; ri < m.reactions.size(); ++ri) {
                const auto& r = m.reactions[ri];
                constexpr float kChipInnerGap = 4.0f;
                constexpr float kImgPad       = 4.0f;
                const bool is_img = !r.source_json.empty();

                tk::TextStyle cst{};
                cst.role = tk::FontRole::UiSemibold;
                auto count_layout =
                    ctx.factory.build_text(std::to_string(r.count), cst);
                if (!count_layout) {
                    if (hovered) owner_.hovered_row_geom_.chips.push_back({});
                    continue;
                }
                tk::Size csz = count_layout->measure();

                std::unique_ptr<tk::TextLayout> emoji_layout;
                tk::Size esz{};
                float content_w;
                if (is_img) {
                    float img_side = chip_h() - kImgPad * 2;
                    content_w = img_side + kChipInnerGap + csz.w;
                } else {
                    tk::TextStyle est{};
                    est.role = tk::FontRole::Title;
                    emoji_layout = ctx.factory.build_text(r.key, est);
                    if (!emoji_layout) {
                        if (hovered) owner_.hovered_row_geom_.chips.push_back({});
                        continue;
                    }
                    esz = emoji_layout->measure();
                    content_w = esz.w + kChipInnerGap + csz.w;
                }
                float w = std::max(content_w + kChipPadX * 2,
                                    chip_h() + 8.0f);
                tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                bool chip_hovered = hovered
                    && owner_.hover_target_ == HoverTarget::Chip
                    && owner_.hover_chip_idx_ == static_cast<int>(ri);
                tk::Color bg     = r.reacted_by_me ? ctx.theme.palette.chip_bg_me
                                                   : ctx.theme.palette.chip_bg;
                tk::Color border = r.reacted_by_me ? ctx.theme.palette.chip_border_me
                                                   : ctx.theme.palette.chip_border;
                tk::Color text   = r.reacted_by_me ? ctx.theme.palette.chip_text_me
                                                   : ctx.theme.palette.chip_text;
                if (chip_hovered) {
                    border = ctx.theme.palette.accent;
                }
                ctx.canvas.fill_rounded_rect(pill, chip_radius(), bg);
                ctx.canvas.stroke_rounded_rect(pill, chip_radius(), border,
                                                chip_hovered ? 1.5f : 1.0f);

                float left_x  = pill.x + kChipPadX;
                float count_y = pill.y + (pill.h - csz.h) * 0.5f;
                if (is_img) {
                    float img_side = chip_h() - kImgPad * 2;
                    tk::Rect img_dst{ left_x, pill.y + kImgPad, img_side, img_side };
                    const tk::Image* img =
                        owner_.image_provider_ ? owner_.image_provider_(r.source_json) : nullptr;
                    if (img) {
                        ctx.canvas.draw_image(*img, img_dst);
                    } else {
                        ctx.canvas.fill_rect(img_dst, tk::Color{0xCC, 0xCC, 0xCC});
                    }
                    ctx.canvas.draw_text(
                        *count_layout,
                        { left_x + img_side + kChipInnerGap, count_y },
                        text);
                } else {
                    // Centre the emoji by its *ascent* (top of layout box to
                    // baseline), not its full line-height: colour-emoji glyphs
                    // fill the ascent region and leave the descender empty, so
                    // box-centring leaves them visually high in the chip.
                    constexpr float kAscentRatio = 0.78f;
                    float emoji_y = pill.y
                                  + (pill.h - esz.h * kAscentRatio) * 0.5f;
                    ctx.canvas.draw_text(*emoji_layout, { left_x, emoji_y }, text);
                    ctx.canvas.draw_text(
                        *count_layout,
                        { left_x + esz.w + kChipInnerGap, count_y },
                        text);
                }
                if (hovered) owner_.hovered_row_geom_.chips.push_back(pill);
                chip_x += w + chip_gap();
            }

            // Trailing "+" pseudo-chip: only painted while the row is
            // hovered. Reads as a discoverable affordance, not a real
            // reaction — muted background, subtle border.
            if (hovered) {
                tk::TextStyle st{};
                st.role = tk::FontRole::Title;
                auto layout = ctx.factory.build_text("+", st);
                if (layout) {
                    float w = std::max(layout->measure().w + kChipPadX * 2,
                                        chip_h() + 8.0f);
                    tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                    bool add_hovered =
                        owner_.hover_target_ == HoverTarget::AddButton;
                    tk::Color bg = add_hovered
                        ? ctx.theme.palette.subtle_pressed
                        : ctx.theme.palette.subtle_hover;
                    tk::Color border = add_hovered
                        ? ctx.theme.palette.accent
                        : ctx.theme.palette.border;
                    ctx.canvas.fill_rounded_rect(pill, chip_radius(), bg);
                    ctx.canvas.stroke_rounded_rect(pill, chip_radius(), border,
                                                    add_hovered ? 1.5f : 1.0f);
                    ctx.canvas.draw_text(
                        *layout,
                        { pill.x + kChipPadX,
                          pill.y + (pill.h - layout->measure().h) * 0.5f },
                        ctx.theme.palette.text_secondary);
                    owner_.hovered_row_geom_.add_button  = pill;
                    owner_.hovered_row_geom_.add_visible = true;
                    chip_x += w + chip_gap();
                }

                // Reply button "↩" — painted immediately after the "+" chip.
                {
                    tk::TextStyle st{};
                    st.role = tk::FontRole::Title;
                    auto layout = ctx.factory.build_text("↩", st);  // ↩
                    if (layout) {
                        float w = std::max(layout->measure().w + kReplyBtnPadX * 2,
                                            chip_h() + 4.0f);
                        tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                        ctx.canvas.fill_rounded_rect(pill, chip_radius(),
                                                      ctx.theme.palette.subtle_hover);
                        ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                                                        ctx.theme.palette.border, 1.0f);
                        ctx.canvas.draw_text(
                            *layout,
                            { pill.x + kReplyBtnPadX,
                              pill.y + (pill.h - layout->measure().h) * 0.5f },
                            ctx.theme.palette.text_secondary);
                        owner_.hovered_row_geom_.reply_button = pill;
                        chip_x += w + chip_gap();
                    }
                }

                // Edit button "✏" — only for own text messages.
                if (m.is_own && m.kind == MessageRowData::Kind::Text) {
                    tk::TextStyle st{};
                    st.role = tk::FontRole::Title;
                    auto layout = ctx.factory.build_text("\xE2\x9C\x8F", st);  // ✏
                    if (layout) {
                        float w = std::max(layout->measure().w + kReplyBtnPadX * 2,
                                            chip_h() + 4.0f);
                        tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                        ctx.canvas.fill_rounded_rect(pill, chip_radius(),
                                                      ctx.theme.palette.subtle_hover);
                        ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                                                        ctx.theme.palette.border, 1.0f);
                        ctx.canvas.draw_text(
                            *layout,
                            { pill.x + kReplyBtnPadX,
                              pill.y + (pill.h - layout->measure().h) * 0.5f },
                            ctx.theme.palette.text_secondary);
                        owner_.hovered_row_geom_.edit_button = pill;
                    }
                }
            }

        }

        // ── Read-receipt cluster ──────────────────────────────────────────────
        // Painted at the bottom-right of the body block — no extra row height.
        // Discs are centred at the bottom edge of the body so they sit at the
        // same vertical level as the last line of text.
        if (!m.read_receipts.empty()) {
            const std::size_t total    = m.read_receipts.size();
            const std::size_t visible  = std::min(total, kReceiptCap);
            const std::size_t overflow = total - visible;
            const float disc_cy = receipt_disc_cy;

            float right_edge = bounds.x + bounds.w - kPadX;
            for (std::size_t i = 0; i < visible; ++i) {
                // m.read_receipts is oldest-first; paint the last
                // `visible` of them right-to-left so the most-recent
                // receipt sits on top of the stack.
                const auto& rr = m.read_receipts[total - 1 - i];
                float cx = right_edge - kReceiptSize * 0.5f
                            - static_cast<float>(i) * kReceiptStride;
                tk::Point centre{ cx, disc_cy };
                if (hovered) {
                    owner_.hovered_row_geom_.receipt_discs.push_back({
                        centre.x - kReceiptSize * 0.5f,
                        centre.y - kReceiptSize * 0.5f,
                        kReceiptSize, kReceiptSize
                    });
                }
                const tk::Image* img = nullptr;
                if (owner_.avatar_provider_ && !rr.avatar_url.empty()) {
                    img = owner_.avatar_provider_(rr.avatar_url);
                }
                if (img) {
                    ctx.canvas.draw_circle_image(*img, centre, kReceiptSize);
                } else {
                    ctx.canvas.draw_initials_circle(
                        rr.display_name.empty() ? rr.user_id : rr.display_name,
                        centre,
                        kReceiptSize,
                        ctx.theme.palette.avatar_initials_bg,
                        ctx.theme.palette.avatar_initials_text);
                }
            }

            // "+N" overflow pill — anchored just to the left of the
            // leftmost disc in the cluster.
            if (overflow > 0) {
                tk::TextStyle st{};
                st.role = tk::FontRole::UiSemibold;
                auto layout = ctx.factory.build_text(
                    std::string("+") + std::to_string(overflow), st);
                if (layout) {
                    tk::Size sz = layout->measure();
                    float pill_w = sz.w + kChipPadX;
                    float pill_h = kReceiptSize;
                    float cluster_left = right_edge
                        - (kReceiptSize + static_cast<float>(visible - 1) * kReceiptStride);
                    tk::Rect pill{
                        cluster_left - kReceiptOverflowGap - pill_w,
                        disc_cy - pill_h * 0.5f,
                        pill_w,
                        pill_h,
                    };
                    ctx.canvas.fill_rounded_rect(
                        pill, pill_h * 0.5f,
                        ctx.theme.palette.chip_bg);
                    ctx.canvas.draw_text(
                        *layout,
                        { pill.x + (pill_w - sz.w) * 0.5f,
                          pill.y + (pill_h - sz.h) * 0.5f },
                        ctx.theme.palette.text_secondary);
                }
            }
        }

        // Hover-only timestamp, painted under the sender avatar (left
        // column). The column from `y = kPadY + kAvatarSize` downward is
        // empty for every row — body text wraps in the right column — so
        // this can sit hugging the row's bottom padding without colliding
        // with anything else the strip painted.
        if (hovered) {
            std::string ts = format_hhmm(m.timestamp_ms);
            if (!ts.empty()) {
                tk::TextStyle st{};
                st.role = tk::FontRole::Timestamp;
                auto layout = ctx.factory.build_text(ts, st);
                if (layout) {
                    tk::Size sz = layout->measure();
                    float tx = avatar_cx - sz.w * 0.5f;
                    float ty = bounds.y + bounds.h - kPadY - sz.h;
                    ctx.canvas.draw_text(*layout, { tx, ty },
                                          ctx.theme.palette.text_muted);
                }
            }
        }
    }

private:
    // ── Virtual timeline item paint helpers ──────────────────────────────────

    void paint_day_separator(const MessageRowData& m, tk::PaintCtx& ctx,
                              tk::Rect bounds) const {
        std::string label = format_day_label(m.timestamp_ms);
        if (label.empty()) return;
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text(label, st);
        if (!lo) return;
        tk::Size sz = lo->measure();
        constexpr float kLabelPadX = 8.0f;
        float cx      = bounds.x + bounds.w * 0.5f;
        float cy      = bounds.y + kDaySepH * 0.5f;
        float label_l = cx - sz.w * 0.5f - kLabelPadX;
        float label_r = cx + sz.w * 0.5f + kLabelPadX;
        float line_y  = std::round(cy);
        if (label_l > bounds.x + kPadX)
            ctx.canvas.fill_rect(
                { bounds.x + kPadX, line_y, label_l - bounds.x - kPadX, 1.0f },
                ctx.theme.palette.border);
        if (label_r < bounds.x + bounds.w - kPadX)
            ctx.canvas.fill_rect(
                { label_r, line_y, bounds.x + bounds.w - kPadX - label_r, 1.0f },
                ctx.theme.palette.border);
        ctx.canvas.draw_text(*lo,
            { cx - sz.w * 0.5f, cy - sz.h * 0.5f },
            ctx.theme.palette.text_muted);
    }

    void paint_read_marker(tk::PaintCtx& ctx, tk::Rect bounds) const {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text("New messages", st);
        if (!lo) return;
        tk::Size sz = lo->measure();
        constexpr float kLabelPadX = 8.0f;
        float cx  = bounds.x + bounds.w * 0.5f;
        float cy  = bounds.y + kReadMarkerH * 0.5f;
        float lx  = cx - sz.w * 0.5f - kLabelPadX;
        float rx  = cx + sz.w * 0.5f + kLabelPadX;
        float ly  = std::round(cy);
        if (lx > bounds.x + kPadX)
            ctx.canvas.fill_rect(
                { bounds.x + kPadX, ly, lx - bounds.x - kPadX, 1.0f },
                ctx.theme.palette.accent);
        if (rx < bounds.x + bounds.w - kPadX)
            ctx.canvas.fill_rect(
                { rx, ly, bounds.x + bounds.w - kPadX - rx, 1.0f },
                ctx.theme.palette.accent);
        ctx.canvas.draw_text(*lo,
            { cx - sz.w * 0.5f, cy - sz.h * 0.5f },
            ctx.theme.palette.accent);
    }

    void paint_timeline_start(tk::PaintCtx& ctx, tk::Rect bounds) const {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text("Start of conversation", st);
        if (!lo) return;
        tk::Size sz = lo->measure();
        float cx = bounds.x + bounds.w * 0.5f;
        float cy = bounds.y + kTimelineStartH * 0.5f;
        ctx.canvas.draw_text(*lo,
            { cx - sz.w * 0.5f, cy - sz.h * 0.5f },
            ctx.theme.palette.text_muted);
    }

    // ── Message row paint helpers ─────────────────────────────────────────────

    float measure_body_block_height(const MessageRowData& m,
                                     tk::LayoutCtx& ctx,
                                     float col_w) const {
        float quote_h = m.has_reply() ? (kQuoteBlockH + kQuoteGapAfter) : 0.0f;
        switch (m.kind) {
            case MessageRowData::Kind::Text:
            case MessageRowData::Kind::Unhandled: {
                float th = measure_body_text(m, ctx, col_w);
                float badge_h = 0.0f;
                if (m.is_edited) {
                    badge_h = kEditedBadgeGap + measure_text_height("(edited)", ctx, col_w);
                }
                float preview_h = 0.0f;
                if (!m.first_url.empty() && owner_.preview_provider_) {
                    const auto* p = owner_.preview_provider_(m.first_url);
                    if (p && p->has_content())
                        preview_h = kPreviewCardGapTop + kPreviewCardH;
                }
                return quote_h + th + badge_h + preview_h;
            }
            case MessageRowData::Kind::Redacted:
                return quote_h + measure_text_height(m.body.empty()
                    ? std::string("(empty message)")
                    : m.body,
                    ctx, col_w);

            case MessageRowData::Kind::Image: {
                tk::Size sz = fit_media(m.media_w, m.media_h,
                                         std::min(col_w, kImageMaxW),
                                         kImageMaxH);
                float h = sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    h += 4.0f + measure_text_height(m.body, ctx, col_w);
                }
                return quote_h + h;
            }
            case MessageRowData::Kind::Sticker: {
                float side = std::min(col_w, kStickerSize);
                return quote_h + side;
            }
            case MessageRowData::Kind::File:
                return quote_h + kFileCardH;
            case MessageRowData::Kind::Voice:
                return quote_h + kVoiceCardH;
            case MessageRowData::Kind::Video: {
                int vw = m.media_w, vh = m.media_h;
                tk::Size sz = (vw > 0 && vh > 0)
                    ? fit_media(vw, vh, std::min(col_w, kImageMaxW), kImageMaxH)
                    : tk::Size{ std::min(col_w, kImageMaxW),
                                std::min(col_w, kImageMaxW) * 9.0f / 16.0f };
                float h = sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    h += 4.0f + measure_text_height(m.body, ctx, col_w);
                }
                return quote_h + h;
            }
            // Virtual items are handled before this function is called.
            case MessageRowData::Kind::DaySeparator:
            case MessageRowData::Kind::ReadMarker:
            case MessageRowData::Kind::TimelineStart:
                return 0.0f;
        }
        return quote_h;
    }

    float paint_body_block(const MessageRowData& m, tk::PaintCtx& ctx,
                            float x, float y, float col_w) const {
        if (m.has_reply()) {
            y = paint_quote_block(m, ctx, x, y, col_w);
            y += kQuoteGapAfter;
        }
        switch (m.kind) {
            case MessageRowData::Kind::Text:
            case MessageRowData::Kind::Unhandled: {
                float h = paint_body_text(m, ctx, x, y, col_w);
                float end_y = y + h;
                // "(edited)" badge on a new inline line below the body.
                if (m.is_edited) {
                    tk::TextStyle st{};
                    st.role      = tk::FontRole::Small;
                    st.trim      = tk::TextTrim::Ellipsis;
                    st.max_width = col_w;
                    auto lo = ctx.factory.build_text("(edited)", st);
                    if (lo) {
                        ctx.canvas.draw_text(*lo,
                            { x, end_y + kEditedBadgeGap },
                            ctx.theme.palette.text_muted);
                        end_y += kEditedBadgeGap + lo->measure().h;
                    }
                }
                if (!m.first_url.empty() && owner_.preview_provider_) {
                    const auto* p = owner_.preview_provider_(m.first_url);
                    if (p && p->has_content()) {
                        end_y += kPreviewCardGapTop;
                        paint_preview_card_(m, *p, ctx, x, end_y, col_w);
                        end_y += kPreviewCardH;
                    }
                }
                return end_y;
            }
            case MessageRowData::Kind::Redacted: {
                float h = paint_wrapped_text("Message deleted", ctx, x, y,
                                              col_w,
                                              ctx.theme.palette.text_muted);
                return y + h;
            }
            case MessageRowData::Kind::Image: {
                tk::Size sz = fit_media(m.media_w, m.media_h,
                                         std::min(col_w, kImageMaxW),
                                         kImageMaxH);
                tk::Rect r{ x, y, sz.w, sz.h };
                paint_inline_media(m, ctx, r);
                if (!m.event_id.empty()) {
                    owner_.image_geom_[m.event_id] = MessageListView::ImageHit{
                        m.event_id, m.media_url, m.body,
                        m.media_w, m.media_h, r
                    };
                }
                float cursor = y + sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    cursor += 4.0f;
                    float ch = paint_wrapped_text(m.body, ctx, x, cursor,
                                                    col_w,
                                                    ctx.theme.palette.text_primary);
                    cursor += ch;
                }
                return cursor;
            }
            case MessageRowData::Kind::Sticker: {
                float side = std::min(col_w, kStickerSize);
                tk::Rect r{ x, y, side, side };
                paint_inline_media(m, ctx, r);
                if (!m.event_id.empty()) {
                    owner_.sticker_geom_[m.event_id] = MessageListView::StickerHit{
                        m.event_id, m.media_url, m.body, r
                    };
                    owner_.image_geom_[m.event_id] = MessageListView::ImageHit{
                        m.event_id, m.media_url, m.body,
                        static_cast<int>(side), static_cast<int>(side), r
                    };
                }
                return y + side;
            }
            case MessageRowData::Kind::File: {
                float card_w = std::min(kFileCardW, col_w);
                tk::Rect r{ x, y, card_w, kFileCardH };
                paint_file_card(m, ctx, r);
                return y + kFileCardH;
            }
            case MessageRowData::Kind::Voice: {
                float card_w = std::min(kVoiceCardW, col_w);
                tk::Rect r{ x, y, card_w, kVoiceCardH };
                paint_voice_card(m, ctx, r);
                return y + kVoiceCardH;
            }
            case MessageRowData::Kind::Video: {
                int vw = m.media_w, vh = m.media_h;
                tk::Size sz = (vw > 0 && vh > 0)
                    ? fit_media(vw, vh, std::min(col_w, kImageMaxW), kImageMaxH)
                    : tk::Size{ std::min(col_w, kImageMaxW),
                                std::min(col_w, kImageMaxW) * 9.0f / 16.0f };
                tk::Rect r{ x, y, sz.w, sz.h };
                paint_video_card(m, ctx, r);
                if (!m.event_id.empty()) {
                    const std::string& thumb = m.video_thumb_url.empty()
                        ? m.media_url : m.video_thumb_url;
                    owner_.video_geom_[m.event_id] = MessageListView::VideoHit{
                        m.event_id, m.media_url, thumb, m.video_mime,
                        m.media_w, m.media_h, m.duration_ms,
                        m.video_autoplay, m.video_loop, m.video_no_audio,
                        m.video_hide_controls, m.video_gif,
                        r
                    };
                }
                float cursor = y + sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    cursor += 4.0f;
                    float ch = paint_wrapped_text(m.body, ctx, x, cursor,
                                                    col_w,
                                                    ctx.theme.palette.text_primary);
                    cursor += ch;
                }
                return cursor;
            }
            // Virtual items are handled before this function is called.
            case MessageRowData::Kind::DaySeparator:
            case MessageRowData::Kind::ReadMarker:
            case MessageRowData::Kind::TimelineStart:
                break;
        }
        return y;
    }

    // Paint the reply quote block (left accent stripe + sender + snippet).
    // Returns the y-coordinate directly below the block (y + kQuoteBlockH).
    float paint_quote_block(const MessageRowData& m, tk::PaintCtx& ctx,
                             float x, float y, float col_w) const {
        tk::Rect card{ x, y, col_w, kQuoteBlockH };

        // Record world-coord rect so on_pointer_down can hit-test it.
        owner_.quote_block_geom_[m.event_id] = card;

        // Background + border
        ctx.canvas.fill_rounded_rect(card, 4.0f, ctx.theme.palette.subtle_hover);
        ctx.canvas.stroke_rounded_rect(card, 4.0f, ctx.theme.palette.border, 1.0f);

        // Left accent stripe (3 px, full height of the card, rounded left edge)
        tk::Rect stripe{ x, y, kQuoteAccentW, kQuoteBlockH };
        ctx.canvas.fill_rounded_rect(stripe, 4.0f, ctx.theme.palette.accent);

        // Text column starts to the right of the stripe + gap
        float tx   = x + kQuoteAccentW + kQuotePadX;
        float tw   = std::max(0.0f, col_w - kQuoteAccentW - kQuotePadX * 2);

        // Build both text layouts before drawing so we can measure their
        // actual heights and vertically centre the pair within the card.
        const std::string& sname = m.in_reply_to_sender_name.empty()
            ? m.in_reply_to_id : m.in_reply_to_sender_name;

        tk::TextStyle name_st{};
        name_st.role      = tk::FontRole::UiSemibold;
        name_st.trim      = tk::TextTrim::Ellipsis;
        name_st.max_width = tw;
        auto name_lo = sname.empty()
            ? nullptr : ctx.factory.build_text(sname, name_st);

        tk::TextStyle body_st{};
        body_st.role      = tk::FontRole::Body;
        body_st.trim      = tk::TextTrim::Ellipsis;
        body_st.max_width = tw;
        auto body_lo = m.in_reply_to_body.empty()
            ? nullptr : ctx.factory.build_text(m.in_reply_to_body, body_st);

        constexpr float kLineGap = 2.0f;
        float name_h = name_lo ? name_lo->measure().h : 0.0f;
        float body_h = body_lo ? body_lo->measure().h : 0.0f;
        float total_h = name_h + (body_h > 0.0f ? kLineGap + body_h : 0.0f);
        float text_y  = y + (kQuoteBlockH - total_h) * 0.5f;

        if (name_lo)
            ctx.canvas.draw_text(*name_lo, { tx, text_y },
                                  ctx.theme.palette.text_secondary);
        if (body_lo)
            ctx.canvas.draw_text(*body_lo, { tx, text_y + name_h + kLineGap },
                                  ctx.theme.palette.text_muted);

        return y + kQuoteBlockH;
    }

    void paint_preview_card_(const MessageRowData& m,
                              const UrlPreviewData& p,
                              tk::PaintCtx& ctx,
                              float x, float y, float col_w) const {
        float card_w = std::min(col_w, kPreviewCardW);
        tk::Rect card{ x, y, card_w, kPreviewCardH };

        ctx.canvas.fill_rounded_rect  (card, 8.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(card, 8.0f, ctx.theme.palette.border, 1.0f);

        // Record world-coord rect for click-to-open hit-test.
        owner_.preview_card_geom_[m.event_id] = { m.first_url, card };

        float thumb_right = 0.0f;
        if (!p.image_mxc.empty() && owner_.image_provider_) {
            const tk::Image* img = owner_.image_provider_(p.image_mxc);
            float tx = x + kPreviewCardPad;
            float ty = y + (kPreviewCardH - kPreviewThumbSide) * 0.5f;
            tk::Rect thumb{ tx, ty, kPreviewThumbSide, kPreviewThumbSide };
            if (img)
                ctx.canvas.draw_image(*img, thumb);
            else
                ctx.canvas.fill_rounded_rect(thumb, 4.0f,
                                              ctx.theme.palette.border);
            thumb_right = tx + kPreviewThumbSide + kPreviewCardPad;
        } else {
            thumb_right = x + kPreviewCardPad;
        }

        float text_x = thumb_right;
        float text_w = std::max(0.0f, card.x + card.w - text_x - kPreviewCardPad);

        float text_y = y + kPreviewCardPad;

        if (!p.title.empty()) {
            tk::TextStyle st{};
            st.role      = tk::FontRole::UiSemibold;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(p.title, st);
            if (lo) {
                ctx.canvas.draw_text(*lo, { text_x, text_y },
                                      ctx.theme.palette.text_primary);
                text_y += lo->measure().h + 2.0f;
            }
        }
        if (!p.description.empty()) {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(p.description, st);
            if (lo) {
                ctx.canvas.draw_text(*lo, { text_x, text_y },
                                      ctx.theme.palette.text_secondary);
                text_y += lo->measure().h + 2.0f;
            }
        }
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(m.first_url, st);
            if (lo)
                ctx.canvas.draw_text(*lo, { text_x, text_y },
                                      ctx.theme.palette.text_muted);
        }
    }

    static tk::TextStyle body_style(float w) {
        tk::TextStyle s{};
        s.role      = tk::FontRole::Body;
        s.wrap      = true;
        s.max_width = w;
        return s;
    }

    float measure_text_height(const std::string& text, tk::LayoutCtx& ctx,
                                float w) const {
        if (text.empty()) return 0;
        auto layout = ctx.factory.build_text(text, body_style(w));
        return layout ? layout->measure().h : 0;
    }

    // Measure height for a text message body — uses rich text when
    // formatted_body is present, otherwise falls back to plain text.
    float measure_body_text(const MessageRowData& m, tk::LayoutCtx& ctx,
                             float w) const {
        if (!m.formatted_body.empty()) {
            auto spans = html_to_spans(m.formatted_body);
            if (!spans.empty()) {
                auto layout = ctx.factory.build_rich_text(spans, body_style(w));
                if (layout) return layout->measure().h;
            }
        }
        return measure_text_height(
            m.body.empty() ? std::string("(empty message)") : m.body, ctx, w);
    }

    float paint_wrapped_text(const std::string& text, tk::PaintCtx& ctx,
                              float x, float y, float w, tk::Color color) const {
        if (text.empty()) return 0;
        auto layout = ctx.factory.build_text(text, body_style(w));
        if (!layout) return 0;
        ctx.canvas.draw_text(*layout, { x, y }, color);
        return layout->measure().h;
    }

    // Paint a text message body — uses rich text when formatted_body is
    // present, otherwise falls back to plain text.
    float paint_body_text(const MessageRowData& m, tk::PaintCtx& ctx,
                           float x, float y, float w) const {
        if (!m.formatted_body.empty()) {
            auto spans = html_to_spans(m.formatted_body);
            if (!spans.empty()) {
                auto layout = ctx.factory.build_rich_text(spans, body_style(w));
                if (layout) {
                    ctx.canvas.draw_text(*layout, { x, y },
                                          ctx.theme.palette.text_primary);
                    return layout->measure().h;
                }
            }
        }
        return paint_wrapped_text(
            m.body.empty() ? std::string("(empty message)") : m.body,
            ctx, x, y, w, ctx.theme.palette.text_primary);
    }

    void paint_inline_media(const MessageRowData& m, tk::PaintCtx& ctx,
                             tk::Rect dst) const {
        const tk::Image* img = nullptr;
        if (owner_.image_provider_ && !m.media_url.empty()) {
            img = owner_.image_provider_(m.media_url);
        }
        if (img) {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*img, dst);
            ctx.canvas.pop_clip();
        } else {
            // Placeholder while bytes are still downloading.
            ctx.canvas.fill_rounded_rect(dst, 8.0f,
                                          ctx.theme.palette.chrome_bg);
            ctx.canvas.stroke_rounded_rect(dst, 8.0f,
                                            ctx.theme.palette.border, 1.0f);
        }
    }

    void paint_file_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const {
        ctx.canvas.fill_rounded_rect(dst, 8.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 8.0f, ctx.theme.palette.border, 1.0f);

        const std::string name = m.file_name.empty() ? m.body : m.file_name;
        const auto icon = file_icon_info(name);

        // Coloured icon box, vertically centred in the card.
        const float icon_y = dst.y + (dst.h - kFileIconSize) * 0.5f;
        const tk::Rect icon_rect{ dst.x + kFileIconPadL, icon_y,
                                   kFileIconSize, kFileIconSize };
        ctx.canvas.fill_rounded_rect(icon_rect, 6.0f, icon.color);

        // Extension label centred inside the icon box.
        tk::TextStyle ls{};
        ls.role      = tk::FontRole::UiSemibold;
        ls.halign    = tk::TextHAlign::Center;
        ls.max_width = kFileIconSize;
        auto label_lo = ctx.factory.build_text(icon.label, ls);
        if (label_lo) {
            // UiSemibold ~11 pt; approximate cap-height ~13 px at 96 dpi.
            const float label_y = icon_y + (kFileIconSize - 13.0f) * 0.5f;
            ctx.canvas.draw_text(*label_lo, { icon_rect.x, label_y },
                                  tk::Color{ 255, 255, 255, 255 });
        }

        // Filename + size shifted right of the icon box.
        const float text_x = dst.x + kFileTextOffX;
        const float text_w = dst.w - kFileTextOffX - 8.0f;

        tk::TextStyle ns{}; ns.role = tk::FontRole::UiSemibold;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(name, ns);

        tk::TextStyle ss{}; ss.role = tk::FontRole::Timestamp;
        auto size_lo = ctx.factory.build_text(format_size(m.file_size), ss);

        if (name_lo)
            ctx.canvas.draw_text(*name_lo, { text_x, dst.y + 10.0f },
                                  ctx.theme.palette.text_primary);
        if (size_lo)
            ctx.canvas.draw_text(*size_lo, { text_x, dst.y + 30.0f },
                                  ctx.theme.palette.text_secondary);
    }

    void paint_voice_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const {
        // Card chrome.
        ctx.canvas.fill_rounded_rect(dst, 10.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 10.0f, ctx.theme.palette.border, 1.0f);

        // Play / pause button (circle on the left).
        const float btn_d = kVoicePlayBtnSize;
        const float btn_x = dst.x + kVoiceCardPadX;
        const float btn_y = dst.y + (dst.h - btn_d) * 0.5f;
        tk::Rect btn_rect{ btn_x, btn_y, btn_d, btn_d };
        ctx.canvas.fill_rounded_rect(btn_rect, btn_d * 0.5f,
                                     ctx.theme.palette.accent);

        const bool is_active_row =
            m.event_id == owner_.playing_event_id_ && owner_.playing_is_active_;

        const tk::Color glyph_col = ctx.theme.palette.text_on_accent;
        if (is_active_row) {
            // Two pause bars centred in the button.
            const float bar_w = 4.0f;
            const float bar_h = btn_d * 0.45f;
            const float gap   = 4.0f;
            const float cy    = btn_y + (btn_d - bar_h) * 0.5f;
            const float cx    = btn_x + btn_d * 0.5f;
            ctx.canvas.fill_rect({ cx - gap * 0.5f - bar_w, cy, bar_w, bar_h }, glyph_col);
            ctx.canvas.fill_rect({ cx + gap * 0.5f,         cy, bar_w, bar_h }, glyph_col);
        } else {
            // Play triangle (▶): stacked horizontal rects, symmetric about
            // the vertical centre so it actually points right.  Row widths
            // are maximum at the midpoint and taper to near-zero at the top
            // and bottom, forming the two slanted edges of the triangle.
            // The horizontal left edge (flat face) is aligned to tri_x;
            // tri_x is shifted so the visual centroid (1/3 from the base)
            // sits at the button centre.
            const float tri_h = btn_d * 0.50f;
            const float tri_w = btn_d * 0.38f;
            const float tri_x = btn_x + btn_d * 0.5f - tri_w / 3.0f;
            const float tri_y = btn_y + (btn_d - tri_h) * 0.5f;
            const int   steps = 8;
            for (int i = 0; i < steps; ++i) {
                const float t     = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float row_h = tri_h / static_cast<float>(steps);
                const float row_w = tri_w * (1.0f - 2.0f * std::abs(t - 0.5f));
                const float ry    = tri_y + i * row_h;
                ctx.canvas.fill_rect({ tri_x, ry, std::max(1.0f, row_w), row_h }, glyph_col);
            }
        }

        // Right-justified duration label. When this row is the active one
        // and the backend reports a non-zero position, show remaining time
        // instead of total — matches Element / FluffyChat affordances.
        std::uint64_t total =
            m.duration_ms > 0
                ? m.duration_ms
                : (m.event_id == owner_.playing_event_id_
                   ? owner_.audio_player_
                     ? owner_.audio_player_->duration_ms()
                     : 0u
                   : 0u);
        std::uint64_t label_ms = total;
        if (is_active_row && total > 0 && owner_.playing_position_ms_ <= total) {
            label_ms = total - owner_.playing_position_ms_;
        }
        tk::TextStyle ds{}; ds.role = tk::FontRole::Timestamp;
        auto dur_lo = ctx.factory.build_text(format_mmss(label_ms), ds);
        float dur_w = dur_lo ? dur_lo->measure().w : 0.0f;
        float dur_h = dur_lo ? dur_lo->measure().h : 0.0f;
        if (dur_lo) {
            ctx.canvas.draw_text(*dur_lo,
                { dst.x + dst.w - kVoiceCardPadX - dur_w,
                  dst.y + (dst.h - dur_h) * 0.5f },
                ctx.theme.palette.text_secondary);
        }

        // Speed pill — sits just to the left of the duration label. Only
        // the active row's pill is interactive; the rate is global, so
        // rendering it on every row would be a lie about scope.
        tk::Rect pill_rect{};
        if (owner_.audio_player_ && is_active_row) {
            const float pill_x = dst.x + dst.w - kVoiceCardPadX - dur_w
                                 - 6.0f - kVoiceSpeedPillW;
            const float pill_y = dst.y + (dst.h - kVoiceSpeedPillH) * 0.5f;
            pill_rect = { pill_x, pill_y, kVoiceSpeedPillW, kVoiceSpeedPillH };
            ctx.canvas.fill_rounded_rect(pill_rect, kVoiceSpeedPillH * 0.5f,
                                         ctx.theme.palette.subtle_hover);

            char rate_buf[8];
            const float r = owner_.playback_rate_;
            if (r >= 1.99f)      std::snprintf(rate_buf, sizeof(rate_buf), "2×");
            else if (r >= 1.49f) std::snprintf(rate_buf, sizeof(rate_buf), "1.5×");
            else                 std::snprintf(rate_buf, sizeof(rate_buf), "1×");
            tk::TextStyle rs{}; rs.role = tk::FontRole::Timestamp;
            auto rate_lo = ctx.factory.build_text(rate_buf, rs);
            if (rate_lo) {
                tk::Size rsz = rate_lo->measure();
                ctx.canvas.draw_text(*rate_lo,
                    { pill_rect.x + (pill_rect.w - rsz.w) * 0.5f,
                      pill_rect.y + (pill_rect.h - rsz.h) * 0.5f },
                    ctx.theme.palette.text_secondary);
            }
        }

        // Waveform strip. Sits between the play button and the duration
        // label (squeezed further when the speed pill is showing). Bars
        // to the left of the cursor render in the accent colour; bars to
        // the right stay muted. When the sender omitted the MSC1767
        // waveform, we paint a flat row of minimum-height bars so the
        // card still has visual rhythm.
        const float strip_x = btn_x + btn_d + kVoiceCardPadX;
        const float right_anchor = (pill_rect.w > 0.0f)
            ? pill_rect.x - 6.0f
            : dst.x + dst.w - kVoiceCardPadX - kVoiceDurationW;
        const float strip_w_avail = right_anchor - strip_x;
        if (strip_w_avail < kVoiceBarW) return;
        const float strip_y = dst.y + dst.h * 0.5f;
        const float strip_h = dst.h - 16.0f;

        const float step  = kVoiceBarW + kVoiceBarGap;
        const int   bars  = std::max(1, static_cast<int>(strip_w_avail / step));

        // Resample sender waveform → `bars` buckets. When empty, the loop
        // below uses the placeholder height.
        auto amp_at = [&](int i) -> float {
            if (m.waveform.empty()) return 0.0f;
            const std::size_t n = m.waveform.size();
            const std::size_t src = std::min<std::size_t>(
                n - 1,
                static_cast<std::size_t>(static_cast<double>(i) / bars
                                         * static_cast<double>(n)));
            return std::min(1.0f, static_cast<float>(m.waveform[src]) / 1024.0f);
        };

        float cursor_frac = 0.0f;
        if (is_active_row && total > 0) {
            cursor_frac = static_cast<float>(owner_.playing_position_ms_) /
                          static_cast<float>(total);
            if (cursor_frac > 1.0f) cursor_frac = 1.0f;
        }
        const int cursor_bar = static_cast<int>(cursor_frac * bars);

        for (int i = 0; i < bars; ++i) {
            float a = amp_at(i);
            float bar_h = m.waveform.empty()
                           ? kVoiceBarMinH
                           : std::max(kVoiceBarMinH, a * strip_h);
            float bx = strip_x + i * step;
            float by = strip_y - bar_h * 0.5f;
            tk::Color c = (i < cursor_bar)
                            ? ctx.theme.palette.accent
                            : ctx.theme.palette.text_muted;
            ctx.canvas.fill_rounded_rect(
                { bx, by, kVoiceBarW, bar_h }, kVoiceBarW * 0.5f, c);
        }

        // Record world-coord geometry so on_pointer_down can hit-test
        // the play button, waveform strip, and speed pill without
        // re-running the layout maths.
        if (!m.event_id.empty()) {
            tk::Rect strip_rect{
                strip_x,
                strip_y - strip_h * 0.5f,
                strip_w_avail,
                strip_h,
            };
            owner_.voice_card_geom_[m.event_id] =
                MessageListView::VoiceCardGeom{
                    m.event_id, btn_rect, strip_rect, pill_rect, dst };
        }
    }

    void paint_video_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const {
        // Live inline player frame takes priority over the static thumbnail.
        const tk::Image* live_frame = nullptr;
        {
            auto it = owner_.inline_players_.find(m.event_id);
            if (it != owner_.inline_players_.end() && it->second.player)
                live_frame = it->second.player->current_frame();
        }

        // Thumbnail fallback (or placeholder when neither is available).
        const std::string& thumb_key = m.video_thumb_url.empty()
            ? m.media_url : m.video_thumb_url;
        const tk::Image* thumb = nullptr;
        if (!live_frame && owner_.image_provider_ && !thumb_key.empty())
            thumb = owner_.image_provider_(thumb_key);

        if (live_frame) {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*live_frame, dst);
            ctx.canvas.pop_clip();
        } else if (thumb) {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*thumb, dst);
            ctx.canvas.pop_clip();
        } else {
            ctx.canvas.fill_rounded_rect(dst, 8.0f, ctx.theme.palette.chrome_bg);
            ctx.canvas.stroke_rounded_rect(dst, 8.0f, ctx.theme.palette.border, 1.0f);
        }

        // Autoplay / GIF-mode clips play immediately — no play button needed.
        const bool animated = m.video_gif || m.video_autoplay;

        // Centred play disc — omitted for animated clips.
        if (!animated) {
            constexpr float kDiscD = 48.0f;
            const float disc_cx = dst.x + dst.w * 0.5f;
            const float disc_cy = dst.y + dst.h * 0.5f;
            tk::Rect disc{ disc_cx - kDiscD * 0.5f, disc_cy - kDiscD * 0.5f,
                           kDiscD, kDiscD };
            ctx.canvas.fill_rounded_rect(disc, kDiscD * 0.5f,
                                         tk::Color{ 0, 0, 0, 120 });

            // Play triangle (▶): same symmetric stacked-rect approach as
            // the voice card — centroid-shifted so the glyph is centred.
            const float tri_h = kDiscD * 0.45f;
            const float tri_w = kDiscD * 0.35f;
            const float tri_x = disc.x + kDiscD * 0.5f - tri_w / 3.0f;
            const float tri_y = disc.y + (kDiscD - tri_h) * 0.5f;
            constexpr int steps = 8;
            for (int i = 0; i < steps; ++i) {
                const float t     = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float row_h = tri_h / static_cast<float>(steps);
                const float row_w = tri_w * (1.0f - 2.0f * std::abs(t - 0.5f));
                ctx.canvas.fill_rect({ tri_x, tri_y + i * row_h,
                                       std::max(1.0f, row_w), row_h },
                                     tk::Color{ 255, 255, 255, 230 });
            }
        }

        // Duration badge at bottom-right — omitted for animated or hide_controls clips.
        if (!animated && !m.video_hide_controls && m.duration_ms > 0) {
            std::string label = format_mmss(m.duration_ms);
            tk::TextStyle ts{}; ts.role = tk::FontRole::Timestamp;
            auto lo = ctx.factory.build_text(label, ts);
            if (lo) {
                tk::Size lsz = lo->measure();
                constexpr float kBadgePadX = 6.0f, kBadgePadY = 3.0f;
                float bx = dst.x + dst.w - lsz.w - kBadgePadX * 2 - 4.0f;
                float by = dst.y + dst.h - lsz.h - kBadgePadY * 2 - 4.0f;
                tk::Rect badge{ bx, by,
                                lsz.w + kBadgePadX * 2,
                                lsz.h + kBadgePadY * 2 };
                ctx.canvas.fill_rounded_rect(badge, 4.0f,
                                             tk::Color{ 0, 0, 0, 140 });
                ctx.canvas.draw_text(*lo, { bx + kBadgePadX, by + kBadgePadY },
                                     tk::Color{ 255, 255, 255, 230 });
            }
        }
    }

    MessageListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

MessageListView::~MessageListView() = default;

MessageListView::MessageListView()
    : adapter_(std::make_unique<Adapter>(*this)) {
    set_adapter(adapter_.get());
    on_row_clicked = [this](int idx) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= messages_.size()) return;
        if (on_message_clicked) on_message_clicked(messages_[idx].event_id);
    };
}

std::optional<MessageListView::StickerHit>
MessageListView::sticker_hit_at(tk::Point world) const {
    // Sticker geometry is recorded by paint_row in world coordinates each
    // paint pass. We linearly search — a sticker viewport rarely exceeds
    // a handful of visible rows so a hash lookup by point isn't worth it.
    for (const auto& [event_id, hit] : sticker_geom_) {
        if (world.x >= hit.world_rect.x &&
            world.y >= hit.world_rect.y &&
            world.x <  hit.world_rect.x + hit.world_rect.w &&
            world.y <  hit.world_rect.y + hit.world_rect.h) {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<MessageListView::ImageHit>
MessageListView::image_hit_at(tk::Point world) const {
    for (const auto& [eid, hit] : image_geom_) {
        if (world.x >= hit.world_rect.x &&
            world.y >= hit.world_rect.y &&
            world.x <  hit.world_rect.x + hit.world_rect.w &&
            world.y <  hit.world_rect.y + hit.world_rect.h) {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<MessageListView::VideoHit>
MessageListView::video_hit_at(tk::Point world) const {
    for (const auto& [eid, hit] : video_geom_) {
        if (world.x >= hit.world_rect.x &&
            world.y >= hit.world_rect.y &&
            world.x <  hit.world_rect.x + hit.world_rect.w &&
            world.y <  hit.world_rect.y + hit.world_rect.h) {
            return hit;
        }
    }
    return std::nullopt;
}

void MessageListView::set_messages(std::vector<MessageRowData> msgs) {
    inline_players_.clear();
    messages_ = std::move(msgs);
    invalidate_data();
    scroll_to_bottom();
    // Start inline players for animated video rows near the bottom (most
    // recently received), up to the cap.
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (static_cast<int>(inline_players_.size()) >= kMaxInlinePlayers) break;
        if (it->kind == MessageRowData::Kind::Video &&
            (it->video_autoplay || it->video_gif))
            start_inline_video(*it);
    }
}

void MessageListView::insert_message(std::size_t index, MessageRowData msg) {
    if (index > messages_.size()) index = messages_.size();

    const bool animated = msg.kind == MessageRowData::Kind::Video &&
                          (msg.video_autoplay || msg.video_gif);

    // Insertion at (or past) the end is an append: follow the live tail
    // when the user is already pinned there.
    if (index == messages_.size()) {
        bool at_bottom =
            scroll_y() + bounds().h + 1.0f >= content_height();
        if (animated) start_inline_video(msg);
        messages_.push_back(std::move(msg));
        invalidate_data();
        if (at_bottom) scroll_to_bottom();
        return;
    }

    // Insertion above (or at) the viewport's first visible row: anchor
    // the existing rows so the user's visual position stays put. For
    // mid-viewport inserts the same preserve-top math is a benign no-op
    // (the row the user is looking at stays under their cursor because
    // its row offset shifts by the new row's height, which is what
    // preserve_top_through compensates for).
    if (animated) start_inline_video(msg);
    preserve_top_through([&]{
        messages_.insert(messages_.begin() + index, std::move(msg));
        invalidate_data();
    });
}

void MessageListView::update_message(std::size_t index, MessageRowData msg) {
    if (index >= messages_.size()) return;
    const std::string& old_eid = messages_[index].event_id;
    const bool was_animated = messages_[index].kind == MessageRowData::Kind::Video &&
                              (messages_[index].video_autoplay || messages_[index].video_gif);
    const bool now_animated = msg.kind == MessageRowData::Kind::Video &&
                              (msg.video_autoplay || msg.video_gif);
    // Clean up player if no longer animated (e.g. redaction).
    if (was_animated && !now_animated)
        inline_players_.erase(old_eid);
    // Start a new player if newly animated (rare: fi.mau.* edit).
    if (now_animated && !inline_players_.count(msg.event_id))
        start_inline_video(msg);
    messages_[index] = std::move(msg);
    invalidate_data();
}

void MessageListView::remove_message(std::size_t index) {
    if (index >= messages_.size()) return;
    inline_players_.erase(messages_[index].event_id);
    preserve_top_through([&]{
        messages_.erase(messages_.begin() + index);
        invalidate_data();
    });
}

void MessageListView::append_message(MessageRowData msg) {
    insert_message(messages_.size(), std::move(msg));
}

void MessageListView::set_avatar_provider(ImageProvider p) {
    avatar_provider_ = std::move(p);
}

void MessageListView::set_image_provider(ImageProvider p) {
    image_provider_ = std::move(p);
}

void MessageListView::set_preview_provider(PreviewProvider p) {
    preview_provider_ = std::move(p);
}

void MessageListView::set_audio_player(std::unique_ptr<tk::AudioPlayer> player) {
    audio_player_ = std::move(player);
    if (audio_player_) {
        audio_player_->on_progress = [this]() { on_audio_progress(); };
    }
}

void MessageListView::set_voice_bytes_provider(VoiceBytesProvider provider) {
    voice_bytes_provider_ = std::move(provider);
}

void MessageListView::set_repaint_requester(std::function<void()> request_repaint) {
    request_repaint_ = std::move(request_repaint);
}

void MessageListView::set_video_player_factory(VideoPlayerFactory f) {
    video_player_factory_ = std::move(f);
}

void MessageListView::set_video_fetch_provider(VideoFetchProvider f) {
    video_fetch_provider_ = std::move(f);
}

void MessageListView::start_inline_video(const MessageRowData& m) {
    if (!video_player_factory_ || !video_fetch_provider_) return;
    if (inline_players_.count(m.event_id)) return;
    if (static_cast<int>(inline_players_.size()) >= kMaxInlinePlayers) return;

    auto player = video_player_factory_();
    if (!player) return;
    player->set_loop(m.video_loop);
    player->set_muted(m.video_no_audio);
    player->on_frame = [this]{ if (request_repaint_) request_repaint_(); };
    inline_players_[m.event_id] = { std::move(player) };

    const std::string eid     = m.event_id;
    const std::string src     = m.media_url;
    const std::string mime    = m.video_mime;
    const bool        autoplay = m.video_autoplay;

    video_fetch_provider_(src,
        [this, eid, mime, autoplay](std::vector<std::uint8_t> bytes) {
            auto it = inline_players_.find(eid);
            if (it == inline_players_.end() || !it->second.player) return;
            if (bytes.empty()) { inline_players_.erase(eid); return; }
            auto& p = *it->second.player;
            p.play(bytes.data(), bytes.size(), mime);
            if (!autoplay) p.pause();
        });
}

void MessageListView::on_pointer_drag(tk::Point local) {
    if (press_voice_kind_ != VoicePressKind::Waveform) {
        tk::ListView::on_pointer_drag(local);
        return;
    }
    if (press_voice_event_id_.empty()) return;
    tk::Point world{ local.x + bounds().x, local.y + bounds().y };
    for (const auto& row : messages_) {
        if (row.event_id == press_voice_event_id_ &&
            row.kind == MessageRowData::Kind::Voice) {
            handle_voice_scrub_at(row, world.x);
            break;
        }
    }
}

void MessageListView::handle_voice_scrub_at(const MessageRowData& row,
                                            float world_x) {
    if (!audio_player_ || !voice_bytes_provider_) return;
    auto it = voice_card_geom_.find(row.event_id);
    if (it == voice_card_geom_.end()) return;
    const tk::Rect& strip = it->second.waveform_strip;
    if (strip.w <= 0.0f) return;

    float frac = (world_x - strip.x) / strip.w;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // Resolve total duration: the sender's metadata if present, otherwise
    // whatever the backend has discovered after loading.
    std::uint64_t total = row.duration_ms > 0 ? row.duration_ms : 0;
    if (total == 0 && row.event_id == playing_event_id_) {
        total = audio_player_->duration_ms();
    }
    if (total == 0) return;

    const std::uint64_t target_ms =
        static_cast<std::uint64_t>(frac * static_cast<float>(total));

    if (row.event_id != playing_event_id_) {
        // Start playback at the clicked position. Same byte-cache path
        // as a regular play click; on cache miss the view stays idle and
        // the user can try again once the prefetch lands.
        std::vector<std::uint8_t> bytes =
            voice_bytes_provider_(row.audio_source);
        if (bytes.empty()) {
            if (request_repaint_) request_repaint_();
            return;
        }
        if (!playing_event_id_.empty()) audio_player_->stop();
        playing_event_id_    = row.event_id;
        playing_position_ms_ = target_ms;
        playing_is_active_   = true;
        audio_player_->set_playback_rate(playback_rate_);
        audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
        audio_player_->seek(target_ms);
        on_audio_progress();
        return;
    }

    audio_player_->seek(target_ms);
    playing_position_ms_ = target_ms;
    if (request_repaint_) request_repaint_();
}

void MessageListView::handle_voice_speed_click() {
    if (!audio_player_) return;
    if (playback_rate_ < 1.49f)      playback_rate_ = 1.5f;
    else if (playback_rate_ < 1.99f) playback_rate_ = 2.0f;
    else                              playback_rate_ = 1.0f;
    audio_player_->set_playback_rate(playback_rate_);
    if (request_repaint_) request_repaint_();
}

void MessageListView::on_audio_progress() {
    if (audio_player_) {
        playing_position_ms_ = audio_player_->position_ms();
        playing_is_active_   = audio_player_->is_playing();
        if (!playing_is_active_ && playing_position_ms_ == 0) {
            // Treat the post-stop tick as "clip ended"; let the row fall
            // back to its idle play-button state.
            playing_event_id_.clear();
        }
    }
    if (request_repaint_) request_repaint_();
}

void MessageListView::handle_voice_play_click(const MessageRowData& row) {
    if (!audio_player_ || !voice_bytes_provider_) return;

    // Clicking the currently-active row's play button toggles pause.
    if (row.event_id == playing_event_id_) {
        if (audio_player_->is_playing()) audio_player_->pause();
        else                              audio_player_->resume();
        on_audio_progress();
        return;
    }

    // Switching rows — stop the current clip cleanly first.
    if (!playing_event_id_.empty()) {
        audio_player_->stop();
    }

    std::vector<std::uint8_t> bytes = voice_bytes_provider_(row.audio_source);
    if (bytes.empty()) {
        // Cache miss — the SDK kicks off a background fetch on the first
        // call. Surface state honestly: nothing is loaded, repaint so the
        // pause glyph (if any) reverts to play, and let the user click
        // again once bytes arrive.
        playing_event_id_.clear();
        playing_is_active_   = false;
        playing_position_ms_ = 0;
        if (request_repaint_) request_repaint_();
        return;
    }
    playing_event_id_    = row.event_id;
    playing_position_ms_ = 0;
    playing_is_active_   = true;
    audio_player_->set_playback_rate(playback_rate_);
    audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
    on_audio_progress();
}

// Resolve which chip (if any) is under a widget-local point. `local`
// is in widget-local coordinates (relative to MessageListView::bounds_);
// the cached geometry is in world coordinates, so we add the widget
// origin back before comparing.
static MessageListView::HoverTarget chip_hit_at(
        const MessageListView::RowChipGeom& g,
        tk::Rect widget_bounds,
        tk::Point local,
        int& out_chip_idx) {
    out_chip_idx = -1;
    if (g.row_index == static_cast<std::size_t>(-1)) {
        return MessageListView::HoverTarget::None;
    }
    tk::Point world{ local.x + widget_bounds.x,
                       local.y + widget_bounds.y };
    if (!rect_contains(g.row_bounds, world)) {
        return MessageListView::HoverTarget::None;
    }
    for (std::size_t i = 0; i < g.chips.size(); ++i) {
        if (g.chips[i].w <= 0) continue;
        if (rect_contains(g.chips[i], world)) {
            out_chip_idx = static_cast<int>(i);
            return MessageListView::HoverTarget::Chip;
        }
    }
    if (g.add_visible && rect_contains(g.add_button, world)) {
        return MessageListView::HoverTarget::AddButton;
    }
    for (std::size_t i = 0; i < g.receipt_discs.size(); ++i) {
        if (rect_contains(g.receipt_discs[i], world)) {
            out_chip_idx = static_cast<int>(i);
            return MessageListView::HoverTarget::Receipt;
        }
    }
    return MessageListView::HoverTarget::None;
}

void MessageListView::on_pointer_move(tk::Point local) {
    tk::ListView::on_pointer_move(local);
    // Row hover may have changed; if the new hovered row is different
    // from the one we have geometry for, invalidate so paint_row will
    // rebuild it. (Paint will populate hovered_row_geom_ on its next
    // frame; until then chip hit-tests will return None.)
    int row = hovered_row_index();
    if (row < 0 ||
        static_cast<std::size_t>(row) != hovered_row_geom_.row_index) {
        hovered_row_geom_.row_index   = static_cast<std::size_t>(-1);
        hovered_row_geom_.chips.clear();
        hovered_row_geom_.receipt_discs.clear();
        hovered_row_geom_.add_visible = false;
    }
    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(),
                                 local, chip_idx);
    if (t != hover_target_ || chip_idx != hover_chip_idx_) {
        hover_target_   = t;
        hover_chip_idx_ = chip_idx;
    }
}

void MessageListView::on_pointer_leave() {
    tk::ListView::on_pointer_leave();
    hovered_row_geom_.row_index   = static_cast<std::size_t>(-1);
    hovered_row_geom_.chips.clear();
    hovered_row_geom_.receipt_discs.clear();
    hovered_row_geom_.add_visible = false;
    hover_target_   = HoverTarget::None;
    hover_chip_idx_ = -1;
    press_pill_     = false;
}

bool MessageListView::should_show_pill() const {
    if (content_height() <= bounds().h) return false;
    return scroll_y() + bounds().h + 1.0f < content_height();
}

bool MessageListView::on_pointer_down(tk::Point local) {
    if (pill_visible_) {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        if (rect_contains(pill_rect_, world)) {
            press_pill_ = true;
            return true;
        }
    }

    // Voice card hit-test — handled before chips because the voice
    // controls sit in the body block, separate from the trailing chips
    // strip. Check pill (small) then play button then waveform strip.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        for (const auto& [event_id, geom] : voice_card_geom_) {
            if (geom.speed_pill.w > 0 && rect_contains(geom.speed_pill, world)) {
                press_voice_event_id_ = event_id;
                press_voice_kind_     = VoicePressKind::SpeedPill;
                return true;
            }
            if (rect_contains(geom.play_button, world)) {
                press_voice_event_id_ = event_id;
                press_voice_kind_     = VoicePressKind::PlayButton;
                return true;
            }
            if (rect_contains(geom.waveform_strip, world)) {
                press_voice_event_id_ = event_id;
                press_voice_kind_     = VoicePressKind::Waveform;
                // Immediate seek on press for snappy scrub-start, then
                // subsequent on_pointer_drag callbacks follow the finger.
                for (const auto& row : messages_) {
                    if (row.event_id == event_id &&
                        row.kind == MessageRowData::Kind::Voice) {
                        handle_voice_scrub_at(row, world.x);
                        break;
                    }
                }
                return true;
            }
        }
    }

    // Reply button hit-test — check before reaction chips so it has priority.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        const tk::Rect& rb = hovered_row_geom_.reply_button;
        if (rb.w > 0 && rect_contains(rb, world)) {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size()) {
                press_reply_btn_      = true;
                press_reply_event_id_ = messages_[row].event_id;
                return true;
            }
        }
    }

    // Edit button hit-test.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        const tk::Rect& eb = hovered_row_geom_.edit_button;
        if (eb.w > 0 && rect_contains(eb, world)) {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size()) {
                press_edit_btn_      = true;
                press_edit_event_id_ = messages_[row].event_id;
                return true;
            }
        }
    }

    // Quote-block hit-test — lets the user jump to the original message.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        for (const auto& [eid, rect] : quote_block_geom_) {
            if (rect_contains(rect, world)) {
                press_quote_          = true;
                press_quote_event_id_ = eid;
                return true;
            }
        }
    }

    // URL preview card hit-test.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        for (const auto& [eid, hit] : preview_card_geom_) {
            if (rect_contains(hit.rect, world)) {
                press_preview_     = true;
                press_preview_url_ = hit.url;
                return true;
            }
        }
    }

    // Video thumbnail click-to-view hit-test (before image so it wins when
    // video_geom_ and image_geom_ happen to overlap for the same event_id).
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        for (const auto& [eid, hit] : video_geom_) {
            if (rect_contains(hit.world_rect, world)) {
                press_video_     = true;
                press_video_eid_ = eid;
                return true;
            }
        }
    }

    // Image / sticker click-to-view hit-test.
    {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        for (const auto& [eid, hit] : image_geom_) {
            if (rect_contains(hit.world_rect, world)) {
                press_image_     = true;
                press_image_eid_ = eid;
                return true;
            }
        }
    }

    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(),
                                 local, chip_idx);
    if (t == HoverTarget::None) {
        return tk::ListView::on_pointer_down(local);
    }
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size()) {
        return tk::ListView::on_pointer_down(local);
    }
    press_target_    = t;
    press_chip_idx_  = chip_idx;
    press_event_id_  = messages_[row].event_id;
    return true;
}

void MessageListView::on_pointer_up(tk::Point local, bool inside_self) {
    if (press_pill_) {
        bool fire = inside_self;
        press_pill_ = false;
        if (fire) {
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            if (rect_contains(pill_rect_, world)) scroll_to_bottom();
        }
        return;
    }
    if (press_voice_kind_ != VoicePressKind::None) {
        VoicePressKind kind = press_voice_kind_;
        std::string ev      = std::move(press_voice_event_id_);
        press_voice_kind_   = VoicePressKind::None;
        press_voice_event_id_.clear();
        if (!inside_self || ev.empty()) return;
        // Scrub presses already moved the playhead in-flight; nothing left
        // to confirm on release.
        if (kind == VoicePressKind::Waveform) return;

        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        auto it = voice_card_geom_.find(ev);
        if (it == voice_card_geom_.end()) return;
        const auto& geom = it->second;
        const tk::Rect& target = (kind == VoicePressKind::PlayButton)
                                 ? geom.play_button
                                 : geom.speed_pill;
        if (target.w <= 0 || !rect_contains(target, world)) return;
        if (kind == VoicePressKind::SpeedPill) {
            handle_voice_speed_click();
            return;
        }
        // PlayButton.
        for (const auto& row : messages_) {
            if (row.event_id == ev && row.kind == MessageRowData::Kind::Voice) {
                handle_voice_play_click(row);
                return;
            }
        }
        return;
    }
    if (press_reply_btn_) {
        bool  fire = inside_self && !press_reply_event_id_.empty();
        std::string ev = std::move(press_reply_event_id_);
        press_reply_btn_      = false;
        press_reply_event_id_.clear();
        if (fire) {
            // Confirm the pointer is still over the reply button.
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            const tk::Rect& rb = hovered_row_geom_.reply_button;
            if (rb.w > 0 && rect_contains(rb, world)) {
                // Find the row to get sender_name + body_preview.
                for (const auto& row : messages_) {
                    if (row.event_id == ev) {
                        if (on_reply_requested) {
                            on_reply_requested(ev, row.sender_name, row.body);
                        }
                        break;
                    }
                }
            }
        }
        return;
    }
    if (press_edit_btn_) {
        bool  fire = inside_self && !press_edit_event_id_.empty();
        std::string ev = std::move(press_edit_event_id_);
        press_edit_btn_      = false;
        press_edit_event_id_.clear();
        if (fire) {
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            const tk::Rect& eb = hovered_row_geom_.edit_button;
            if (eb.w > 0 && rect_contains(eb, world)) {
                for (const auto& row : messages_) {
                    if (row.event_id == ev) {
                        if (on_edit_requested) on_edit_requested(ev, row.body);
                        break;
                    }
                }
            }
        }
        return;
    }
    if (press_quote_) {
        bool  fire = inside_self && !press_quote_event_id_.empty();
        std::string ev = std::move(press_quote_event_id_);
        press_quote_          = false;
        press_quote_event_id_.clear();
        if (fire) {
            for (std::size_t i = 0; i < messages_.size(); ++i) {
                if (messages_[i].event_id == ev) {
                    const std::string& orig_id = messages_[i].in_reply_to_id;
                    if (orig_id.empty()) break;
                    for (std::size_t j = 0; j < messages_.size(); ++j) {
                        if (messages_[j].event_id == orig_id) {
                            scroll_to_index(static_cast<int>(j));
                            return;
                        }
                    }
                    if (on_scroll_to_original) on_scroll_to_original(orig_id);
                    break;
                }
            }
        }
        return;
    }

    if (press_preview_) {
        bool fire = inside_self && !press_preview_url_.empty();
        std::string url = std::move(press_preview_url_);
        press_preview_     = false;
        press_preview_url_.clear();
        if (fire) {
            // Confirm pointer is still inside the card rect on release.
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            for (const auto& [eid, hit] : preview_card_geom_) {
                if (hit.url == url && rect_contains(hit.rect, world)) {
                    if (on_link_clicked) on_link_clicked(url);
                    break;
                }
            }
        }
        return;
    }

    if (press_video_) {
        bool fire = inside_self && !press_video_eid_.empty();
        std::string eid = std::move(press_video_eid_);
        press_video_     = false;
        press_video_eid_.clear();
        if (fire) {
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            auto it = video_geom_.find(eid);
            if (it != video_geom_.end() &&
                rect_contains(it->second.world_rect, world) &&
                on_video_clicked) {
                on_video_clicked(it->second);
            }
        }
        return;
    }

    if (press_image_) {
        bool fire = inside_self && !press_image_eid_.empty();
        std::string eid = std::move(press_image_eid_);
        press_image_     = false;
        press_image_eid_.clear();
        if (fire) {
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            auto it = image_geom_.find(eid);
            if (it != image_geom_.end() &&
                rect_contains(it->second.world_rect, world) &&
                on_image_clicked) {
                on_image_clicked(it->second);
            }
        }
        return;
    }

    if (press_target_ == HoverTarget::None) {
        tk::ListView::on_pointer_up(local, inside_self);
        return;
    }
    HoverTarget t = press_target_;
    int idx       = press_chip_idx_;
    std::string ev = std::move(press_event_id_);
    press_target_   = HoverTarget::None;
    press_chip_idx_ = -1;
    press_event_id_.clear();
    if (!inside_self) return;

    if (t == HoverTarget::Chip) {
        // Confirm the release still lands on the same chip.
        int now_idx = -1;
        HoverTarget now_t = chip_hit_at(hovered_row_geom_, bounds(),
                                         local, now_idx);
        if (now_t != HoverTarget::Chip || now_idx != idx) return;
        // Find the row that geometry was captured for and read the
        // reaction key directly off the model.
        std::size_t row = hovered_row_geom_.row_index;
        if (row >= messages_.size()) return;
        const auto& reactions = messages_[row].reactions;
        if (idx < 0 || static_cast<std::size_t>(idx) >= reactions.size())
            return;
        if (on_reaction_toggled) {
            on_reaction_toggled(ev, reactions[idx].key);
        }
    } else if (t == HoverTarget::AddButton) {
        int now_idx = -1;
        HoverTarget now_t = chip_hit_at(hovered_row_geom_, bounds(),
                                         local, now_idx);
        if (now_t != HoverTarget::AddButton) return;
        if (on_add_reaction_requested) {
            on_add_reaction_requested(ev, hovered_row_geom_.add_button);
        }
    }
}

std::string MessageListView::newest_visible_real_event_id() const {
    auto [first, last] = visible_range();
    if (last < 0) return {};
    using Kind = MessageRowData::Kind;
    for (int i = last; i >= first; --i) {
        const auto& row = messages_[static_cast<std::size_t>(i)];
        if (row.kind != Kind::DaySeparator &&
            row.kind != Kind::ReadMarker   &&
            row.kind != Kind::TimelineStart &&
            !row.event_id.empty())
            return row.event_id;
    }
    return {};
}

void MessageListView::maybe_notify_receipt_() const {
    if (!on_receipt_needed) return;
    auto eid = newest_visible_real_event_id();
    if (eid.empty() || eid == last_receipt_event_id_) return;
    last_receipt_event_id_ = eid;
    on_receipt_needed(eid);
}

void MessageListView::paint(tk::PaintCtx& ctx) {
    // Sticker, voice, quote, and video rects are rebuilt per-paint by Adapter::paint_row.
    // Clear here so entries scrolled offscreen don't linger.
    sticker_geom_.clear();
    image_geom_.clear();
    video_geom_.clear();
    voice_card_geom_.clear();
    quote_block_geom_.clear();
    preview_card_geom_.clear();
    tk::ListView::paint(ctx);
    maybe_notify_receipt_();

    // Scroll-to-bottom pill — overlays the bottom-right corner of the
    // viewport when the user is not pinned to the live tail. Painted
    // before the chip tooltip so the tooltip (rare, hover-only) wins on
    // any geometric overlap. Click handling lives in on_pointer_*.
    pill_visible_ = should_show_pill();
    if (pill_visible_) {
        constexpr float kSz = 36.0f, kInsetR = 12.0f, kInsetB = 16.0f;
        tk::Rect v = bounds();
        pill_rect_ = { v.x + v.w - kSz - kInsetR,
                       v.y + v.h - kSz - kInsetB, kSz, kSz };
        auto bg = press_pill_ ? ctx.theme.palette.subtle_pressed
                              : ctx.theme.palette.chrome_bg;
        ctx.canvas.fill_rounded_rect  (pill_rect_, kSz * 0.5f, bg);
        ctx.canvas.stroke_rounded_rect(pill_rect_, kSz * 0.5f,
                                        ctx.theme.palette.border, 1.0f);
        tk::TextStyle gs{};
        gs.role = tk::FontRole::UiSemibold;
        gs.wrap = false;
        auto glyph = ctx.factory.build_text("\xE2\x86\x93", gs); // U+2193 ↓
        if (glyph) {
            tk::Size sz = glyph->measure();
            ctx.canvas.draw_text(*glyph,
                { pill_rect_.x + (kSz - sz.w) * 0.5f,
                  pill_rect_.y + (kSz - sz.h) * 0.5f },
                ctx.theme.palette.text_primary);
        }
    } else {
        pill_rect_ = {};
    }

    // Tooltip overlay: paint a small panel listing senders of the
    // hovered reaction chip, or the display name of the hovered read-receipt
    // disc. We paint after rows so the panel sits on top of subsequent rows.
    if (hover_target_ == HoverTarget::Receipt) {
        if (hover_chip_idx_ < 0) return;
        std::size_t row = hovered_row_geom_.row_index;
        if (row >= messages_.size()) return;
        const auto& rrs = messages_[row].read_receipts;
        const std::size_t total   = rrs.size();
        const std::size_t visible = std::min(total, kReceiptCap);
        if (static_cast<std::size_t>(hover_chip_idx_) >= visible) return;
        // receipt_discs[i] corresponds to rrs[total - 1 - i] (newest first).
        const auto& rr = rrs[total - 1 - static_cast<std::size_t>(hover_chip_idx_)];
        const std::string& label = rr.display_name.empty() ? rr.user_id : rr.display_name;

        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto layout = ctx.factory.build_text(label, st);
        if (!layout) return;
        tk::Size sz = layout->measure();

        constexpr float kTipPadX = 8.0f;
        constexpr float kTipPadY = 6.0f;
        float panel_w = sz.w + kTipPadX * 2;
        float panel_h = sz.h + kTipPadY * 2;

        tk::Rect disc = hovered_row_geom_.receipt_discs[hover_chip_idx_];
        tk::Rect view = bounds();

        float panel_y = disc.y - panel_h - 4.0f;
        if (panel_y < view.y) panel_y = disc.y + disc.h + 4.0f;
        float panel_x = disc.x + disc.w * 0.5f - panel_w * 0.5f;
        if (panel_x + panel_w > view.x + view.w) panel_x = view.x + view.w - panel_w - 4.0f;
        if (panel_x < view.x + 4.0f) panel_x = view.x + 4.0f;

        tk::Rect panel{ panel_x, panel_y, panel_w, panel_h };
        ctx.canvas.fill_rounded_rect  (panel, 6.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(panel, 6.0f, ctx.theme.palette.border, 1.0f);
        ctx.canvas.draw_text(*layout,
                              { panel.x + kTipPadX, panel.y + kTipPadY },
                              ctx.theme.palette.text_primary);
        return;
    }

    if (hover_target_ != HoverTarget::Chip) return;
    if (hover_chip_idx_ < 0) return;
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size()) return;
    const auto& reactions = messages_[row].reactions;
    if (static_cast<std::size_t>(hover_chip_idx_) >= reactions.size()) return;
    const auto& r = reactions[hover_chip_idx_];
    if (r.senders.empty()) return;

    // Build one TextLayout per line. Canvas backends measure single-line
    // text via advance width / font height; a multi-line string returns
    // single-line dimensions even though draw renders the newlines, which
    // would clip the panel. Stacking per-line layouts gives the panel an
    // accurate height and correct max width across all backends.
    std::vector<std::string> lines;
    lines.reserve(r.senders.size() + 1);
    {
        std::string header = "Reacted with ";
        header += r.key;
        header += ":";
        lines.push_back(std::move(header));
    }
    for (const auto& s : r.senders) lines.push_back(s);

    tk::TextStyle st{};
    st.role = tk::FontRole::Small;
    st.wrap = false;

    struct LineLayout {
        std::unique_ptr<tk::TextLayout> layout;
        tk::Size size{};
    };
    std::vector<LineLayout> ls;
    ls.reserve(lines.size());
    float max_w = 0.0f;
    float total_h = 0.0f;
    for (const auto& line : lines) {
        auto layout = ctx.factory.build_text(line, st);
        if (!layout) return;
        tk::Size sz = layout->measure();
        max_w = std::max(max_w, sz.w);
        total_h += sz.h;
        ls.push_back({ std::move(layout), sz });
    }

    constexpr float kTipPadX = 8.0f;
    constexpr float kTipPadY = 6.0f;
    float panel_w = max_w + kTipPadX * 2;
    float panel_h = total_h + kTipPadY * 2;

    tk::Rect chip = hovered_row_geom_.chips[hover_chip_idx_];
    tk::Rect view = bounds();

    // Prefer above the chip; flip below if it would clip the top.
    float panel_y = chip.y - panel_h - 4.0f;
    if (panel_y < view.y) {
        panel_y = chip.y + chip.h + 4.0f;
    }
    float panel_x = chip.x;
    if (panel_x + panel_w > view.x + view.w) {
        panel_x = view.x + view.w - panel_w - 4.0f;
    }
    if (panel_x < view.x + 4.0f) {
        panel_x = view.x + 4.0f;
    }

    tk::Rect panel{ panel_x, panel_y, panel_w, panel_h };
    ctx.canvas.fill_rounded_rect(panel, 6.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(panel, 6.0f, ctx.theme.palette.border, 1.0f);

    float y = panel.y + kTipPadY;
    for (const auto& line : ls) {
        ctx.canvas.draw_text(*line.layout,
                              { panel.x + kTipPadX, y },
                              ctx.theme.palette.text_primary);
        y += line.size.h;
    }
}

} // namespace tesseract::views
