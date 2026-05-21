#include "MessageListView.h"
#include "html_spans.h"
#include "map_tiles.h"
#include "media_utils.h"

#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tesseract::views
{

// ── Multi-click selection helpers ────────────────────────────────────────────

static std::string spans_to_plain(const std::vector<tk::TextSpan>& spans)
{
    std::string out;
    for (const auto& s : spans)
        out += s.text;
    return out;
}

static std::pair<int, int> word_range_in_text(const std::string& text,
                                               int byte_offset)
{
    int n = static_cast<int>(text.size());
    byte_offset = std::clamp(byte_offset, 0, n);
    auto is_word = [](unsigned char c)
    { return c >= 0x80 || std::isalnum(c) || c == '_'; };
    int lo = byte_offset;
    while (lo > 0 && is_word(static_cast<unsigned char>(text[lo - 1])))
        --lo;
    int hi = byte_offset;
    while (hi < n && is_word(static_cast<unsigned char>(text[hi])))
        ++hi;
    if (lo == hi) // on a non-word char: select just that char
        hi = std::min(hi + 1, n);
    return {lo, hi};
}

static std::pair<int, int> line_range_in_text(const std::string& text,
                                               int byte_offset)
{
    int n = static_cast<int>(text.size());
    byte_offset = std::clamp(byte_offset, 0, n);
    int lo = byte_offset;
    while (lo > 0 && text[lo - 1] != '\n')
        --lo;
    int hi = byte_offset;
    while (hi < n && text[hi] != '\n')
        ++hi;
    return {lo, hi};
}

static int64_t steady_ms_now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ─────────────────────────────────────────────────────────────────────────────

MessageRowData make_row_data(const tesseract::Event& ev,
                             const std::string& my_user_id)
{
    using Kind = MessageRowData::Kind;
    MessageRowData row;
    row.event_id = ev.event_id;
    row.sender = ev.sender;
    row.sender_name = ev.sender_name;
    row.sender_avatar_url = ev.sender_avatar_url;
    row.body = ev.body;
    row.formatted_body = ev.formatted_body;
    row.timestamp_ms = ev.timestamp;
    row.is_own = !my_user_id.empty() && ev.sender == my_user_id;
    row.reactions = ev.reactions;
    row.read_receipts = ev.read_receipts;

    row.in_reply_to_id = ev.in_reply_to_id;
    row.in_reply_to_sender_name = ev.in_reply_to_sender_name;
    // Collapse newlines to spaces: the quote card has a fixed height and no
    // clip rect, so hard line-breaks in a multiline original message cause
    // the text to render outside the card bounds.
    {
        std::string flat;
        flat.reserve(ev.in_reply_to_body.size());
        for (char c : ev.in_reply_to_body)
            flat += (c == '\n' || c == '\r') ? ' ' : c;
        row.in_reply_to_body = std::move(flat);
    }
    row.is_edited = ev.is_edited;

    if (ev.pending_state == "sending")
    {
        row.pending_state = MessageRowData::PendingState::Sending;
    }
    else if (ev.pending_state == "failed")
    {
        row.pending_state = MessageRowData::PendingState::Failed;
    }
    else
    {
        row.pending_state = MessageRowData::PendingState::None;
    }
    row.pending_txn_id = ev.pending_txn_id;
    row.pending_error = ev.pending_error;
    row.pending_recoverable = ev.pending_recoverable;

    switch (ev.type)
    {
    case tesseract::EventType::Text:
        row.kind = Kind::Text;
        break;
    case tesseract::EventType::Notice:
        row.kind = Kind::Notice;
        break;
    case tesseract::EventType::Emote:
        row.kind = Kind::Emote;
        break;
    case tesseract::EventType::Image:
    {
        row.kind = Kind::Image;
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        row.source    = img.source;
        row.thumbnail = img.thumbnail;
        row.media_w = static_cast<int>(img.width);
        row.media_h = static_cast<int>(img.height);
        row.has_filename_caption = !img.filename.empty();
        row.blurhash = img.blurhash;
        row.image_animated = img.animated;
        break;
    }
    case tesseract::EventType::Sticker:
    {
        row.kind = Kind::Sticker;
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        row.source    = s.source;
        row.thumbnail = s.thumbnail;
        row.media_w = static_cast<int>(s.width);
        row.media_h = static_cast<int>(s.height);
        row.blurhash = s.blurhash;
        row.sticker_info_json = s.info_json;
        row.image_animated = s.animated;
        break;
    }
    case tesseract::EventType::File:
    {
        row.kind = Kind::File;
        const auto& f = static_cast<const tesseract::FileEvent&>(ev);
        row.file_source = f.source;
        row.file_name   = f.file_name;
        row.file_size   = f.file_size;
        break;
    }
    case tesseract::EventType::Audio:
    {
        row.kind = Kind::Audio;
        const auto& a = static_cast<const tesseract::AudioEvent&>(ev);
        row.audio_source = a.source;
        row.audio_mime   = a.mime_type;
        row.duration_ms  = a.duration_ms;
        row.file_name    = a.filename;
        row.file_size    = a.file_size;
        break;
    }
    case tesseract::EventType::Voice:
    {
        row.kind = Kind::Voice;
        const auto& v = static_cast<const tesseract::VoiceEvent&>(ev);
        row.audio_source = v.source;
        row.audio_mime   = v.mime_type;
        row.duration_ms  = v.duration_ms;
        row.waveform     = v.waveform;
        break;
    }
    case tesseract::EventType::Video:
    {
        row.kind = Kind::Video;
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        row.source    = vid.source;
        // When the server omits a thumbnail, the client-side first-frame
        // generator fills image_provider_ under a "thumb::<event_id>" key.
        // Store it as a PlainMediaSource so fetch_token() / image provider
        // lookups are uniform.  mxc_url() on this sentinel returns a non-mxc
        // string — it must only be used as a cache key, never sent to a server.
        row.thumbnail = vid.thumbnail
                            ? vid.thumbnail
                            : tesseract::MediaSource::plain("thumb::" + ev.event_id);
        row.video_mime = vid.mime_type;
        row.media_w = static_cast<int>(vid.width);
        row.media_h = static_cast<int>(vid.height);
        row.duration_ms = vid.duration_ms;
        row.has_filename_caption = !vid.filename.empty();
        row.video_autoplay = vid.autoplay;
        row.video_loop = vid.loop;
        row.video_no_audio = vid.no_audio;
        row.video_hide_controls = vid.hide_controls;
        row.video_gif = vid.gif;
        row.blurhash = vid.blurhash;
        break;
    }
    case tesseract::EventType::Location:
    {
        const auto& loc = static_cast<const tesseract::LocationEvent&>(ev);
        row.kind = Kind::Location;
        row.location_lat = loc.lat;
        row.location_lon = loc.lon;
        row.location_description = loc.description;
        row.map_viewport = {loc.lat, loc.lon, 15};
        break;
    }
    case tesseract::EventType::Redacted:
        row.kind = Kind::Redacted;
        break;
    case tesseract::EventType::Unhandled:
        row.kind = Kind::Unhandled;
        break;
    case tesseract::EventType::DaySeparator:
        row.kind = Kind::DaySeparator;
        break;
    case tesseract::EventType::ReadMarker:
        row.kind = Kind::ReadMarker;
        break;
    case tesseract::EventType::TimelineStart:
        row.kind = Kind::TimelineStart;
        break;
    }

    // Extract the first URL from text messages for preview card display.
    if (row.kind == Kind::Text || row.kind == Kind::Notice ||
        row.kind == Kind::Emote || row.kind == Kind::Unhandled)
    {
        if (!row.formatted_body.empty())
        {
            row.first_url = first_url_from_html(row.formatted_body);
        }
        if (row.first_url.empty() && !row.body.empty())
        {
            row.first_url = first_url_from_plain(row.body);
        }
    }

    return row;
}

namespace
{

constexpr float kPadX = tesseract::visual::kSpaceMD;                  // 12
constexpr float kPadY = tesseract::visual::kMsgRowVerticalPad;        // 6
constexpr float kAvatarSize = tesseract::visual::kMsgAvatarSize;      // 32
constexpr float kAvatarGap = tesseract::visual::kMsgAvatarGap;        // 8
constexpr float kSenderH = tesseract::visual::kMsgSenderNameHeight;   // 16
constexpr float kTimestampH = tesseract::visual::kMsgTimestampHeight; // 14
constexpr float kChipPadX = 10.0f;

// Read-receipt avatar cluster — painted at the bottom-right of each row,
// inside the existing bounds. Discs overlap by (kReceiptSize - kReceiptStride)
// so a busy room's receipts stay narrow. Anything above `kReceiptCap` collapses
// into a small "+N" pill anchored to the left of the cluster.
constexpr float kReceiptSize = 16.0f;
constexpr float kReceiptStride = 11.0f; // 5 px overlap
constexpr std::size_t kReceiptCap = 5;
constexpr float kReceiptOverflowGap = 4.0f; // gap between "+N" pill and discs

inline float chip_h()
{
    return static_cast<float>(
        tesseract::Settings::instance().reaction_chip_height);
}
inline float chip_gap()
{
    return static_cast<float>(
        tesseract::Settings::instance().reaction_chip_gap);
}
inline float chip_radius()
{
    return chip_h() * 0.5f;
}
constexpr float kImageMaxW = tesseract::visual::kMaxInlineImageWidth;  // 320
constexpr float kImageMaxH = tesseract::visual::kMaxInlineImageHeight; // 200
constexpr float kStickerSize = tesseract::visual::kStickerSize;        // 256
constexpr float kFileCardH = 56.0f;
constexpr float kFileCardW = 280.0f;

// URL preview card dimensions.
constexpr float kPreviewCardH = 72.0f;
constexpr float kPreviewCardW = 280.0f;
constexpr float kPreviewThumbSide = 56.0f;
constexpr float kPreviewCardPad = 10.0f;
constexpr float kPreviewCardGapTop = 6.0f;
constexpr float kFileIconSize = 36.0f;
constexpr float kFileIconPadL = 10.0f;
constexpr float kFileTextOffX = kFileIconPadL + kFileIconSize + 8.0f; // 54px

// MSC3245 voice card. Same width as the file card so the timeline stays
// visually aligned, slightly shorter because there's no second line of
// metadata. Play button is a 32 px circle on the left; remaining width
// is split between the waveform strip and the right-justified duration
// label.
constexpr float kVoiceCardH = 48.0f;
constexpr float kVoiceCardW = 280.0f;

// Audio card (plain m.audio, no waveform): two-row layout.
// Row 1: play/pause + linear progress track + time label.
// Row 2: filename · file size.
constexpr float kAudioCardH = 64.0f;
constexpr float kAudioCardW = 280.0f;
constexpr float kAudioPlayBtnSize = 32.0f;
constexpr float kAudioCardPadX = 8.0f;
constexpr float kAudioTrackH = 4.0f; // progress track height
constexpr float kVoicePlayBtnSize = 32.0f;
constexpr float kVoiceCardPadX = 8.0f;
constexpr float kVoiceBarW = 3.0f;
constexpr float kVoiceBarGap = 2.0f;
constexpr float kVoiceBarMinH = 3.0f;     // placeholder bar height
constexpr float kVoiceDurationW = 40.0f;  // reserved for "0:00" label
constexpr float kVoiceSpeedPillW = 30.0f; // "1×" / "1.5×" / "2×"
constexpr float kVoiceSpeedPillH = 20.0f;

// Reply quote block — painted above the body block when m.has_reply().
constexpr float kQuoteBlockH = 44.0f; // total height of the quote band
constexpr float kQuoteAccentW = 3.0f; // left accent stripe width
constexpr float kQuotePadX = 8.0f;
constexpr float kQuoteGapAfter = 4.0f; // gap between quote and body
// Reply hover button ("↩") — right-aligned in the chip strip.
constexpr float kReplyBtnW = 28.0f;
constexpr float kReplyBtnPadX = 8.0f;
// Edit hover button ("✏") — left of the reply button in the chip strip.
constexpr float kEditBtnW = 28.0f;
// "(edited)" badge — appended after the body text of an edited message.
constexpr float kEditedBadgeGap = 4.0f;
// Reduced top padding for continuation rows (no avatar/sender chrome).
constexpr float kContPadY = 2.0f;

// Virtual timeline item heights.
constexpr float kDaySepH = 28.0f;
constexpr float kReadMarkerH = 20.0f;
constexpr float kTimelineStartH = 20.0f;
constexpr float kTypingRowH = 20.0f;

// Max time the message list is held invisible after a room switch while
// the visible rows' height-affecting media / preview cards resolve, before
// revealing anyway. Bounds the worst case on a slow / offline network.
constexpr int kRoomSwitchGateTimeoutMs = 400;

// Duration to display "just sent" highlight on own messages before auto-clearing.
constexpr int kJustSentHighlightMs = 2000;

std::string format_mmss(std::uint64_t ms)
{
    if (ms == 0)
    {
        return "0:00";
    }
    std::uint64_t total_s = ms / 1000;
    std::uint64_t mm = total_s / 60;
    std::uint64_t ss = total_s % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(mm),
                  static_cast<unsigned long long>(ss));
    return std::string(buf);
}

std::string format_hhmm(std::uint64_t timestamp_ms)
{
    if (timestamp_ms == 0)
    {
        return {};
    }
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return std::string(buf);
}

std::string format_size(std::uint64_t bytes)
{
    if (bytes < 1024)
    {
        return std::to_string(bytes) + " B";
    }
    if (bytes < 1024 * 1024)
    {
        return std::to_string(bytes / 1024) + " KB";
    }
    if (bytes < 1024ull * 1024 * 1024)
    {
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
}

struct FileIconInfo
{
    tk::Color color;
    std::string label; // uppercase extension, ≤4 chars
};

static FileIconInfo file_icon_info(std::string_view filename)
{
    auto dot = filename.rfind('.');
    std::string ext;
    if (dot != std::string_view::npos)
    {
        ext = std::string(filename.substr(dot + 1));
        for (auto& c : ext)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    std::string label = ext.empty() ? "FILE" : ext;
    for (auto& c : label)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (label.size() > 4)
    {
        label.resize(4);
    }

    static const std::unordered_map<std::string, tk::Color> map = {
        // images
        {"png", tk::Color::rgb(0x22A062)},
        {"jpg", tk::Color::rgb(0x22A062)},
        {"jpeg", tk::Color::rgb(0x22A062)},
        {"webp", tk::Color::rgb(0x22A062)},
        {"gif", tk::Color::rgb(0x22A062)},
        {"bmp", tk::Color::rgb(0x22A062)},
        {"svg", tk::Color::rgb(0x22A062)},
        {"avif", tk::Color::rgb(0x22A062)},
        // documents
        {"pdf", tk::Color::rgb(0x2B7DD4)},
        {"doc", tk::Color::rgb(0x2B7DD4)},
        {"docx", tk::Color::rgb(0x2B7DD4)},
        {"odt", tk::Color::rgb(0x2B7DD4)},
        {"txt", tk::Color::rgb(0x2B7DD4)},
        {"rtf", tk::Color::rgb(0x2B7DD4)},
        {"epub", tk::Color::rgb(0x2B7DD4)},
        // spreadsheets
        {"xls", tk::Color::rgb(0x2E8B4A)},
        {"xlsx", tk::Color::rgb(0x2E8B4A)},
        {"ods", tk::Color::rgb(0x2E8B4A)},
        {"csv", tk::Color::rgb(0x2E8B4A)},
        // presentations
        {"ppt", tk::Color::rgb(0xC0572B)},
        {"pptx", tk::Color::rgb(0xC0572B)},
        {"odp", tk::Color::rgb(0xC0572B)},
        {"key", tk::Color::rgb(0xC0572B)},
        // archives
        {"zip", tk::Color::rgb(0xE07B1E)},
        {"tar", tk::Color::rgb(0xE07B1E)},
        {"gz", tk::Color::rgb(0xE07B1E)},
        {"bz2", tk::Color::rgb(0xE07B1E)},
        {"xz", tk::Color::rgb(0xE07B1E)},
        {"7z", tk::Color::rgb(0xE07B1E)},
        {"rar", tk::Color::rgb(0xE07B1E)},
        {"zst", tk::Color::rgb(0xE07B1E)},
        // audio
        {"mp3", tk::Color::rgb(0x7C54C8)},
        {"wav", tk::Color::rgb(0x7C54C8)},
        {"ogg", tk::Color::rgb(0x7C54C8)},
        {"flac", tk::Color::rgb(0x7C54C8)},
        {"m4a", tk::Color::rgb(0x7C54C8)},
        {"aac", tk::Color::rgb(0x7C54C8)},
        {"opus", tk::Color::rgb(0x7C54C8)},
        // video
        {"mp4", tk::Color::rgb(0xD13030)},
        {"mkv", tk::Color::rgb(0xD13030)},
        {"avi", tk::Color::rgb(0xD13030)},
        {"mov", tk::Color::rgb(0xD13030)},
        {"webm", tk::Color::rgb(0xD13030)},
        {"m4v", tk::Color::rgb(0xD13030)},
        // code
        {"py", tk::Color::rgb(0x1A8F8A)},
        {"js", tk::Color::rgb(0x1A8F8A)},
        {"ts", tk::Color::rgb(0x1A8F8A)},
        {"cpp", tk::Color::rgb(0x1A8F8A)},
        {"h", tk::Color::rgb(0x1A8F8A)},
        {"rs", tk::Color::rgb(0x1A8F8A)},
        {"java", tk::Color::rgb(0x1A8F8A)},
        {"json", tk::Color::rgb(0x1A8F8A)},
        {"xml", tk::Color::rgb(0x1A8F8A)},
        {"html", tk::Color::rgb(0x1A8F8A)},
        {"css", tk::Color::rgb(0x1A8F8A)},
        {"go", tk::Color::rgb(0x1A8F8A)},
        {"sh", tk::Color::rgb(0x1A8F8A)},
        {"rb", tk::Color::rgb(0x1A8F8A)},
    };
    constexpr tk::Color kGeneric = tk::Color::rgb(0x7A7A8E);
    auto it = map.find(ext);
    return {it != map.end() ? it->second : kGeneric, std::move(label)};
}

float body_text_max_width(float row_width)
{
    return std::max(0.0f, row_width - kPadX - kAvatarSize - kAvatarGap - kPadX);
}

// Width to reserve on the right of the body column for a read-receipt cluster.
// Returns 0 when there are no receipts or when reactions are present (the
// cluster then floats beside the chip strip, not overlapping body text).
inline float receipt_reserve_width(const MessageRowData& m)
{
    if (m.read_receipts.empty() || !m.reactions.empty())
    {
        return 0.0f;
    }
    const std::size_t n = std::min(m.read_receipts.size(), kReceiptCap);
    return kReceiptSize + static_cast<float>(n - 1) * kReceiptStride +
           chip_gap();
}

std::string format_day_label(std::uint64_t timestamp_ms)
{
    if (timestamp_ms == 0)
    {
        return {};
    }
    std::time_t now_t = std::time(nullptr);
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm now_tm{}, sep_tm{};
#if defined(_WIN32)
    localtime_s(&now_tm, &now_t);
    localtime_s(&sep_tm, &t);
#else
    localtime_r(&now_t, &now_tm);
    localtime_r(&t, &sep_tm);
#endif
    if (sep_tm.tm_year == now_tm.tm_year && sep_tm.tm_mon == now_tm.tm_mon &&
        sep_tm.tm_mday == now_tm.tm_mday)
    {
        return "Today";
    }
    std::time_t yest_t = now_t - 86400;
    std::tm yest_tm{};
#if defined(_WIN32)
    localtime_s(&yest_tm, &yest_t);
#else
    localtime_r(&yest_t, &yest_tm);
#endif
    if (sep_tm.tm_year == yest_tm.tm_year && sep_tm.tm_mon == yest_tm.tm_mon &&
        sep_tm.tm_mday == yest_tm.tm_mday)
    {
        return "Yesterday";
    }
    if (now_t > t && static_cast<std::uint64_t>(now_t - t) < 7u * 86400u)
    {
        constexpr const char* kDays[] = {"Sunday",    "Monday",   "Tuesday",
                                         "Wednesday", "Thursday", "Friday",
                                         "Saturday"};
        return kDays[sep_tm.tm_wday];
    }
    constexpr const char* kMonths[] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %d, %d", kMonths[sep_tm.tm_mon],
                  sep_tm.tm_mday, sep_tm.tm_year + 1900);
    return buf;
}

} // namespace

class MessageListView::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(MessageListView& owner) : owner_(owner)
    {
    }

    // The optional trailing typing row is a synthetic UI row that is NOT
    // part of messages_ (which mirrors the SDK timeline 1:1). It always
    // sits at index == messages_.size() when active, so it scrolls with
    // the tail and is naturally outside the viewport when the user scrolls
    // up — exactly the requested behaviour.
    bool is_typing_index(std::size_t i) const
    {
        return i == owner_.messages_.size();
    }

    // Returns true if at least one real content row (not DaySeparator,
    // ReadMarker, or TimelineStart) exists after `index`.
    bool has_content_after(std::size_t index) const
    {
        using Kind = MessageRowData::Kind;
        for (std::size_t j = index + 1; j < owner_.messages_.size(); ++j)
        {
            auto k = owner_.messages_[j].kind;
            if (k != Kind::DaySeparator && k != Kind::ReadMarker &&
                k != Kind::TimelineStart)
            {
                return true;
            }
        }
        return false;
    }

    // Returns true only if there is at least one real content row between
    // this index and the next DaySeparator (or the end of the list). Used to
    // suppress day separators that have no visible events beneath them, so
    // consecutive empty-day separators collapse to just the last one.
    bool has_content_before_next_separator(std::size_t index) const
    {
        using Kind = MessageRowData::Kind;
        for (std::size_t j = index + 1; j < owner_.messages_.size(); ++j)
        {
            const Kind k = owner_.messages_[j].kind;
            if (k == Kind::DaySeparator)
                return false;
            if (k != Kind::ReadMarker && k != Kind::TimelineStart)
                return true;
        }
        return false;
    }

    std::size_t count() const override
    {
        return owner_.messages_.size() + 1; // +1 for always-present typing row
    }

    // True when `index` is a continuation of the previous row: same
    // sender, not a reply, within the grouping window. Continuation
    // rows suppress the avatar and sender name.
    bool is_cont(std::size_t index) const
    {
        if (index == 0)
        {
            return false;
        }
        const auto& curr = owner_.messages_[index];
        // Virtual rows are never continuations; nor is the row after one.
        using Kind = MessageRowData::Kind;
        if (curr.kind == Kind::DaySeparator || curr.kind == Kind::ReadMarker ||
            curr.kind == Kind::TimelineStart)
        {
            return false;
        }
        if (curr.has_reply())
        {
            return false;
        }
        // When the read marker is suppressed (waiting for the SDK to move
        // it after a new message), skip over it so a message from the same
        // sender is still treated as a continuation rather than a new group.
        std::size_t prev_idx = index - 1;
        if (owner_.suppress_read_marker_)
        {
            while (prev_idx > 0 &&
                   owner_.messages_[prev_idx].kind == Kind::ReadMarker)
            {
                --prev_idx;
            }
            if (owner_.messages_[prev_idx].kind == Kind::ReadMarker)
            {
                return false;
            }
        }
        const auto& prev = owner_.messages_[prev_idx];
        if (prev.kind == Kind::DaySeparator || prev.kind == Kind::ReadMarker ||
            prev.kind == Kind::TimelineStart)
        {
            return false;
        }
        if (prev.sender != curr.sender)
        {
            return false;
        }
        int interval_s =
            tesseract::Settings::instance().message_group_interval_s;
        if (interval_s <= 0)
        {
            return false;
        }
        if (curr.timestamp_ms < prev.timestamp_ms)
        {
            return false;
        }
        return (curr.timestamp_ms - prev.timestamp_ms) <=
               static_cast<std::uint64_t>(interval_s) * 1000;
    }

    float measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                             float width) override
    {
        if (is_typing_index(index))
        {
            return kTypingRowH;
        }
        if (index >= owner_.messages_.size())
        {
            return 0;
        }
        const auto& m = owner_.messages_[index];
        using Kind = MessageRowData::Kind;
        if (m.kind == Kind::DaySeparator)
        {
            return has_content_before_next_separator(index) ? kDaySepH : 0.0f;
        }
        if (m.kind == Kind::ReadMarker)
        {
            return (owner_.suppress_read_marker_ || !has_content_after(index))
                       ? 0.0f : kReadMarkerH;
        }
        if (m.kind == Kind::TimelineStart)
        {
            return kTimelineStartH;
        }
        bool cont = is_cont(index);
        float body_w = std::max(0.0f, body_text_max_width(width) -
                                          receipt_reserve_width(m));
        float body_h = measure_body_block_height(m, ctx, body_w);
        float eff_chip_h = cont ? std::min(chip_h(), body_h) : chip_h();
        float chips_h = !m.reactions.empty() ? eff_chip_h : 0.0f;
        float top_pad = cont ? kContPadY : kPadY;
        float header_h = cont ? 0.0f : kAvatarSize;
        float raw_h = top_pad + header_h + body_h + chips_h + kPadY;
        // Continuation rows without reactions must be at least chip_h() tall so
        // the hover action buttons (same height as chip_h) fit without overflow.
        if (cont && chips_h == 0.0f)
        {
            raw_h = std::max(raw_h, chip_h());
        }
        return raw_h;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool /*selected*/, bool hovered) override
    {
        if (is_typing_index(index))
        {
            paint_typing_row(ctx, bounds);
            return;
        }
        if (index >= owner_.messages_.size())
        {
            return;
        }
        const auto& m = owner_.messages_[index];

        // Virtual items get their own minimal rendering — no avatar/body layout.
        using Kind = MessageRowData::Kind;
        if (m.kind == Kind::DaySeparator)
        {
            if (has_content_before_next_separator(index))
            {
                paint_day_separator(m, ctx, bounds);
            }
            return;
        }
        if (m.kind == Kind::ReadMarker)
        {
            if (!owner_.suppress_read_marker_ && has_content_after(index))
            {
                paint_read_marker(ctx, bounds);
            }
            return;
        }
        if (m.kind == Kind::TimelineStart)
        {
            paint_timeline_start(ctx, bounds);
            return;
        }

        bool cont = is_cont(index);

        if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.subtle_hover);
            owner_.hovered_row_geom_.row_index = index;
            owner_.hovered_row_geom_.row_bounds = bounds;
            owner_.hovered_row_geom_.chips.clear();
            owner_.hovered_row_geom_.receipt_discs.clear();
            owner_.hovered_row_geom_.add_button = tk::Rect{};
            owner_.hovered_row_geom_.add_visible = false;
            owner_.hovered_row_geom_.reply_button = tk::Rect{};
            owner_.hovered_row_geom_.edit_button = tk::Rect{};
            owner_.hovered_row_geom_.delete_button = tk::Rect{};
            owner_.hovered_row_geom_.retry_button = tk::Rect{};
            owner_.hovered_row_geom_.abort_button = tk::Rect{};
        }

        // Avatar column centre — used both for painting and for the
        // hover timestamp (continuation rows skip the avatar itself).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + kPadY + kAvatarSize * 0.5f;

        // Right-of-avatar column (same indent for cont + non-cont so
        // body text aligns with the row above in a continuation group).
        float col_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float col_w = std::max(0.0f, bounds.x + bounds.w - col_x - kPadX -
                                         receipt_reserve_width(m));

        if (!cont)
        {
            // Avatar disc / initials.
            const tk::Image* avatar = nullptr;
            if (owner_.avatar_provider_ && !m.sender_avatar_url.empty())
            {
                avatar = owner_.avatar_provider_(m.sender_avatar_url);
            }
            if (avatar)
            {
                ctx.canvas.draw_circle_image(*avatar, {avatar_cx, avatar_cy},
                                             kAvatarSize);
            }
            else
            {
                ctx.canvas.draw_initials_circle(
                    m.sender_name.empty() ? m.sender : m.sender_name,
                    {avatar_cx, avatar_cy}, kAvatarSize,
                    ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);
            }

            // Sender name — vertically centered against the avatar disc.
            float sender_y = bounds.y + kPadY + (kAvatarSize - kSenderH) * 0.5f;
            auto& rc = cache_for(index);
            const std::string skey =
                m.sender_name.empty() ? m.sender : m.sender_name;
            if (!rc.sender || rc.sender_key != skey || rc.sender_col_w != col_w)
            {
                tk::TextStyle s{};
                s.role = tk::FontRole::SenderName;
                s.trim = tk::TextTrim::Ellipsis;
                s.max_width = col_w;
                rc.sender = ctx.factory.build_text(skey, s);
                rc.sender_key = skey;
                rc.sender_col_w = col_w;
            }
            if (rc.sender)
            {
                ctx.canvas.draw_text(*rc.sender, {col_x, sender_y},
                                     ctx.theme.palette.text_secondary);
            }
        }

        // Body block: below avatar for full rows, tight to top for continuations.
        float body_top =
            cont ? (bounds.y + kContPadY) : (bounds.y + kPadY + kAvatarSize);
        float cursor = body_top;
        cursor = paint_body_block(m, ctx, col_x, cursor, col_w);
        // For continuation rows (no avatar), cap reaction chips to body height
        // so a single-line message's chip strip never exceeds the text area.
        float eff_chip_h =
            cont ? std::min(chip_h(), cursor - body_top) : chip_h();
        float eff_chip_r = eff_chip_h * 0.5f;

        // ── Hover-button overlay (no reactions / no receipts) ───────────────
        // When there is nothing to permanently show below the body, the row
        // is compact (chips_h == 0). In that case the +/↩/✏ hover buttons
        // float right-aligned at the avatar-band height so they never push
        // the row taller. Built right-to-left so the cluster stays flush with
        // the right edge regardless of which buttons are present.
        if (hovered && m.reactions.empty())
        {
            static_cache_.ensure(ctx.factory);
            float btn_y =
                cont ? (bounds.y + (bounds.h - chip_h()) * 0.5f)
                     : (bounds.y + kPadY + (kAvatarSize - chip_h()) * 0.5f);
            float btn_right = bounds.x + bounds.w - kPadX;
            if (!m.read_receipts.empty())
            {
                const std::size_t n =
                    std::min(m.read_receipts.size(), kReceiptCap);
                float cluster_w =
                    kReceiptSize + static_cast<float>(n - 1) * kReceiptStride;
                btn_right -= cluster_w + chip_gap();
            }

            auto paint_btn = [&](const tk::TextLayout* l, tk::Rect& geom_out)
            {
                if (!l)
                {
                    return;
                }
                tk::Size sz = l->measure();
                float w = std::max(sz.w + kReplyBtnPadX * 2, chip_h() + 4.0f);
                tk::Rect pill{btn_right - w, btn_y, w, chip_h()};
                ctx.canvas.fill_rounded_rect(pill, chip_radius(),
                                             ctx.theme.palette.subtle_hover);
                ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                                               ctx.theme.palette.border, 1.0f);
                ctx.canvas.draw_text(
                    *l,
                    {pill.x + kReplyBtnPadX, pill.y + (pill.h - sz.h) * 0.5f},
                    ctx.theme.palette.text_secondary);
                geom_out = pill;
                btn_right -= w + chip_gap();
            };

            if (m.is_own && m.kind != MessageRowData::Kind::Redacted)
            {
                paint_btn(static_cache_.trash.get(),
                          owner_.hovered_row_geom_.delete_button);
            }
            if (m.is_own && m.kind == MessageRowData::Kind::Text)
            {
                paint_btn(static_cache_.edit.get(),
                          owner_.hovered_row_geom_.edit_button);
            }
            paint_btn(static_cache_.reply.get(),
                      owner_.hovered_row_geom_.reply_button);
            if (const auto* l = static_cache_.plus.get())
            {
                tk::Size sz = l->measure();
                float w = std::max(sz.w + kChipPadX * 2, chip_h() + 8.0f);
                tk::Rect pill{btn_right - w, btn_y, w, chip_h()};
                bool add_hov = owner_.hover_target_ == HoverTarget::AddButton;
                ctx.canvas.fill_rounded_rect(
                    pill, chip_radius(),
                    add_hov ? ctx.theme.palette.subtle_pressed
                            : ctx.theme.palette.subtle_hover);
                ctx.canvas.stroke_rounded_rect(pill, chip_radius(),
                                               add_hov
                                                   ? ctx.theme.palette.accent
                                                   : ctx.theme.palette.border,
                                               add_hov ? 1.5f : 1.0f);
                ctx.canvas.draw_text(
                    *l, {pill.x + kChipPadX, pill.y + (pill.h - sz.h) * 0.5f},
                    ctx.theme.palette.text_secondary);
                owner_.hovered_row_geom_.add_button = pill;
                owner_.hovered_row_geom_.add_visible = true;
            }
        }

        // Disc centre Y for receipts. Receipts always overlay the row — they
        // never expand it. Default: centre in the bottom kPadY strip (cursor
        // is its top edge). When reactions are present the block below
        // overrides this to align with the chip strip centre.
        float receipt_disc_cy = cursor - kReceiptSize * 0.5f;

        // ── Bottom chip strip (reactions) ────────────────────────────────────
        // Only created when there are persistent reaction chips to show.
        if (!m.reactions.empty())
        {
            float chip_y = cursor;
            float chip_x = col_x;
            receipt_disc_cy = chip_y + eff_chip_h * 0.5f;
            for (std::size_t ri = 0; ri < m.reactions.size(); ++ri)
            {
                const auto& r = m.reactions[ri];
                constexpr float kChipInnerGap = 4.0f;
                constexpr float kImgPad = 4.0f;
                const bool is_img = r.source != nullptr;

                auto& rc = cache_for(index);
                if (rc.reactions.size() <= ri)
                {
                    rc.reactions.resize(ri + 1);
                }
                auto& rxc = rc.reactions[ri];
                if (rxc.key != r.key || rxc.count != r.count)
                {
                    rxc.key = r.key;
                    rxc.count = r.count;
                    tk::TextStyle cst{};
                    cst.role = tk::FontRole::UiSemibold;
                    rxc.count_layout =
                        ctx.factory.build_text(std::to_string(r.count), cst);
                    if (r.source != nullptr)
                    {
                        rxc.glyph_layout.reset();
                    }
                    else
                    {
                        tk::TextStyle est{};
                        est.role = tk::FontRole::Title;
                        rxc.glyph_layout = ctx.factory.build_text(r.key, est);
                    }
                }
                const auto& count_layout = rxc.count_layout;
                if (!count_layout)
                {
                    if (hovered)
                    {
                        owner_.hovered_row_geom_.chips.push_back({});
                    }
                    continue;
                }
                tk::Size csz = count_layout->measure();

                const tk::TextLayout* emoji_layout = nullptr;
                tk::Size esz{};
                float content_w;
                if (is_img)
                {
                    float img_side = eff_chip_h - kImgPad * 2;
                    content_w = img_side + kChipInnerGap + csz.w;
                }
                else
                {
                    emoji_layout = rxc.glyph_layout.get();
                    if (!emoji_layout)
                    {
                        if (hovered)
                        {
                            owner_.hovered_row_geom_.chips.push_back({});
                        }
                        continue;
                    }
                    esz = emoji_layout->measure();
                    content_w = esz.w + kChipInnerGap + csz.w;
                }
                float w =
                    std::max(content_w + kChipPadX * 2, eff_chip_h + 8.0f);
                tk::Rect pill{chip_x, chip_y, w, eff_chip_h};
                bool chip_hovered =
                    hovered && owner_.hover_target_ == HoverTarget::Chip &&
                    owner_.hover_chip_idx_ == static_cast<int>(ri);
                tk::Color bg = r.reacted_by_me ? ctx.theme.palette.chip_bg_me
                                               : ctx.theme.palette.chip_bg;
                tk::Color border = r.reacted_by_me
                                       ? ctx.theme.palette.chip_border_me
                                       : ctx.theme.palette.chip_border;
                tk::Color text = r.reacted_by_me
                                     ? ctx.theme.palette.chip_text_me
                                     : ctx.theme.palette.chip_text;
                if (chip_hovered)
                {
                    border = ctx.theme.palette.accent;
                }
                ctx.canvas.fill_rounded_rect(pill, eff_chip_r, bg);
                ctx.canvas.stroke_rounded_rect(pill, eff_chip_r, border,
                                               chip_hovered ? 1.5f : 1.0f);

                float left_x = pill.x + kChipPadX;
                float count_y = pill.y + (pill.h - csz.h) * 0.5f;
                if (is_img)
                {
                    float img_side = eff_chip_h - kImgPad * 2;
                    tk::Rect img_dst{left_x, pill.y + kImgPad, img_side,
                                     img_side};
                    const tk::Image* img =
                        (owner_.image_provider_ && r.source)
                            ? owner_.image_provider_(r.source->fetch_token())
                            : nullptr;
                    if (img)
                    {
                        ctx.canvas.draw_image(*img, img_dst);
                    }
                    else
                    {
                        ctx.canvas.fill_rect(img_dst,
                                             tk::Color{0xCC, 0xCC, 0xCC});
                    }
                    ctx.canvas.draw_text(
                        *count_layout,
                        {left_x + img_side + kChipInnerGap, count_y}, text);
                }
                else
                {
                    // Centre the emoji by ascent() — the visual fill height
                    // of the colour-emoji glyph within the layout box.
                    // Centering by the full line-height leaves emoji high
                    // because the descent region is empty for these fonts.
                    float emoji_y =
                        pill.y + (pill.h - emoji_layout->ascent()) * 0.5f;
                    ctx.canvas.draw_text(*emoji_layout, {left_x, emoji_y},
                                         text);
                    ctx.canvas.draw_text(
                        *count_layout,
                        {left_x + esz.w + kChipInnerGap, count_y}, text);
                }
                if (hovered)
                {
                    owner_.hovered_row_geom_.chips.push_back(pill);
                }
                chip_x += w + chip_gap();
            }

            // Trailing "+" pseudo-chip: only painted while the row is
            // hovered. Reads as a discoverable affordance, not a real
            // reaction — muted background, subtle border.
            if (hovered)
            {
                static_cache_.ensure(ctx.factory);
                if (const auto* layout = static_cache_.plus.get())
                {
                    tk::Size sz = layout->measure();
                    float w = std::max(sz.w + kChipPadX * 2, eff_chip_h + 8.0f);
                    tk::Rect pill{chip_x, chip_y, w, eff_chip_h};
                    bool add_hovered =
                        owner_.hover_target_ == HoverTarget::AddButton;
                    tk::Color bg = add_hovered
                                       ? ctx.theme.palette.subtle_pressed
                                       : ctx.theme.palette.subtle_hover;
                    tk::Color border = add_hovered ? ctx.theme.palette.accent
                                                   : ctx.theme.palette.border;
                    ctx.canvas.fill_rounded_rect(pill, eff_chip_r, bg);
                    ctx.canvas.stroke_rounded_rect(pill, eff_chip_r, border,
                                                   add_hovered ? 1.5f : 1.0f);
                    ctx.canvas.draw_text(
                        *layout,
                        {pill.x + kChipPadX, pill.y + (pill.h - sz.h) * 0.5f},
                        ctx.theme.palette.text_secondary);
                    owner_.hovered_row_geom_.add_button = pill;
                    owner_.hovered_row_geom_.add_visible = true;
                    chip_x += w + chip_gap();
                }

                auto paint_strip_btn =
                    [&](const tk::TextLayout* l, tk::Rect& geom_out, bool last)
                {
                    if (!l)
                    {
                        return;
                    }
                    tk::Size sz = l->measure();
                    float w =
                        std::max(sz.w + kReplyBtnPadX * 2, eff_chip_h + 4.0f);
                    tk::Rect pill{chip_x, chip_y, w, eff_chip_h};
                    ctx.canvas.fill_rounded_rect(
                        pill, eff_chip_r, ctx.theme.palette.subtle_hover);
                    ctx.canvas.stroke_rounded_rect(
                        pill, eff_chip_r, ctx.theme.palette.border, 1.0f);
                    ctx.canvas.draw_text(*l,
                                         {pill.x + kReplyBtnPadX,
                                          pill.y + (pill.h - sz.h) * 0.5f},
                                         ctx.theme.palette.text_secondary);
                    geom_out = pill;
                    if (!last)
                    {
                        chip_x += w + chip_gap();
                    }
                };

                paint_strip_btn(static_cache_.reply.get(),
                                owner_.hovered_row_geom_.reply_button, false);
                if (m.is_own && m.kind == MessageRowData::Kind::Text)
                {
                    paint_strip_btn(static_cache_.edit.get(),
                                    owner_.hovered_row_geom_.edit_button,
                                    false);
                }
                if (m.is_own && m.kind != MessageRowData::Kind::Redacted)
                {
                    paint_strip_btn(static_cache_.trash.get(),
                                    owner_.hovered_row_geom_.delete_button,
                                    true);
                }
            }
        }

        // ── Read-receipt cluster ──────────────────────────────────────────────
        // Painted at the bottom-right of the body block — no extra row height.
        // Discs are centred at the bottom edge of the body so they sit at the
        // same vertical level as the last line of text.
        if (!m.read_receipts.empty())
        {
            const std::size_t total = m.read_receipts.size();
            const std::size_t visible = std::min(total, kReceiptCap);
            const std::size_t overflow = total - visible;
            const float disc_cy = receipt_disc_cy;

            float right_edge = bounds.x + bounds.w - kPadX;
            for (std::size_t i = 0; i < visible; ++i)
            {
                // m.read_receipts is oldest-first; paint the last
                // `visible` of them right-to-left so the most-recent
                // receipt sits on top of the stack.
                const auto& rr = m.read_receipts[total - 1 - i];
                float cx = right_edge - kReceiptSize * 0.5f -
                           static_cast<float>(i) * kReceiptStride;
                tk::Point centre{cx, disc_cy};
                if (hovered)
                {
                    owner_.hovered_row_geom_.receipt_discs.push_back(
                        {centre.x - kReceiptSize * 0.5f,
                         centre.y - kReceiptSize * 0.5f, kReceiptSize,
                         kReceiptSize});
                }
                const tk::Image* img = nullptr;
                if (owner_.avatar_provider_ && !rr.avatar_url.empty())
                {
                    img = owner_.avatar_provider_(rr.avatar_url);
                }
                if (img)
                {
                    ctx.canvas.draw_circle_image(*img, centre, kReceiptSize);
                }
                else
                {
                    ctx.canvas.draw_initials_circle(
                        rr.display_name.empty() ? rr.user_id : rr.display_name,
                        centre, kReceiptSize,
                        ctx.theme.palette.avatar_initials_bg,
                        ctx.theme.palette.avatar_initials_text);
                }
            }

            // "+N" overflow pill — anchored just to the left of the
            // leftmost disc in the cluster.
            if (overflow > 0)
            {
                tk::TextStyle st{};
                st.role = tk::FontRole::UiSemibold;
                auto layout = ctx.factory.build_text(
                    std::string("+") + std::to_string(overflow), st);
                if (layout)
                {
                    tk::Size sz = layout->measure();
                    float pill_w = sz.w + kChipPadX;
                    float pill_h = kReceiptSize;
                    float cluster_left =
                        right_edge -
                        (kReceiptSize +
                         static_cast<float>(visible - 1) * kReceiptStride);
                    tk::Rect pill{
                        cluster_left - kReceiptOverflowGap - pill_w,
                        disc_cy - pill_h * 0.5f,
                        pill_w,
                        pill_h,
                    };
                    ctx.canvas.fill_rounded_rect(pill, pill_h * 0.5f,
                                                 ctx.theme.palette.chip_bg);
                    ctx.canvas.draw_text(*layout,
                                         {pill.x + (pill_w - sz.w) * 0.5f,
                                          pill.y + (pill_h - sz.h) * 0.5f},
                                         ctx.theme.palette.text_secondary);
                }
            }
        }

        // ── Pending send indicator (own messages only) ──────────────────────
        // Painted at the bottom-right of the message body, below it, in the
        // same zone as read receipts. Never expands the row — stays inside
        // the existing bounds. Text uses FontRole::Small.
        //   None / just_sent  → "✓"  (accent colour)
        //   Sending           → "◷"  (text_muted)
        //   Failed recoverable → "⚠" + "Retry" (red + accent, Retry rect stored)
        //   Failed unrecoverable → "⚠" + "✕"   (red + red, abort rect stored)
        if (m.is_own)
        {
            using PS = MessageRowData::PendingState;
            const bool show_check = m.just_sent;
            const bool show_clock =
                (m.pending_state == PS::Sending && !m.just_sent);
            const bool show_failed = (m.pending_state == PS::Failed);
            if (show_check || show_clock || show_failed)
            {
                constexpr float kPendingInsetX = 4.0f;
                constexpr float kPendingGap = 4.0f; // gap between ⚠ and button

                float right_edge = bounds.x + bounds.w - kPadX;
                // Reserve space for receipts if they are present (same
                // logic as the hover-button path above).
                if (!m.read_receipts.empty())
                {
                    const std::size_t n =
                        std::min(m.read_receipts.size(), kReceiptCap);
                    float cluster_w = kReceiptSize + static_cast<float>(n - 1) *
                                                         kReceiptStride;
                    right_edge -= cluster_w + chip_gap();
                }
                right_edge -= kPendingInsetX;

                tk::TextStyle small_st{};
                small_st.role = tk::FontRole::Small;
                small_st.wrap = false;

                if (show_check)
                {
                    // ✓
                    auto lo = ctx.factory.build_text("\xE2\x9C\x93", small_st);
                    if (lo)
                    {
                        tk::Size sz = lo->measure();
                        float tx = right_edge - sz.w;
                        float ty = cursor - sz.h;
                        ctx.canvas.draw_text(*lo, {tx, ty},
                                             ctx.theme.palette.accent);
                    }
                }
                else if (show_clock)
                {
                    // ◷
                    auto lo = ctx.factory.build_text("\xE2\x97\xB7", small_st);
                    if (lo)
                    {
                        tk::Size sz = lo->measure();
                        float tx = right_edge - sz.w;
                        float ty = cursor - sz.h;
                        ctx.canvas.draw_text(*lo, {tx, ty},
                                             ctx.theme.palette.text_muted);
                    }
                }
                else if (show_failed)
                {
                    // ⚠
                    auto warn_lo =
                        ctx.factory.build_text("\xE2\x9A\xA0", small_st);
                    const tk::Color kRed = tk::Color::rgb(0xE53935);
                    float cur_right = right_edge;

                    if (m.pending_recoverable)
                    {
                        // "Retry" button
                        auto btn_lo = ctx.factory.build_text("Retry", small_st);
                        if (btn_lo)
                        {
                            tk::Size bsz = btn_lo->measure();
                            float bx = cur_right - bsz.w;
                            float by = cursor - bsz.h;
                            tk::Rect btn_rect{bx, by, bsz.w, bsz.h};
                            ctx.canvas.draw_text(*btn_lo, {bx, by},
                                                 ctx.theme.palette.accent);
                            if (hovered)
                            {
                                owner_.hovered_row_geom_.retry_button =
                                    btn_rect;
                            }
                            cur_right = bx;
                        }
                    }
                    else
                    {
                        // ✕ abort button
                        auto btn_lo =
                            ctx.factory.build_text("\xE2\x9C\x95", small_st);
                        if (btn_lo)
                        {
                            tk::Size bsz = btn_lo->measure();
                            float bx = cur_right - bsz.w;
                            float by = cursor - bsz.h;
                            tk::Rect btn_rect{bx, by, bsz.w, bsz.h};
                            ctx.canvas.draw_text(*btn_lo, {bx, by}, kRed);
                            if (hovered)
                            {
                                owner_.hovered_row_geom_.abort_button =
                                    btn_rect;
                            }
                            cur_right = bx;
                        }
                    }

                    if (warn_lo)
                    {
                        tk::Size wsz = warn_lo->measure();
                        float wx = cur_right - kPendingGap - wsz.w;
                        float wy = cursor - wsz.h;
                        ctx.canvas.draw_text(*warn_lo, {wx, wy}, kRed);
                    }
                }
            }
        }

        // Hover-only timestamp, painted under the sender avatar (left
        // column). The column from `y = kPadY + kAvatarSize` downward is
        // empty for every row — body text wraps in the right column — so
        // this can sit hugging the row's bottom padding without colliding
        // with anything else the strip painted.
        if (hovered)
        {
            std::string ts = format_hhmm(m.timestamp_ms);
            if (!ts.empty())
            {
                tk::TextStyle st{};
                st.role = tk::FontRole::Timestamp;
                auto layout = ctx.factory.build_text(ts, st);
                if (layout)
                {
                    tk::Size sz = layout->measure();
                    float tx = avatar_cx - sz.w * 0.5f;
                    float ty = bounds.y + bounds.h - kPadY - sz.h;
                    ctx.canvas.draw_text(*layout, {tx, ty},
                                         ctx.theme.palette.text_muted);
                }
            }
        }
    }

    // Cache management — called from MessageListView mutation methods.
    void clear_layout_cache()
    {
        layout_cache_.clear();
        static_cache_.clear();
    }
    void insert_layout_cache_at(std::size_t index)
    {
        if (index <= layout_cache_.size())
        {
            layout_cache_.emplace(layout_cache_.begin() + index);
        }
    }
    void erase_layout_cache_at(std::size_t index)
    {
        if (index < layout_cache_.size())
        {
            layout_cache_.erase(layout_cache_.begin() + index);
        }
    }
    void invalidate_layout_cache_at(std::size_t index)
    {
        if (index < layout_cache_.size())
        {
            layout_cache_[index].clear();
        }
    }

private:
    // ── Virtual timeline item paint helpers ──────────────────────────────────

    void paint_day_separator(const MessageRowData& m, tk::PaintCtx& ctx,
                             tk::Rect bounds) const
    {
        std::string label = format_day_label(m.timestamp_ms);
        if (label.empty())
        {
            return;
        }
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text(label, st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        constexpr float kLabelPadX = 8.0f;
        float cx = bounds.x + bounds.w * 0.5f;
        float cy = bounds.y + kDaySepH * 0.5f;
        float label_l = cx - sz.w * 0.5f - kLabelPadX;
        float label_r = cx + sz.w * 0.5f + kLabelPadX;
        float line_y = std::round(cy);
        if (label_l > bounds.x + kPadX)
        {
            ctx.canvas.fill_rect(
                {bounds.x + kPadX, line_y, label_l - bounds.x - kPadX, 1.0f},
                ctx.theme.palette.border);
        }
        if (label_r < bounds.x + bounds.w - kPadX)
        {
            ctx.canvas.fill_rect(
                {label_r, line_y, bounds.x + bounds.w - kPadX - label_r, 1.0f},
                ctx.theme.palette.border);
        }
        ctx.canvas.draw_text(*lo, {cx - sz.w * 0.5f, cy - sz.h * 0.5f},
                             ctx.theme.palette.text_muted);
    }

    void paint_read_marker(tk::PaintCtx& ctx, tk::Rect bounds) const
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text("New messages", st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        constexpr float kLabelPadX = 8.0f;
        float cx = bounds.x + bounds.w * 0.5f;
        float cy = bounds.y + kReadMarkerH * 0.5f;
        float lx = cx - sz.w * 0.5f - kLabelPadX;
        float rx = cx + sz.w * 0.5f + kLabelPadX;
        float ly = std::round(cy);
        if (lx > bounds.x + kPadX)
        {
            ctx.canvas.fill_rect(
                {bounds.x + kPadX, ly, lx - bounds.x - kPadX, 1.0f},
                ctx.theme.palette.accent);
        }
        if (rx < bounds.x + bounds.w - kPadX)
        {
            ctx.canvas.fill_rect(
                {rx, ly, bounds.x + bounds.w - kPadX - rx, 1.0f},
                ctx.theme.palette.accent);
        }
        ctx.canvas.draw_text(*lo, {cx - sz.w * 0.5f, cy - sz.h * 0.5f},
                             ctx.theme.palette.accent);
    }

    void paint_timeline_start(tk::PaintCtx& ctx, tk::Rect bounds) const
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto lo = ctx.factory.build_text("Start of conversation", st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        float cx = bounds.x + bounds.w * 0.5f;
        float cy = bounds.y + kTimelineStartH * 0.5f;
        ctx.canvas.draw_text(*lo, {cx - sz.w * 0.5f, cy - sz.h * 0.5f},
                             ctx.theme.palette.text_muted);
    }

    // Trailing synthetic "X is typing…" row. Left-aligned + ellipsized
    // muted text, matching the look the old RoomView strip had.
    void paint_typing_row(tk::PaintCtx& ctx, tk::Rect bounds) const
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, bounds.w - kPadX * 2);
        auto lo = ctx.factory.build_text(owner_.typing_text_, st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        ctx.canvas.draw_text(
            *lo, {bounds.x + kPadX, bounds.y + (kTypingRowH - sz.h) * 0.5f},
            ctx.theme.palette.text_muted);
    }

    // ── Message row paint helpers ─────────────────────────────────────────────

    float measure_body_block_height(const MessageRowData& m, tk::LayoutCtx& ctx,
                                    float col_w) const
    {
        float quote_h = m.has_reply() ? (kQuoteBlockH + kQuoteGapAfter) : 0.0f;
        switch (m.kind)
        {
        case MessageRowData::Kind::Text:
        case MessageRowData::Kind::Notice:
        case MessageRowData::Kind::Unhandled:
        {
            float th = measure_body_text(m, ctx, col_w);
            float badge_h = 0.0f;
            if (m.is_edited)
            {
                badge_h = kEditedBadgeGap +
                          measure_text_height("(edited)", ctx, col_w);
            }
            float preview_h = 0.0f;
            if (!m.first_url.empty() && owner_.preview_provider_)
            {
                const auto* p = owner_.preview_provider_(m.first_url);
                if (p && p->has_content())
                {
                    preview_h = kPreviewCardGapTop + kPreviewCardH;
                }
            }
            return quote_h + th + badge_h + preview_h;
        }
        case MessageRowData::Kind::Redacted:
            return quote_h +
                   measure_text_height(
                       m.body.empty() ? std::string("(empty message)") : m.body,
                       ctx, col_w);

        case MessageRowData::Kind::Image:
        {
            float max_w = std::min(col_w, kImageMaxW);
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            const std::string img_key = look ? look->fetch_token() : std::string{};
            const tk::Image* img =
                (owner_.image_provider_ && !img_key.empty())
                    ? owner_.image_provider_(img_key)
                    : nullptr;
            tk::Size sz =
                (img && img->width() > 0 && img->height() > 0)
                    ? fit_media(img->width(), img->height(), max_w, kImageMaxH)
                    : fit_media(m.media_w, m.media_h, max_w, kImageMaxH);
            float h = sz.h;
            if (m.has_filename_caption && !m.body.empty())
            {
                h += 4.0f + measure_text_height(m.body, ctx, col_w);
            }
            return quote_h + h;
        }
        case MessageRowData::Kind::Sticker:
        {
            float max_side = std::min(col_w, kStickerSize);
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            const std::string sticker_key = look ? look->fetch_token() : std::string{};
            const tk::Image* sticker_img =
                (owner_.image_provider_ && !sticker_key.empty())
                    ? owner_.image_provider_(sticker_key)
                    : nullptr;
            tk::Size sz;
            if (sticker_img && sticker_img->width() > 0 &&
                sticker_img->height() > 0)
            {
                sz = fit_media(sticker_img->width(), sticker_img->height(),
                               max_side, max_side);
            }
            else if (m.media_w > 0 && m.media_h > 0)
            {
                sz = fit_media(m.media_w, m.media_h, max_side, max_side);
            }
            else
            {
                sz = {max_side, max_side};
            }
            return quote_h + sz.h;
        }
        case MessageRowData::Kind::File:
            return quote_h + kFileCardH;
        case MessageRowData::Kind::Audio:
            return quote_h + kAudioCardH;
        case MessageRowData::Kind::Voice:
            return quote_h + kVoiceCardH;
        case MessageRowData::Kind::Video:
        {
            int vw = m.media_w, vh = m.media_h;
            tk::Size sz =
                (vw > 0 && vh > 0)
                    ? fit_media(vw, vh, std::min(col_w, kImageMaxW), kImageMaxH)
                    : tk::Size{std::min(col_w, kImageMaxW),
                               std::min(col_w, kImageMaxW) * 9.0f / 16.0f};
            float h = sz.h;
            if (m.has_filename_caption && !m.body.empty())
            {
                h += 4.0f + measure_text_height(m.body, ctx, col_w);
            }
            return quote_h + h;
        }
        case MessageRowData::Kind::Emote:
        {
            bool revealed = owner_.revealed_spoilers_.count(m.event_id) > 0;
            auto spans = build_emote_spans(m, revealed);
            float th = 0.0f;
            if (!spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(spans, body_style(col_w));
                if (layout)
                {
                    th = layout->measure().h;
                }
            }
            float badge_h =
                m.is_edited ? kEditedBadgeGap +
                                  measure_text_height("(edited)", ctx, col_w)
                            : 0.0f;
            float preview_h = 0.0f;
            if (!m.first_url.empty() && owner_.preview_provider_)
            {
                const auto* p = owner_.preview_provider_(m.first_url);
                if (p && p->has_content())
                {
                    preview_h = kPreviewCardGapTop + kPreviewCardH;
                }
            }
            return quote_h + th + badge_h + preview_h;
        }
        case MessageRowData::Kind::Location:
        {
            constexpr float kMapRowH = 240.0f;
            float desc_h = 0.0f;
            if (!m.location_description.empty())
            {
                desc_h =
                    measure_text_height(m.location_description, ctx, col_w) +
                    kPadY;
            }
            return quote_h + kMapRowH + desc_h;
        }
        // Virtual items are handled before this function is called.
        case MessageRowData::Kind::DaySeparator:
        case MessageRowData::Kind::ReadMarker:
        case MessageRowData::Kind::TimelineStart:
            return 0.0f;
        }
        return quote_h;
    }

    float paint_body_block(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                           float y, float col_w) const
    {
        if (m.has_reply())
        {
            y = paint_quote_block(m, ctx, x, y, col_w);
            y += kQuoteGapAfter;
        }
        switch (m.kind)
        {
        case MessageRowData::Kind::Text:
        case MessageRowData::Kind::Unhandled:
        {
            float h = paint_body_text(m, ctx, x, y, col_w,
                                      ctx.theme.palette.text_primary);
            float end_y = y + h;
            // "(edited)" badge on a new inline line below the body.
            if (m.is_edited)
            {
                tk::TextStyle st{};
                st.role = tk::FontRole::Small;
                st.trim = tk::TextTrim::Ellipsis;
                st.max_width = col_w;
                auto lo = ctx.factory.build_text("(edited)", st);
                if (lo)
                {
                    ctx.canvas.draw_text(*lo, {x, end_y + kEditedBadgeGap},
                                         ctx.theme.palette.text_muted);
                    end_y += kEditedBadgeGap + lo->measure().h;
                }
            }
            if (!m.first_url.empty() && owner_.preview_provider_)
            {
                const auto* p = owner_.preview_provider_(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    paint_preview_card_(m, *p, ctx, x, end_y, col_w);
                    end_y += kPreviewCardH;
                }
            }
            return end_y;
        }
        case MessageRowData::Kind::Notice:
        {
            float h = paint_body_text(m, ctx, x, y, col_w,
                                      ctx.theme.palette.text_muted);
            float end_y = y + h;
            if (m.is_edited)
            {
                tk::TextStyle st{};
                st.role = tk::FontRole::Small;
                st.trim = tk::TextTrim::Ellipsis;
                st.max_width = col_w;
                auto lo = ctx.factory.build_text("(edited)", st);
                if (lo)
                {
                    ctx.canvas.draw_text(*lo, {x, end_y + kEditedBadgeGap},
                                         ctx.theme.palette.text_muted);
                    end_y += kEditedBadgeGap + lo->measure().h;
                }
            }
            if (!m.first_url.empty() && owner_.preview_provider_)
            {
                const auto* p = owner_.preview_provider_(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    paint_preview_card_(m, *p, ctx, x, end_y, col_w);
                    end_y += kPreviewCardH;
                }
            }
            return end_y;
        }
        case MessageRowData::Kind::Emote:
        {
            bool revealed = owner_.revealed_spoilers_.count(m.event_id) > 0;
            auto spans = build_emote_spans(m, revealed);
            float end_y = y;
            if (!spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(spans, body_style(col_w));
                if (layout)
                {
                    ctx.canvas.draw_text(*layout, {x, y},
                                         ctx.theme.palette.text_primary);
                    end_y = y + layout->measure().h;
                    owner_.link_layout_cache_[m.event_id] = {
                        std::move(layout), {x, y}, spans_to_plain(spans)};
                }
            }
            if (m.is_edited)
            {
                tk::TextStyle st{};
                st.role = tk::FontRole::Small;
                st.trim = tk::TextTrim::Ellipsis;
                st.max_width = col_w;
                auto lo = ctx.factory.build_text("(edited)", st);
                if (lo)
                {
                    ctx.canvas.draw_text(*lo, {x, end_y + kEditedBadgeGap},
                                         ctx.theme.palette.text_muted);
                    end_y += kEditedBadgeGap + lo->measure().h;
                }
            }
            if (!m.first_url.empty() && owner_.preview_provider_)
            {
                const auto* p = owner_.preview_provider_(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    paint_preview_card_(m, *p, ctx, x, end_y, col_w);
                    end_y += kPreviewCardH;
                }
            }
            return end_y;
        }
        case MessageRowData::Kind::Redacted:
        {
            float h = paint_wrapped_text("Message deleted", ctx, x, y, col_w,
                                         ctx.theme.palette.text_muted);
            return y + h;
        }
        case MessageRowData::Kind::Image:
        {
            float max_w = std::min(col_w, kImageMaxW);
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            const std::string img_key = look ? look->fetch_token() : std::string{};
            const tk::Image* img =
                (owner_.image_provider_ && !img_key.empty())
                    ? owner_.image_provider_(img_key)
                    : nullptr;
            tk::Size sz =
                (img && img->width() > 0 && img->height() > 0)
                    ? fit_media(img->width(), img->height(), max_w, kImageMaxH)
                    : fit_media(m.media_w, m.media_h, max_w, kImageMaxH);
            tk::Rect r{x, y, sz.w, sz.h};
            paint_inline_media(m, ctx, r);
            // GIF badge for animated images (MSC4230).
            if (m.image_animated)
            {
                tk::TextStyle ts{};
                ts.role = tk::FontRole::Timestamp;
                auto lo = ctx.factory.build_text("GIF", ts);
                if (lo)
                {
                    tk::Size lsz = lo->measure();
                    constexpr float kBadgePadX = 6.0f, kBadgePadY = 3.0f;
                    float bx = r.x + r.w - lsz.w - kBadgePadX * 2 - 4.0f;
                    float by = r.y + r.h - lsz.h - kBadgePadY * 2 - 4.0f;
                    tk::Rect badge{bx, by, lsz.w + kBadgePadX * 2,
                                   lsz.h + kBadgePadY * 2};
                    ctx.canvas.fill_rounded_rect(badge, 4.0f,
                                                 tk::Color{0, 0, 0, 140});
                    ctx.canvas.draw_text(*lo,
                                         {bx + kBadgePadX, by + kBadgePadY},
                                         tk::Color{255, 255, 255, 230});
                }
            }
            if (!m.event_id.empty())
            {
                // Prefer the ORIGINAL (sender-supplied) dimensions so
                // the viewer opens at true 1:1 of the full image, not
                // the downscaled inline thumbnail. Fall back to the
                // decoded size only when metadata is absent.
                int iw = (m.media_w > 0) ? m.media_w : (img ? img->width() : 0);
                int ih =
                    (m.media_h > 0) ? m.media_h : (img ? img->height() : 0);
                owner_.image_geom_[m.event_id] = MessageListView::ImageHit{
                    m.event_id, m.source, m.thumbnail, m.body, iw, ih, r};
            }
            float cursor = y + sz.h;
            if (m.has_filename_caption && !m.body.empty())
            {
                cursor += 4.0f;
                float ch = paint_wrapped_text(m.body, ctx, x, cursor, col_w,
                                              ctx.theme.palette.text_primary);
                cursor += ch;
            }
            return cursor;
        }
        case MessageRowData::Kind::Sticker:
        {
            float max_side = std::min(col_w, kStickerSize);
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            const std::string sticker_key = look ? look->fetch_token() : std::string{};
            const tk::Image* sticker_img =
                (owner_.image_provider_ && !sticker_key.empty())
                    ? owner_.image_provider_(sticker_key)
                    : nullptr;
            tk::Size sz;
            if (sticker_img && sticker_img->width() > 0 &&
                sticker_img->height() > 0)
            {
                sz = fit_media(sticker_img->width(), sticker_img->height(),
                               max_side, max_side);
            }
            else if (m.media_w > 0 && m.media_h > 0)
            {
                sz = fit_media(m.media_w, m.media_h, max_side, max_side);
            }
            else
            {
                sz = {max_side, max_side};
            }
            tk::Rect r{x, y, sz.w, sz.h};
            paint_inline_media(m, ctx, r);
            if (!m.event_id.empty())
            {
                owner_.sticker_geom_[m.event_id] = MessageListView::StickerHit{
                    m.event_id, m.source, m.body, m.sticker_info_json, r};
                int iw = (sticker_img && sticker_img->width() > 0)
                             ? sticker_img->width()
                             : m.media_w;
                int ih = (sticker_img && sticker_img->height() > 0)
                             ? sticker_img->height()
                             : m.media_h;
                owner_.image_geom_[m.event_id] = MessageListView::ImageHit{
                    m.event_id, m.source, m.thumbnail, m.body, iw, ih, r};
            }
            return y + sz.h;
        }
        case MessageRowData::Kind::File:
        {
            float card_w = std::min(kFileCardW, col_w);
            tk::Rect r{x, y, card_w, kFileCardH};
            paint_file_card(m, ctx, r);
            if (!m.event_id.empty())
            {
                owner_.file_geom_[m.event_id] =
                    MessageListView::FileHit{m.event_id, m.file_source,
                                              m.file_name, m.file_size, r};
            }
            return y + kFileCardH;
        }
        case MessageRowData::Kind::Audio:
        {
            float card_w = std::min(kAudioCardW, col_w);
            tk::Rect r{x, y, card_w, kAudioCardH};
            paint_audio_card(m, ctx, r);
            return y + kAudioCardH;
        }
        case MessageRowData::Kind::Voice:
        {
            float card_w = std::min(kVoiceCardW, col_w);
            tk::Rect r{x, y, card_w, kVoiceCardH};
            paint_voice_card(m, ctx, r);
            return y + kVoiceCardH;
        }
        case MessageRowData::Kind::Video:
        {
            int vw = m.media_w, vh = m.media_h;
            tk::Size sz =
                (vw > 0 && vh > 0)
                    ? fit_media(vw, vh, std::min(col_w, kImageMaxW), kImageMaxH)
                    : tk::Size{std::min(col_w, kImageMaxW),
                               std::min(col_w, kImageMaxW) * 9.0f / 16.0f};
            tk::Rect r{x, y, sz.w, sz.h};
            paint_video_card(m, ctx, r);
            if (!m.event_id.empty())
            {
                owner_.video_geom_[m.event_id] =
                    MessageListView::VideoHit{m.event_id,
                                              m.source,
                                              m.thumbnail,
                                              m.video_mime,
                                              m.media_w,
                                              m.media_h,
                                              m.duration_ms,
                                              m.video_autoplay,
                                              m.video_loop,
                                              m.video_no_audio,
                                              m.video_hide_controls,
                                              m.video_gif,
                                              r};
            }
            float cursor = y + sz.h;
            if (m.has_filename_caption && !m.body.empty())
            {
                cursor += 4.0f;
                float ch = paint_wrapped_text(m.body, ctx, x, cursor, col_w,
                                              ctx.theme.palette.text_primary);
                cursor += ch;
            }
            return cursor;
        }
        case MessageRowData::Kind::Location:
        {
            constexpr float kMapRowH = 240.0f;
            tk::Rect map_rect{x, y, std::min(col_w, kImageMaxW), kMapRowH};
            paint_location_map(m, ctx, map_rect);
            return y + kMapRowH;
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
    float paint_quote_block(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                            float y, float col_w) const
    {
        tk::Rect card{x, y, col_w, kQuoteBlockH};

        // Record world-coord rect so on_pointer_down can hit-test it.
        owner_.quote_block_geom_[m.event_id] = card;

        // Background + border
        ctx.canvas.fill_rounded_rect(card, 4.0f,
                                     ctx.theme.palette.subtle_hover);
        ctx.canvas.stroke_rounded_rect(card, 4.0f, ctx.theme.palette.border,
                                       1.0f);

        // Left accent stripe (3 px, full height of the card, rounded left edge)
        tk::Rect stripe{x, y, kQuoteAccentW, kQuoteBlockH};
        ctx.canvas.fill_rounded_rect(stripe, 4.0f, ctx.theme.palette.accent);

        // Text column starts to the right of the stripe + gap
        float tx = x + kQuoteAccentW + kQuotePadX;
        float tw = std::max(0.0f, col_w - kQuoteAccentW - kQuotePadX * 2);

        // Build both text layouts before drawing so we can measure their
        // actual heights and vertically centre the pair within the card.
        // When the replied-to event isn't in the local timeline cache the
        // SDK sends empty sender_name + body — fall back to a single muted
        // placeholder line instead of showing the raw event id.
        const bool unresolved = m.in_reply_to_sender_name.empty();
        const std::string sname = unresolved ? std::string{}
                                             : m.in_reply_to_sender_name;
        const std::string sbody =
            unresolved
                ? std::string("Original message unavailable")
                : m.in_reply_to_body;

        tk::TextStyle name_st{};
        name_st.role = tk::FontRole::UiSemibold;
        name_st.trim = tk::TextTrim::Ellipsis;
        name_st.max_width = tw;
        auto name_lo =
            sname.empty() ? nullptr : ctx.factory.build_text(sname, name_st);

        tk::TextStyle body_st{};
        body_st.role = tk::FontRole::Body;
        body_st.trim = tk::TextTrim::Ellipsis;
        body_st.max_width = tw;
        auto body_lo =
            sbody.empty() ? nullptr : ctx.factory.build_text(sbody, body_st);

        constexpr float kLineGap = 2.0f;
        float name_h = name_lo ? name_lo->measure().h : 0.0f;
        float body_h = body_lo ? body_lo->measure().h : 0.0f;
        float total_h = name_h + (body_h > 0.0f ? kLineGap + body_h : 0.0f);
        float text_y = y + (kQuoteBlockH - total_h) * 0.5f;

        if (name_lo)
        {
            ctx.canvas.draw_text(*name_lo, {tx, text_y},
                                 ctx.theme.palette.text_secondary);
        }
        if (body_lo)
        {
            ctx.canvas.draw_text(*body_lo, {tx, text_y + name_h + kLineGap},
                                 ctx.theme.palette.text_muted);
        }

        return y + kQuoteBlockH;
    }

    void paint_preview_card_(const MessageRowData& m, const UrlPreviewData& p,
                             tk::PaintCtx& ctx, float x, float y,
                             float col_w) const
    {
        float card_w = std::min(col_w, kPreviewCardW);
        tk::Rect card{x, y, card_w, kPreviewCardH};

        ctx.canvas.fill_rounded_rect(card, 8.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(card, 8.0f, ctx.theme.palette.border,
                                       1.0f);

        // Record world-coord rect for click-to-open hit-test.
        owner_.preview_card_geom_[m.event_id] = {m.first_url, card};

        float thumb_right = 0.0f;
        if (!p.image_mxc.empty() && owner_.image_provider_)
        {
            const tk::Image* img = owner_.image_provider_(p.image_mxc);
            float tx = x + kPreviewCardPad;
            float ty = y + (kPreviewCardH - kPreviewThumbSide) * 0.5f;
            tk::Rect thumb{tx, ty, kPreviewThumbSide, kPreviewThumbSide};
            if (img)
            {
                ctx.canvas.draw_image(*img, thumb);
            }
            else
            {
                ctx.canvas.fill_rounded_rect(thumb, 4.0f,
                                             ctx.theme.palette.border);
            }
            thumb_right = tx + kPreviewThumbSide + kPreviewCardPad;
        }
        else
        {
            thumb_right = x + kPreviewCardPad;
        }

        float text_x = thumb_right;
        float text_w =
            std::max(0.0f, card.x + card.w - text_x - kPreviewCardPad);

        float text_y = y + kPreviewCardPad;

        if (!p.title.empty())
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::UiSemibold;
            st.trim = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(p.title, st);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {text_x, text_y},
                                     ctx.theme.palette.text_primary);
                text_y += lo->measure().h + 2.0f;
            }
        }
        if (!p.description.empty())
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            st.trim = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(p.description, st);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {text_x, text_y},
                                     ctx.theme.palette.text_secondary);
                text_y += lo->measure().h + 2.0f;
            }
        }
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Small;
            st.trim = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lo = ctx.factory.build_text(m.first_url, st);
            if (lo)
            {
                ctx.canvas.draw_text(*lo, {text_x, text_y},
                                     ctx.theme.palette.text_muted);
            }
        }
    }

    // Returns true when every non-whitespace character in `utf8` is a Unicode
    // emoji codepoint (including ZWJ sequences, skin-tone modifiers, variation
    // selectors, regional indicators, and keycap sequences). Used to pick the
    // 2× BigEmoji font for emoji-only message bodies.
    static bool is_emoji_only(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return false;
        }

        // Decode UTF-8 to codepoints.
        std::vector<uint32_t> cps;
        cps.reserve(utf8.size());
        const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
        const auto* end = p + utf8.size();
        while (p < end)
        {
            uint32_t cp;
            unsigned char c = *p;
            if (c < 0x80)
            {
                cp = c;
                p += 1;
            }
            else if ((c & 0xE0) == 0xC0 && p + 1 < end)
            {
                cp = uint32_t(c & 0x1F) << 6 | (p[1] & 0x3F);
                p += 2;
            }
            else if ((c & 0xF0) == 0xE0 && p + 2 < end)
            {
                cp = uint32_t(c & 0x0F) << 12 | uint32_t(p[1] & 0x3F) << 6 |
                     (p[2] & 0x3F);
                p += 3;
            }
            else if ((c & 0xF8) == 0xF0 && p + 3 < end)
            {
                cp = uint32_t(c & 0x07) << 18 | uint32_t(p[1] & 0x3F) << 12 |
                     uint32_t(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
                p += 4;
            }
            else
            {
                return false; // invalid UTF-8 → not emoji-only
            }
            cps.push_back(cp);
        }

        bool has_emoji = false;
        std::size_t i = 0;
        while (i < cps.size())
        {
            uint32_t cp = cps[i];
            // Transparent combining characters — skip without counting.
            if (cp == 0x200D ||                   // ZWJ
                (cp >= 0xFE00 && cp <= 0xFE0F) || // variation selectors
                cp == 0x20E3 ||                   // combining enclosing keycap
                (cp >= 0x1F3FB && cp <= 0x1F3FF)) // skin-tone modifiers
            {
                ++i;
                continue;
            }
            // Whitespace.
            if (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D)
            {
                ++i;
                continue;
            }
            // Keycap sequence: [0-9*#] followed by optional U+FE0F then U+20E3.
            if ((cp >= 0x30 && cp <= 0x39) || cp == 0x2A || cp == 0x23)
            {
                std::size_t j = i + 1;
                if (j < cps.size() && cps[j] == 0xFE0F)
                {
                    ++j;
                }
                if (j < cps.size() && cps[j] == 0x20E3)
                {
                    has_emoji = true;
                    i = j + 1;
                    continue;
                }
                return false; // bare digit / * / # → not emoji-only
            }
            // Emoji codepoint ranges (mirrors the Twemoji fallback table).
            if (cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
                cp == 0x2122 || cp == 0x2139 ||
                (cp >= 0x2194 && cp <= 0x2199) ||
                (cp >= 0x21A9 && cp <= 0x21AA) ||
                (cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 ||
                cp == 0x23CF || (cp >= 0x23E9 && cp <= 0x23FA) ||
                cp == 0x24C2 || (cp >= 0x25AA && cp <= 0x25FE) ||
                (cp >= 0x2600 && cp <= 0x27BF) ||
                (cp >= 0x2934 && cp <= 0x2935) ||
                (cp >= 0x2B05 && cp <= 0x2B55) || cp == 0x3030 ||
                cp == 0x303D || cp == 0x3297 || cp == 0x3299 || cp == 0x1F004 ||
                cp == 0x1F0CF || (cp >= 0x1F170 && cp <= 0x1F171) ||
                (cp >= 0x1F17E && cp <= 0x1F17F) || cp == 0x1F18E ||
                (cp >= 0x1F191 && cp <= 0x1F19A) ||
                (cp >= 0x1F1E0 && cp <= 0x1F1FF) || // regional indicators
                (cp >= 0x1F201 && cp <= 0x1F251) ||
                (cp >= 0x1F300 && cp <= 0x1F9FF) || // main emoji block
                (cp >= 0x1FA00 && cp <= 0x1FAFF))   // extended
            {
                has_emoji = true;
                ++i;
                continue;
            }
            return false; // non-emoji codepoint
        }
        return has_emoji;
    }

    static tk::TextStyle body_style(float w, bool emoji_only = false)
    {
        tk::TextStyle s{};
        s.role = emoji_only ? tk::FontRole::BigEmoji : tk::FontRole::Body;
        s.wrap = true;
        s.max_width = w;
        return s;
    }

    float measure_text_height(const std::string& text, tk::LayoutCtx& ctx,
                              float w, bool emoji_only = false) const
    {
        if (text.empty())
        {
            return 0;
        }
        auto layout = ctx.factory.build_text(text, body_style(w, emoji_only));
        return layout ? layout->measure().h : 0;
    }

    static bool has_spoilers(const MessageRowData& m)
    {
        return m.formatted_body.find("data-mx-spoiler") != std::string::npos;
    }

    // Pre-process spans for rendering: when unrevealed, replace hidden spoiler
    // text with a bold label so the content is hidden but the span is visible.
    std::vector<tk::TextSpan> prepare_spans(const MessageRowData& m,
                                            bool revealed) const
    {
        auto spans = html_to_spans(m.formatted_body);
        if (!revealed)
        {
            for (auto& sp : spans)
            {
                if (!sp.spoiler)
                {
                    continue;
                }
                sp.text = sp.spoiler_reason.empty()
                              ? "[Spoiler]"
                              : "[Spoiler: " + sp.spoiler_reason + "]";
                sp.bold = true;
                sp.italic = false;
                sp.strikethrough = false;
                sp.url = {};
            }
        }
        return spans;
    }

    // Build italic TextSpan vector for an m.emote row:
    // "* SenderName body_text" with every span forced italic.
    std::vector<tk::TextSpan> build_emote_spans(const MessageRowData& m,
                                                bool revealed) const
    {
        const std::string prefix =
            "* " + (m.sender_name.empty() ? m.sender : m.sender_name) + " ";
        std::vector<tk::TextSpan> result;
        if (!m.formatted_body.empty())
        {
            auto body = prepare_spans(m, revealed);
            tk::TextSpan pfx;
            pfx.text = prefix;
            pfx.italic = true;
            result.push_back(std::move(pfx));
            for (auto& s : body)
            {
                s.italic = true;
                result.push_back(std::move(s));
            }
        }
        else
        {
            tk::TextSpan sp;
            sp.text = prefix + (m.body.empty() ? "(empty message)" : m.body);
            sp.italic = true;
            result.push_back(std::move(sp));
        }
        return result;
    }

    // Measure height for a text message body — uses rich text when
    // formatted_body is present, otherwise falls back to plain text.
    float measure_body_text(const MessageRowData& m, tk::LayoutCtx& ctx,
                            float w) const
    {
        bool eo = is_emoji_only(m.body);
        if (!m.formatted_body.empty())
        {
            bool revealed = owner_.revealed_spoilers_.count(m.event_id) > 0;
            auto spans = prepare_spans(m, revealed);
            if (!spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(spans, body_style(w, eo));
                if (layout)
                {
                    return layout->measure().h;
                }
            }
        }
        // Plain text: autolink bare URLs so they render (and measure) as a
        // rich-text layout — must mirror paint_body_text exactly.
        if (!eo && !m.body.empty())
        {
            auto link_spans = autolink_plain_to_spans(m.body);
            if (!link_spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(link_spans, body_style(w, eo));
                if (layout)
                {
                    return layout->measure().h;
                }
            }
        }
        return measure_text_height(
            m.body.empty() ? std::string("(empty message)") : m.body, ctx, w,
            eo);
    }

    float paint_wrapped_text(const std::string& text, tk::PaintCtx& ctx,
                             float x, float y, float w, tk::Color color,
                             bool emoji_only = false) const
    {
        if (text.empty())
        {
            return 0;
        }
        auto layout = ctx.factory.build_text(text, body_style(w, emoji_only));
        if (!layout)
        {
            return 0;
        }
        ctx.canvas.draw_text(*layout, {x, y}, color);
        return layout->measure().h;
    }

    // Paint a text message body — uses rich text when formatted_body is
    // present, otherwise falls back to plain text.
    float paint_body_text(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                          float y, float w, tk::Color color) const
    {
        bool eo = is_emoji_only(m.body);
        auto ord     = owner_.selection_ordered();
        int  msg_idx = owner_.message_index_of(m.event_id);
        auto draw_with_selection = [&](tk::TextLayout& layout, float ox,
                                       float oy, int plain_len)
        {
            if (ord && msg_idx >= 0 &&
                msg_idx >= ord->lo_idx && msg_idx <= ord->hi_idx)
            {
                int lo_b = (msg_idx == ord->lo_idx) ? ord->lo_byte : 0;
                int hi_b = (msg_idx == ord->hi_idx) ? ord->hi_byte
                                                     : plain_len;
                if (lo_b != hi_b)
                {
                    for (const tk::Rect& r : layout.selection_rects(lo_b, hi_b))
                    {
                        ctx.canvas.fill_rect(
                            {r.x + ox, r.y + oy, r.w, r.h},
                            ctx.theme.palette.selection);
                    }
                }
            }
            ctx.canvas.draw_text(layout, {ox, oy}, color);
        };
        if (!m.formatted_body.empty())
        {
            bool revealed = owner_.revealed_spoilers_.count(m.event_id) > 0;
            auto spans = prepare_spans(m, revealed);
            if (!spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(spans, body_style(w, eo));
                if (layout)
                {
                    std::string plain_spans = spans_to_plain(spans);
                    draw_with_selection(*layout, x, y,
                                        static_cast<int>(plain_spans.size()));
                    float h = layout->measure().h;
                    owner_.link_layout_cache_[m.event_id] = {
                        std::move(layout), {x, y}, std::move(plain_spans)};
                    return h;
                }
            }
        }
        // Plain text: autolink bare URLs into a rich-text layout and cache
        // it so link_at() hit-testing fires on_link_clicked. Must mirror
        // measure_body_text exactly so the row height matches.
        if (!eo && !m.body.empty())
        {
            auto link_spans = autolink_plain_to_spans(m.body);
            if (!link_spans.empty())
            {
                auto layout =
                    ctx.factory.build_rich_text(link_spans, body_style(w, eo));
                if (layout)
                {
                    draw_with_selection(*layout, x, y,
                                        static_cast<int>(m.body.size()));
                    float h = layout->measure().h;
                    owner_.link_layout_cache_[m.event_id] = {
                        std::move(layout), {x, y}, m.body};
                    return h;
                }
            }
        }
        // Plain text with no URLs — build a text layout so selection works.
        {
            std::string plain_text =
                m.body.empty() ? std::string("(empty message)") : m.body;
            auto layout = ctx.factory.build_text(plain_text, body_style(w, eo));
            if (!layout)
                return 0.0f;
            draw_with_selection(*layout, x, y,
                                static_cast<int>(plain_text.size()));
            float h = layout->measure().h;
            owner_.link_layout_cache_[m.event_id] = {
                std::move(layout), {x, y}, std::move(plain_text)};
            return h;
        }
    }

    void paint_inline_media(const MessageRowData& m, tk::PaintCtx& ctx,
                            tk::Rect dst) const
    {
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        const std::string display_key = look ? look->fetch_token() : std::string{};
        const tk::Image* img = nullptr;
        if (owner_.image_provider_ && !display_key.empty())
        {
            img = owner_.image_provider_(display_key);
        }
        if (img)
        {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*img, dst);
            ctx.canvas.pop_clip();
        }
        else
        {
            const tk::Image* bh_img = nullptr;
            if (!m.blurhash.empty() && owner_.image_provider_)
            {
                bh_img = owner_.image_provider_("blurhash::" + m.event_id);
            }
            if (bh_img)
            {
                ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
                ctx.canvas.draw_image(*bh_img, dst);
                ctx.canvas.pop_clip();
            }
            else
            {
                ctx.canvas.fill_rounded_rect(dst, 8.0f,
                                             ctx.theme.palette.chrome_bg);
                ctx.canvas.stroke_rounded_rect(dst, 8.0f,
                                               ctx.theme.palette.border, 1.0f);
            }
        }
    }

    // Paint a slippy-map tile composite for a Kind::Location row.
    // `map_rect` is the bounding box for the map canvas area (pre-clipped).
    void paint_location_map(const MessageRowData& m, tk::PaintCtx& ctx,
                            tk::Rect map_rect) const
    {
        owner_.map_rect_geom_[m.event_id] = map_rect;

        // Round corners on the map card.
        ctx.canvas.push_clip_rounded_rect(map_rect, 8.0f);

        const auto& vp = m.map_viewport;
        tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
        auto tiles = tiles_in_view(vp, map_rect);

        // Draw tiles (or a placeholder if not yet loaded).
        bool any_tile_drawn = false;
        for (const auto& t : tiles)
        {
            tk::Point origin = tile_pixel_origin(t, vp_px, map_rect);
            tk::Rect tdst{origin.x, origin.y, 256.0f, 256.0f};
            const std::string key = tile_cache_key(t);
            const tk::Image* img =
                owner_.image_provider_ ? owner_.image_provider_(key) : nullptr;
            if (img)
            {
                ctx.canvas.draw_image(*img, tdst);
                any_tile_drawn = true;
            }
            else
            {
                // Placeholder: fill with the surface background colour and
                // request the tile from the host.
                ctx.canvas.fill_rect(tdst, ctx.theme.palette.chrome_bg);
                if (owner_.on_tile_needed)
                {
                    owner_.on_tile_needed(t.z, t.x, t.y);
                }
            }
        }

        // Show a hint text when no tiles have loaded yet so the map area
        // doesn't look like a rendering gap.
        if (!any_tile_drawn && !tiles.empty())
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Small;
            st.max_width = map_rect.w;
            auto lo = ctx.factory.build_text("Loading map\xe2\x80\xa6", st);
            if (lo)
            {
                tk::Size sz = lo->measure();
                ctx.canvas.draw_text(
                    *lo,
                    {map_rect.x + (map_rect.w - sz.w) * 0.5f,
                     map_rect.y + (map_rect.h - lo->ascent()) * 0.5f},
                    ctx.theme.palette.text_muted);
            }
        }

        // Location pin — white outer disc + red inner disc, centred on the
        // pin coordinate. Use fill_rounded_rect with radius = diameter/2.
        {
            tk::Point pin_px =
                lat_lon_to_world_px(m.location_lat, m.location_lon, vp.zoom);
            float map_cx = map_rect.x + map_rect.w * 0.5f;
            float map_cy = map_rect.y + map_rect.h * 0.5f;
            float pin_sx = map_cx + (pin_px.x - vp_px.x);
            float pin_sy = map_cy + (pin_px.y - vp_px.y);

            constexpr float kPinOuter = 18.0f;
            constexpr float kPinInner = 12.0f;
            tk::Rect outer{pin_sx - kPinOuter * 0.5f, pin_sy - kPinOuter * 0.5f,
                           kPinOuter, kPinOuter};
            tk::Rect inner{pin_sx - kPinInner * 0.5f, pin_sy - kPinInner * 0.5f,
                           kPinInner, kPinInner};
            ctx.canvas.fill_rounded_rect(outer, kPinOuter * 0.5f,
                                         tk::Color{255, 255, 255, 230});
            ctx.canvas.fill_rounded_rect(inner, kPinInner * 0.5f,
                                         tk::Color::rgb(0xE53935));
        }

        // Attribution badge — "© OpenStreetMap contributors" at bottom-right.
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Small;
            st.wrap = false;
            auto lo = ctx.factory.build_text(
                "\xC2\xA9 OpenStreetMap contributors", st);
            if (lo)
            {
                tk::Size sz = lo->measure();
                constexpr float kBadgePadX = 5.0f;
                constexpr float kBadgePadY = 3.0f;
                float bx =
                    map_rect.x + map_rect.w - sz.w - kBadgePadX * 2 - 4.0f;
                float by =
                    map_rect.y + map_rect.h - sz.h - kBadgePadY * 2 - 4.0f;
                tk::Rect badge{bx, by, sz.w + kBadgePadX * 2,
                               sz.h + kBadgePadY * 2};
                ctx.canvas.fill_rounded_rect(badge, 3.0f,
                                             tk::Color{255, 255, 255, 180});
                ctx.canvas.draw_text(*lo, {bx + kBadgePadX, by + kBadgePadY},
                                     tk::Color{0, 0, 0, 200});
            }
        }

        ctx.canvas.pop_clip();
    }

    void paint_file_card(const MessageRowData& m, tk::PaintCtx& ctx,
                         tk::Rect dst) const
    {
        ctx.canvas.fill_rounded_rect(dst, 8.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 8.0f, ctx.theme.palette.border,
                                       1.0f);

        const std::string name = m.file_name.empty() ? m.body : m.file_name;
        const auto icon = file_icon_info(name);

        // Coloured icon box, vertically centred in the card.
        const float icon_y = dst.y + (dst.h - kFileIconSize) * 0.5f;
        const tk::Rect icon_rect{dst.x + kFileIconPadL, icon_y, kFileIconSize,
                                 kFileIconSize};
        ctx.canvas.fill_rounded_rect(icon_rect, 6.0f, icon.color);

        // Extension label centred inside the icon box.
        tk::TextStyle ls{};
        ls.role = tk::FontRole::UiSemibold;
        ls.halign = tk::TextHAlign::Center;
        ls.max_width = kFileIconSize;
        auto label_lo = ctx.factory.build_text(icon.label, ls);
        if (label_lo)
        {
            // UiSemibold ~11 pt; approximate cap-height ~13 px at 96 dpi.
            const float label_y = icon_y + (kFileIconSize - 13.0f) * 0.5f;
            ctx.canvas.draw_text(*label_lo, {icon_rect.x, label_y},
                                 tk::Color{255, 255, 255, 255});
        }

        // Filename + size shifted right of the icon box.
        const float text_x = dst.x + kFileTextOffX;
        const float text_w = dst.w - kFileTextOffX - 8.0f;

        tk::TextStyle ns{};
        ns.role = tk::FontRole::UiSemibold;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(name, ns);

        tk::TextStyle ss{};
        ss.role = tk::FontRole::Timestamp;
        auto size_lo = ctx.factory.build_text(format_size(m.file_size), ss);

        if (name_lo)
        {
            ctx.canvas.draw_text(*name_lo, {text_x, dst.y + 10.0f},
                                 ctx.theme.palette.text_primary);
        }
        if (size_lo)
        {
            ctx.canvas.draw_text(*size_lo, {text_x, dst.y + 30.0f},
                                 ctx.theme.palette.text_secondary);
        }
    }

    void paint_voice_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const
    {
        // Card chrome.
        ctx.canvas.fill_rounded_rect(dst, 10.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 10.0f, ctx.theme.palette.border,
                                       1.0f);

        // Play / pause button (circle on the left).
        const float btn_d = kVoicePlayBtnSize;
        const float btn_x = dst.x + kVoiceCardPadX;
        const float btn_y = dst.y + (dst.h - btn_d) * 0.5f;
        tk::Rect btn_rect{btn_x, btn_y, btn_d, btn_d};
        ctx.canvas.fill_rounded_rect(btn_rect, btn_d * 0.5f,
                                     ctx.theme.palette.accent);

        const bool is_active_row =
            m.event_id == owner_.playing_event_id_ && owner_.playing_is_active_;

        const tk::Color glyph_col = ctx.theme.palette.text_on_accent;
        if (is_active_row)
        {
            // Two pause bars centred in the button.
            const float bar_w = 4.0f;
            const float bar_h = btn_d * 0.45f;
            const float gap = 4.0f;
            const float cy = btn_y + (btn_d - bar_h) * 0.5f;
            const float cx = btn_x + btn_d * 0.5f;
            ctx.canvas.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h},
                                 glyph_col);
            ctx.canvas.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h},
                                 glyph_col);
        }
        else
        {
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
            const int steps = 8;
            for (int i = 0; i < steps; ++i)
            {
                const float t =
                    (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float row_h = tri_h / static_cast<float>(steps);
                const float row_w = tri_w * (1.0f - 2.0f * std::abs(t - 0.5f));
                const float ry = tri_y + i * row_h;
                ctx.canvas.fill_rect({tri_x, ry, std::max(1.0f, row_w), row_h},
                                     glyph_col);
            }
        }

        // Right-justified duration label. When this row is the active one
        // and the backend reports a non-zero position, show remaining time
        // instead of total — matches Element / FluffyChat affordances.
        std::uint64_t total =
            m.duration_ms > 0 ? m.duration_ms
                              : (m.event_id == owner_.playing_event_id_
                                     ? owner_.audio_player_
                                           ? owner_.audio_player_->duration_ms()
                                           : 0u
                                     : 0u);
        std::uint64_t label_ms = total;
        if (is_active_row && total > 0 && owner_.playing_position_ms_ <= total)
        {
            label_ms = total - owner_.playing_position_ms_;
        }
        tk::TextStyle ds{};
        ds.role = tk::FontRole::Timestamp;
        auto dur_lo = ctx.factory.build_text(format_mmss(label_ms), ds);
        tk::Size dur_sz = dur_lo ? dur_lo->measure() : tk::Size{};
        float dur_w = dur_sz.w;
        float dur_h = dur_sz.h;
        if (dur_lo)
        {
            ctx.canvas.draw_text(*dur_lo,
                                 {dst.x + dst.w - kVoiceCardPadX - dur_w,
                                  dst.y + (dst.h - dur_h) * 0.5f},
                                 ctx.theme.palette.text_secondary);
        }

        // Speed pill — sits just to the left of the duration label. Only
        // the active row's pill is interactive; the rate is global, so
        // rendering it on every row would be a lie about scope.
        tk::Rect pill_rect{};
        if (owner_.audio_player_ && is_active_row)
        {
            const float pill_x = dst.x + dst.w - kVoiceCardPadX - dur_w - 6.0f -
                                 kVoiceSpeedPillW;
            const float pill_y = dst.y + (dst.h - kVoiceSpeedPillH) * 0.5f;
            pill_rect = {pill_x, pill_y, kVoiceSpeedPillW, kVoiceSpeedPillH};
            ctx.canvas.fill_rounded_rect(pill_rect, kVoiceSpeedPillH * 0.5f,
                                         ctx.theme.palette.subtle_hover);

            char rate_buf[8];
            const float r = owner_.playback_rate_;
            if (r >= 1.99f)
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "2×");
            }
            else if (r >= 1.49f)
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "1.5×");
            }
            else
            {
                std::snprintf(rate_buf, sizeof(rate_buf), "1×");
            }
            tk::TextStyle rs{};
            rs.role = tk::FontRole::Timestamp;
            auto rate_lo = ctx.factory.build_text(rate_buf, rs);
            if (rate_lo)
            {
                tk::Size rsz = rate_lo->measure();
                ctx.canvas.draw_text(
                    *rate_lo,
                    {pill_rect.x + (pill_rect.w - rsz.w) * 0.5f,
                     pill_rect.y + (pill_rect.h - rsz.h) * 0.5f},
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
        const float right_anchor =
            (pill_rect.w > 0.0f)
                ? pill_rect.x - 6.0f
                : dst.x + dst.w - kVoiceCardPadX - kVoiceDurationW;
        const float strip_w_avail = right_anchor - strip_x;
        if (strip_w_avail < kVoiceBarW)
        {
            return;
        }
        const float strip_y = dst.y + dst.h * 0.5f;
        const float strip_h = dst.h - 16.0f;

        const float step = kVoiceBarW + kVoiceBarGap;
        const int bars = std::max(1, static_cast<int>(strip_w_avail / step));

        // Resample sender waveform → `bars` buckets. When empty, the loop
        // below uses the placeholder height. Senders often record voice well
        // below the spec's 0..=1024 ceiling (a normal-volume voice peaks at
        // ~10% of full scale), so normalise by the per-message peak instead
        // of the spec ceiling — otherwise quiet recordings collapse into a
        // flat row of minimum-height bars.
        std::uint16_t wf_peak = 0;
        for (std::uint16_t v : m.waveform)
            if (v > wf_peak) wf_peak = v;
        const float wf_norm = wf_peak > 0 ? 1.0f / static_cast<float>(wf_peak)
                                          : 0.0f;
        auto amp_at = [&](int i) -> float
        {
            if (m.waveform.empty())
            {
                return 0.0f;
            }
            const std::size_t n = m.waveform.size();
            const std::size_t src = std::min<std::size_t>(
                n - 1, static_cast<std::size_t>(static_cast<double>(i) / bars *
                                                static_cast<double>(n)));
            return std::min(1.0f,
                            static_cast<float>(m.waveform[src]) * wf_norm);
        };

        float cursor_frac = 0.0f;
        if (is_active_row && total > 0)
        {
            cursor_frac = static_cast<float>(owner_.playing_position_ms_) /
                          static_cast<float>(total);
            if (cursor_frac > 1.0f)
            {
                cursor_frac = 1.0f;
            }
        }
        const int cursor_bar = static_cast<int>(cursor_frac * bars);

        for (int i = 0; i < bars; ++i)
        {
            float a = amp_at(i);
            float bar_h = m.waveform.empty()
                              ? kVoiceBarMinH
                              : std::max(kVoiceBarMinH, a * strip_h);
            float bx = strip_x + i * step;
            float by = strip_y - bar_h * 0.5f;
            tk::Color c = (i < cursor_bar) ? ctx.theme.palette.accent
                                           : ctx.theme.palette.text_muted;
            ctx.canvas.fill_rounded_rect({bx, by, kVoiceBarW, bar_h},
                                         kVoiceBarW * 0.5f, c);
        }

        // Record world-coord geometry so on_pointer_down can hit-test
        // the play button, waveform strip, and speed pill without
        // re-running the layout maths.
        if (!m.event_id.empty())
        {
            tk::Rect strip_rect{
                strip_x,
                strip_y - strip_h * 0.5f,
                strip_w_avail,
                strip_h,
            };
            owner_.voice_card_geom_[m.event_id] =
                MessageListView::VoiceCardGeom{m.event_id, btn_rect, strip_rect,
                                               pill_rect, dst};
        }
    }

    void paint_audio_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const
    {
        // Card chrome — same radius as voice card.
        ctx.canvas.fill_rounded_rect(dst, 10.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 10.0f, ctx.theme.palette.border,
                                       1.0f);

        // Layout:
        //   Row 1 (top 36 px): [play btn 32px] [track] [time label]
        //   Row 2 (bottom 24 px): filename · size
        const float row1_h = 36.0f;
        const float row1_cy = dst.y + row1_h * 0.5f;

        // Play / pause button — identical to voice card.
        const float btn_d = kAudioPlayBtnSize;
        const float btn_x = dst.x + kAudioCardPadX;
        const float btn_y = row1_cy - btn_d * 0.5f;
        tk::Rect btn_rect{btn_x, btn_y, btn_d, btn_d};
        ctx.canvas.fill_rounded_rect(btn_rect, btn_d * 0.5f,
                                     ctx.theme.palette.accent);

        const bool is_active_row =
            m.event_id == owner_.playing_event_id_ && owner_.playing_is_active_;
        const tk::Color glyph_col = ctx.theme.palette.text_on_accent;
        if (is_active_row)
        {
            const float bar_w = 4.0f;
            const float bar_h = btn_d * 0.45f;
            const float gap = 4.0f;
            const float cy = btn_y + (btn_d - bar_h) * 0.5f;
            const float cx = btn_x + btn_d * 0.5f;
            ctx.canvas.fill_rect({cx - gap * 0.5f - bar_w, cy, bar_w, bar_h},
                                 glyph_col);
            ctx.canvas.fill_rect({cx + gap * 0.5f, cy, bar_w, bar_h},
                                 glyph_col);
        }
        else
        {
            const float tri_h = btn_d * 0.50f;
            const float tri_w = btn_d * 0.38f;
            const float tri_x = btn_x + btn_d * 0.5f - tri_w / 3.0f;
            const float tri_y = btn_y + (btn_d - tri_h) * 0.5f;
            const int steps = 8;
            for (int i = 0; i < steps; ++i)
            {
                const float t =
                    (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float row_h = tri_h / static_cast<float>(steps);
                const float row_w = tri_w * (1.0f - 2.0f * std::abs(t - 0.5f));
                const float ry = tri_y + i * row_h;
                ctx.canvas.fill_rect({tri_x, ry, std::max(1.0f, row_w), row_h},
                                     glyph_col);
            }
        }

        // Duration / elapsed time label — right-justified in row 1.
        std::uint64_t total =
            m.duration_ms > 0
                ? m.duration_ms
                : (m.event_id == owner_.playing_event_id_ && owner_.audio_player_
                       ? owner_.audio_player_->duration_ms()
                       : 0u);
        tk::TextStyle ds{};
        ds.role = tk::FontRole::Timestamp;
        std::uint64_t label_ms = (is_active_row && total > 0 &&
                                  owner_.playing_position_ms_ <= total)
                                     ? (total - owner_.playing_position_ms_)
                                     : total;
        auto dur_lo = ctx.factory.build_text(format_mmss(label_ms), ds);
        tk::Size dur_sz = dur_lo ? dur_lo->measure() : tk::Size{};
        const float time_x = dst.x + dst.w - kAudioCardPadX - dur_sz.w;
        if (dur_lo)
        {
            ctx.canvas.draw_text(
                *dur_lo,
                {time_x, row1_cy - dur_sz.h * 0.5f},
                ctx.theme.palette.text_secondary);
        }

        // Linear progress track — between play button and time label.
        const float track_x = btn_x + btn_d + kAudioCardPadX;
        const float track_right = time_x - kAudioCardPadX;
        const float track_w = track_right - track_x;
        const float track_y = row1_cy - kAudioTrackH * 0.5f;
        tk::Rect track_bg{track_x, track_y, track_w, kAudioTrackH};
        if (track_w > 0.0f)
        {
            ctx.canvas.fill_rounded_rect(track_bg, kAudioTrackH * 0.5f,
                                         ctx.theme.palette.text_muted);

            float fill_frac = 0.0f;
            if (is_active_row && total > 0)
            {
                fill_frac = static_cast<float>(owner_.playing_position_ms_) /
                            static_cast<float>(total);
                if (fill_frac > 1.0f)
                    fill_frac = 1.0f;
            }
            if (fill_frac > 0.0f)
            {
                ctx.canvas.fill_rounded_rect(
                    {track_x, track_y, track_w * fill_frac, kAudioTrackH},
                    kAudioTrackH * 0.5f, ctx.theme.palette.accent);
            }
        }

        // Row 2: filename · size.
        const float row2_y = dst.y + row1_h + 4.0f;
        const float name_x = btn_x + btn_d + kAudioCardPadX;
        const float name_w = dst.x + dst.w - kAudioCardPadX - name_x;
        const std::string display_name =
            m.file_name.empty() ? m.body : m.file_name;
        const std::string meta = m.file_size > 0
                                     ? display_name + " \xc2\xb7 " +
                                           format_size(m.file_size)
                                     : display_name;
        tk::TextStyle ns{};
        ns.role = tk::FontRole::Timestamp;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = name_w;
        auto name_lo = ctx.factory.build_text(meta, ns);
        if (name_lo)
        {
            ctx.canvas.draw_text(*name_lo, {name_x, row2_y},
                                 ctx.theme.palette.text_secondary);
        }

        // Record hit-test geometry.
        if (!m.event_id.empty())
        {
            tk::Rect full_track{track_x, dst.y, track_w,
                                row1_h}; // full row-1 height for easier tapping
            owner_.audio_card_geom_[m.event_id] =
                MessageListView::AudioCardGeom{m.event_id, btn_rect, full_track,
                                               dst};
        }
    }

    void paint_video_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const
    {
        // Live inline player frame takes priority over the static thumbnail.
        const tk::Image* live_frame = nullptr;
        {
            auto it = owner_.inline_players_.find(m.event_id);
            if (it != owner_.inline_players_.end() && it->second.player)
            {
                live_frame = it->second.player->current_frame();
            }
        }

        // Thumbnail fallback (or placeholder when neither is available).
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        const std::string thumb_key = look ? look->fetch_token() : std::string{};
        const tk::Image* thumb = nullptr;
        if (!live_frame && owner_.image_provider_ && !thumb_key.empty())
        {
            thumb = owner_.image_provider_(thumb_key);
        }

        if (live_frame)
        {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*live_frame, dst);
            ctx.canvas.pop_clip();
        }
        else if (thumb)
        {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*thumb, dst);
            ctx.canvas.pop_clip();
        }
        else
        {
            const tk::Image* bh_img = nullptr;
            if (!m.blurhash.empty() && owner_.image_provider_)
            {
                bh_img = owner_.image_provider_("blurhash::" + m.event_id);
            }
            if (bh_img)
            {
                ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
                ctx.canvas.draw_image(*bh_img, dst);
                ctx.canvas.pop_clip();
            }
            else
            {
                ctx.canvas.fill_rounded_rect(dst, 8.0f,
                                             ctx.theme.palette.chrome_bg);
                ctx.canvas.stroke_rounded_rect(dst, 8.0f,
                                               ctx.theme.palette.border, 1.0f);
            }
        }

        // Autoplay / GIF-mode clips play immediately — no play button needed.
        const bool animated = m.video_gif || m.video_autoplay;

        // Centred play disc — omitted for animated clips.
        if (!animated)
        {
            constexpr float kDiscD = 48.0f;
            const float disc_cx = dst.x + dst.w * 0.5f;
            const float disc_cy = dst.y + dst.h * 0.5f;
            tk::Rect disc{disc_cx - kDiscD * 0.5f, disc_cy - kDiscD * 0.5f,
                          kDiscD, kDiscD};
            ctx.canvas.fill_rounded_rect(disc, kDiscD * 0.5f,
                                         tk::Color{0, 0, 0, 120});

            // Play triangle (▶): same symmetric stacked-rect approach as
            // the voice card — centroid-shifted so the glyph is centred.
            const float tri_h = kDiscD * 0.45f;
            const float tri_w = kDiscD * 0.35f;
            const float tri_x = disc.x + kDiscD * 0.5f - tri_w / 3.0f;
            const float tri_y = disc.y + (kDiscD - tri_h) * 0.5f;
            constexpr int steps = 8;
            for (int i = 0; i < steps; ++i)
            {
                const float t =
                    (static_cast<float>(i) + 0.5f) / static_cast<float>(steps);
                const float row_h = tri_h / static_cast<float>(steps);
                const float row_w = tri_w * (1.0f - 2.0f * std::abs(t - 0.5f));
                ctx.canvas.fill_rect(
                    {tri_x, tri_y + i * row_h, std::max(1.0f, row_w), row_h},
                    tk::Color{255, 255, 255, 230});
            }
        }

        // Duration badge at bottom-right — omitted for animated or hide_controls clips.
        if (!animated && !m.video_hide_controls && m.duration_ms > 0)
        {
            std::string label = format_mmss(m.duration_ms);
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Timestamp;
            auto lo = ctx.factory.build_text(label, ts);
            if (lo)
            {
                tk::Size lsz = lo->measure();
                constexpr float kBadgePadX = 6.0f, kBadgePadY = 3.0f;
                float bx = dst.x + dst.w - lsz.w - kBadgePadX * 2 - 4.0f;
                float by = dst.y + dst.h - lsz.h - kBadgePadY * 2 - 4.0f;
                tk::Rect badge{bx, by, lsz.w + kBadgePadX * 2,
                               lsz.h + kBadgePadY * 2};
                ctx.canvas.fill_rounded_rect(badge, 4.0f,
                                             tk::Color{0, 0, 0, 140});
                ctx.canvas.draw_text(*lo, {bx + kBadgePadX, by + kBadgePadY},
                                     tk::Color{255, 255, 255, 230});
            }
        }
    }

    // ── Layout caches ───────────────────────────────────────────────────────

    // Glyphs that never change across rows — built once on first paint.
    struct StaticLayouts
    {
        std::unique_ptr<tk::TextLayout> plus;
        std::unique_ptr<tk::TextLayout> reply;
        std::unique_ptr<tk::TextLayout> edit;
        std::unique_ptr<tk::TextLayout> trash;

        void ensure(tk::CanvasFactory& f)
        {
            if (plus)
            {
                return;
            }
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            plus = f.build_text("+", st);
            reply = f.build_text("\xE2\x86\xA9", st);     // ↩
            edit = f.build_text("\xE2\x9C\x8F", st);      // ✏
            trash = f.build_text("\xF0\x9F\x97\x91", st); // 🗑
        }
        void clear()
        {
            plus.reset();
            reply.reset();
            edit.reset();
            trash.reset();
        }
    };

    // Per-row cache: sender name layout + per-reaction count/glyph layouts.
    struct RowLayoutCache
    {
        std::string sender_key;
        float sender_col_w = -1;
        std::unique_ptr<tk::TextLayout> sender;

        struct ReactionEntry
        {
            std::string key;
            int count = -1;
            std::unique_ptr<tk::TextLayout> count_layout;
            std::unique_ptr<tk::TextLayout>
                glyph_layout; // null for image reactions
        };
        std::vector<ReactionEntry> reactions;

        void clear()
        {
            sender_key.clear();
            sender_col_w = -1;
            sender.reset();
            reactions.clear();
        }
    };

    // Returns a writable reference to the cache entry for `index`,
    // growing the vector on demand (default-constructs new entries).
    RowLayoutCache& cache_for(std::size_t index) const
    {
        if (index >= layout_cache_.size())
        {
            layout_cache_.resize(index + 1);
        }
        return layout_cache_[index];
    }

    mutable StaticLayouts static_cache_;
    mutable std::vector<RowLayoutCache> layout_cache_;

    MessageListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

MessageListView::~MessageListView() = default;

MessageListView::MessageListView() : adapter_(std::make_unique<Adapter>(*this))
{
    set_adapter(adapter_.get());
    on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= messages_.size())
        {
            return;
        }
        if (on_message_clicked)
        {
            on_message_clicked(messages_[idx].event_id);
        }
    };
}

std::optional<MessageListView::StickerHit>
MessageListView::sticker_hit_at(tk::Point world) const
{
    // Sticker geometry is recorded by paint_row in world coordinates each
    // paint pass. We linearly search — a sticker viewport rarely exceeds
    // a handful of visible rows so a hash lookup by point isn't worth it.
    for (const auto& [event_id, hit] : sticker_geom_)
    {
        if (world.x >= hit.world_rect.x && world.y >= hit.world_rect.y &&
            world.x < hit.world_rect.x + hit.world_rect.w &&
            world.y < hit.world_rect.y + hit.world_rect.h)
        {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<MessageListView::ImageHit>
MessageListView::image_hit_at(tk::Point world) const
{
    for (const auto& [eid, hit] : image_geom_)
    {
        if (world.x >= hit.world_rect.x && world.y >= hit.world_rect.y &&
            world.x < hit.world_rect.x + hit.world_rect.w &&
            world.y < hit.world_rect.y + hit.world_rect.h)
        {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<MessageListView::VideoHit>
MessageListView::video_hit_at(tk::Point world) const
{
    for (const auto& [eid, hit] : video_geom_)
    {
        if (world.x >= hit.world_rect.x && world.y >= hit.world_rect.y &&
            world.x < hit.world_rect.x + hit.world_rect.w &&
            world.y < hit.world_rect.y + hit.world_rect.h)
        {
            return hit;
        }
    }
    return std::nullopt;
}

std::optional<MessageListView::FileHit>
MessageListView::file_hit_at(tk::Point world) const
{
    for (const auto& [eid, hit] : file_geom_)
    {
        if (world.x >= hit.world_rect.x && world.y >= hit.world_rect.y &&
            world.x < hit.world_rect.x + hit.world_rect.w &&
            world.y < hit.world_rect.y + hit.world_rect.h)
        {
            return hit;
        }
    }
    return std::nullopt;
}

void MessageListView::set_messages(std::vector<MessageRowData> msgs,
                                   bool room_switch)
{
    inline_players_.clear();
    revealed_spoilers_.clear();
    link_layout_cache_.clear();
    adapter_->clear_layout_cache();
    suppress_read_marker_ = false;
    sel_.reset();
    sel_is_dragging_ = false;
    press_sel_ = false;
    messages_ = std::move(msgs);
    invalidate_data();
    scroll_to_bottom();
    // Start inline players for animated video rows near the bottom (most
    // recently received), up to the cap.
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    {
        if (static_cast<int>(inline_players_.size()) >= kMaxInlinePlayers)
        {
            break;
        }
        if (it->kind == MessageRowData::Kind::Video &&
            (it->video_autoplay || it->video_gif))
        {
            start_inline_video(*it);
        }
    }

    // Supersede any prior gate (rapid re-switch / same-room reset). The
    // bumped epoch also neutralises an outstanding timeout closure.
    ++room_switch_epoch_;
    room_switch_gate_.reset();
    if (!room_switch || messages_.empty())
    {
        return; // nothing to gate
    }

    RoomSwitchGate g;
    g.epoch = room_switch_epoch_;
    room_switch_gate_ = std::move(g);
    // Dependencies are collected on the first paint (the visible band
    // needs a measure pass). Arm the timeout fallback now so a slow /
    // offline network can never hold the list invisible forever.
    if (post_delayed_)
    {
        std::weak_ptr<bool> walive = alive_;
        std::uint64_t ep = room_switch_epoch_;
        post_delayed_(
            kRoomSwitchGateTimeoutMs,
            [this, walive, ep]()
            {
                if (walive.expired())
                {
                    return;
                }
                if (!room_switch_gate_ || room_switch_gate_->epoch != ep)
                {
                    return;
                }
                // Mark evaluated so a first paint that hasn't run yet (window
                // occluded / paint delayed past the deadline) won't re-derive
                // and re-arm `pending` in collect_gate_deps_ — the one-shot
                // timeout would otherwise be lost and the list stay hidden.
                room_switch_gate_->evaluated = true;
                room_switch_gate_->pending.clear(); // force reveal next paint
                if (request_repaint_)
                {
                    request_repaint_();
                }
            });
    }
}

void MessageListView::begin_focused_gate(const std::string& focus_event_id)
{
    if (!room_switch_gate_ || room_switch_gate_->evaluated)
    {
        return;
    }
    room_switch_gate_->focused = true;
    room_switch_gate_->focus_event_id = focus_event_id;
}

bool MessageListView::edit_last_own()
{
    if (!on_edit_requested)
    {
        return false;
    }
    // Newest first. Same editability rule as the hover ✏ button: own,
    // Kind::Text, fully sent (not a Sending/Failed local echo), real id.
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    {
        const MessageRowData& m = *it;
        if (m.is_own && m.kind == MessageRowData::Kind::Text &&
            m.pending_state == MessageRowData::PendingState::None &&
            !m.event_id.empty())
        {
            on_edit_requested(m.event_id, m.body);
            return true;
        }
    }
    return false;
}

bool MessageListView::gate_dep_satisfied_(const MessageRowData& m) const
{
    using K = MessageRowData::Kind;
    switch (m.kind)
    {
    case K::Image:
    case K::Sticker:
    {
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        const std::string wait_key = look ? look->fetch_token() : std::string{};
        if (wait_key.empty() || !image_provider_)
        {
            return true;
        }
        if (const tk::Image* im = image_provider_(wait_key))
        {
            return im->width() > 0 && im->height() > 0;
        }
        return false;
    }
    case K::Video:
        // Only a server-provided thumbnail is worth waiting for. When the
        // server omits one the row falls back to a client-generated frame
        // (no generator on every platform) — don't stall the whole list
        // on it; the metadata/placeholder height is already stable.
        if (!m.thumbnail || !image_provider_)
        {
            return true;
        }
        if (const tk::Image* im = image_provider_(m.thumbnail->fetch_token()))
        {
            return im->width() > 0 && im->height() > 0;
        }
        return false;
    case K::Text:
    case K::Notice:
    case K::Unhandled:
    case K::Emote:
        // A pending preview returns nullptr; a failed one is released via
        // on_url_preview_failed_ → notify_url_preview_ready (height stays
        // 0, so no jump) so we don't wait the full timeout on dead links.
        if (m.first_url.empty() || !preview_provider_)
        {
            return true;
        }
        return preview_provider_(m.first_url) != nullptr;
    default:
        return true; // file / voice / redacted / separators: height final
    }
}

void MessageListView::collect_gate_deps_()
{
    if (!room_switch_gate_)
    {
        return;
    }
    auto& g = *room_switch_gate_;
    g.pending.clear();

    auto [first, last] = visible_range();
    if (first < 0 || last < first)
    {
        return; // nothing visible → reveal
    }

    for (int i = first; i <= last && i < static_cast<int>(messages_.size());
         ++i)
    {
        const auto& m = messages_[static_cast<std::size_t>(i)];
        if (gate_dep_satisfied_(m))
        {
            continue;
        }
        using K = MessageRowData::Kind;
        if (m.kind == K::Image || m.kind == K::Sticker)
        {
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            if (look) g.pending.insert(look->fetch_token());
        }
        else if (m.kind == K::Video)
        {
            if (m.thumbnail) g.pending.insert(m.thumbnail->fetch_token());
        }
        else if (!m.first_url.empty())
        {
            g.pending.insert(m.first_url);
        }
    }
}

void MessageListView::reveal_room_switch_gate_()
{
    if (!room_switch_gate_)
    {
        return;
    }
    const bool focused = room_switch_gate_->focused;
    const std::string fid = room_switch_gate_->focus_event_id;
    room_switch_gate_.reset();
    // Heights are already final for this frame (ensure_measured ran before
    // we got here and every gated dependency is resolved). Just re-pin the
    // scroll so the very first visible frame is correct: the bottom case is
    // already handled by stick_to_bottom_ inside ensure_measured; focused
    // mode must recompute against the now-final offsets.
    if (focused && !fid.empty())
    {
        scroll_to_event_id(fid);
    }
    else
    {
        scroll_to_bottom();
    }
}

void MessageListView::on_gate_notify_(const std::string& key)
{
    if (!room_switch_gate_)
    {
        return;
    }
    auto& g = *room_switch_gate_;
    g.pending.erase(key);
    if (g.evaluated && g.pending.empty() && request_repaint_)
    {
        request_repaint_(); // next paint reveals via reveal_room_switch_gate_
    }
}

void MessageListView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    post_delayed_ = std::move(f);
}

void MessageListView::insert_message(std::size_t index, MessageRowData msg)
{
    if (index > messages_.size())
    {
        index = messages_.size();
    }

    const bool animated = msg.kind == MessageRowData::Kind::Video &&
                          (msg.video_autoplay || msg.video_gif);

    // Suppress the read marker while the SDK catches up to the new position.
    // update_message() clears this flag when it delivers the updated marker.
    using Kind = MessageRowData::Kind;
    if (msg.kind != Kind::ReadMarker && msg.kind != Kind::DaySeparator &&
        msg.kind != Kind::TimelineStart)
    {
        suppress_read_marker_ = true;
    }

    // Insertion at (or past) the end is an append: follow the live tail
    // when the user is already pinned there.
    if (index == messages_.size())
    {
        bool at_bottom = scroll_y() + bounds().h + 1.0f >= content_height();
        if (animated)
        {
            start_inline_video(msg);
        }
        messages_.push_back(std::move(msg));
        invalidate_data();
        if (at_bottom)
        {
            scroll_to_bottom();
        }
        return;
    }

    // Insertion above (or at) the viewport's first visible row: anchor
    // the existing rows so the user's visual position stays put. For
    // mid-viewport inserts the same preserve-top math is a benign no-op
    // (the row the user is looking at stays under their cursor because
    // its row offset shifts by the new row's height, which is what
    // preserve_top_through compensates for).
    if (animated)
    {
        start_inline_video(msg);
    }
    adapter_->insert_layout_cache_at(index);
    preserve_top_through(
        [&]
        {
            messages_.insert(messages_.begin() + index, std::move(msg));
            invalidate_data();
        });
}

void MessageListView::update_message(std::size_t index, MessageRowData msg)
{
    if (index >= messages_.size())
    {
        return;
    }
    if (msg.kind == MessageRowData::Kind::ReadMarker)
    {
        suppress_read_marker_ = false;
    }
    // Copy, not reference: messages_[index] is reassigned below.
    const std::string old_eid = messages_[index].event_id;
    const bool was_animated =
        messages_[index].kind == MessageRowData::Kind::Video &&
        (messages_[index].video_autoplay || messages_[index].video_gif);
    const bool now_animated = msg.kind == MessageRowData::Kind::Video &&
                              (msg.video_autoplay || msg.video_gif);
    // Erase the old player when it no longer applies — also when the
    // event_id changed, otherwise the old keyed player would leak (and
    // could exceed the inline-player cap for the view's lifetime).
    if ((was_animated && !now_animated) || old_eid != msg.event_id)
    {
        inline_players_.erase(old_eid);
    }
    if (now_animated && !inline_players_.count(msg.event_id))
    {
        start_inline_video(msg);
    }
    adapter_->invalidate_layout_cache_at(index);

    // Detect Sending → None transition for own messages: set just_sent and
    // schedule a 2-second timer to clear it.
    using PS = MessageRowData::PendingState;
    const bool was_sending = messages_[index].pending_state == PS::Sending;
    const bool now_none = msg.pending_state == PS::None;
    if (was_sending && now_none && messages_[index].is_own)
    {
        msg.just_sent = true;
        if (on_just_sent)
        {
            on_just_sent(msg.event_id);
        }
        if (post_delayed_)
        {
            std::weak_ptr<bool> walive = alive_;
            const std::string eid = msg.event_id;
            post_delayed_(kJustSentHighlightMs,
                          [this, walive, eid]
                          {
                              if (walive.expired())
                              {
                                  return;
                              }
                              clear_just_sent(eid);
                          });
        }
    }

    messages_[index] = std::move(msg);
    invalidate_data();
}

void MessageListView::clear_just_sent(const std::string& event_id)
{
    for (auto& row : messages_)
    {
        if (row.event_id == event_id && row.just_sent)
        {
            row.just_sent = false;
            invalidate_data();
            return;
        }
    }
}

void MessageListView::remove_message(std::size_t index)
{
    if (index >= messages_.size())
    {
        return;
    }
    inline_players_.erase(messages_[index].event_id);
    adapter_->erase_layout_cache_at(index);
    preserve_top_through(
        [&]
        {
            messages_.erase(messages_.begin() + index);
            invalidate_data();
        });
}

void MessageListView::append_message(MessageRowData msg)
{
    insert_message(messages_.size(), std::move(msg));
}

void MessageListView::set_typing_text(std::string text)
{
    if (text == typing_text_)
    {
        return;
    }
    bool was_empty = typing_text_.empty();
    typing_text_ = std::move(text);
    // Snap to bottom when the indicator first appears so the user sees it
    // without scrolling, but only when already near the bottom — avoids
    // hijacking scroll position while browsing paginated history.
    if (was_empty && !typing_text_.empty())
    {
        if (scroll_y() + bounds().h + kTypingRowH + 1.0f >= content_height())
        {
            scroll_to_bottom();
        }
    }
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::set_avatar_provider(ImageProvider p)
{
    avatar_provider_ = std::move(p);
}

void MessageListView::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void MessageListView::set_preview_provider(PreviewProvider p)
{
    preview_provider_ = std::move(p);
}

void MessageListView::notify_url_preview_ready(const std::string& url)
{
    on_gate_notify_(url);
    auto [first, last] = visible_range();
    for (std::size_t i = 0; i < messages_.size(); ++i)
    {
        if (messages_[i].first_url == url)
        {
            if (first > 0 && static_cast<int>(i) < first)
            {
                preserve_top_through(
                    [&]
                    {
                        invalidate_data();
                    });
            }
            else
            {
                invalidate_data();
            }
            return;
        }
    }
    invalidate_data();
}

void MessageListView::notify_image_ready(const std::string& url)
{
    on_gate_notify_(url);
    auto [first, last] = visible_range();
    for (std::size_t i = 0; i < messages_.size(); ++i)
    {
        // Match either the full-res token or the thumbnail token.
        // ShellBase pre-fetches whichever is present (thumbnail wins), so an
        // Image/Sticker row whose only cached representation is the thumbnail
        // would otherwise never remeasure when bytes arrive.
        const auto& m = messages_[i];
        const bool src_match = m.source && m.source->fetch_token() == url;
        const bool thumb_match = m.thumbnail && m.thumbnail->fetch_token() == url;
        const bool fsrc_match = m.file_source && m.file_source->fetch_token() == url;
        if (src_match || thumb_match || fsrc_match)
        {
            if (first > 0 && static_cast<int>(i) < first)
            {
                preserve_top_through(
                    [&]
                    {
                        invalidate_data();
                    });
            }
            else
            {
                invalidate_data();
            }
            return;
        }
    }
}

void MessageListView::update_voice_waveform(const std::string&         event_id,
                                            std::vector<std::uint16_t> waveform)
{
    for (auto& msg : messages_)
    {
        if (msg.kind == MessageRowData::Kind::Voice &&
            msg.event_id == event_id && msg.waveform.empty())
        {
            msg.waveform = std::move(waveform);
            invalidate_data();
            return;
        }
    }
}

void MessageListView::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    audio_player_ = std::move(player);
    if (audio_player_)
    {
        audio_player_->on_progress = [this]()
        {
            on_audio_progress();
        };
    }
}

void MessageListView::set_voice_bytes_provider(VoiceBytesProvider provider)
{
    voice_bytes_provider_ = std::move(provider);
}

void MessageListView::set_repaint_requester(
    std::function<void()> request_repaint)
{
    request_repaint_ = std::move(request_repaint);
}

void MessageListView::set_video_player_factory(VideoPlayerFactory f)
{
    video_player_factory_ = std::move(f);
}

void MessageListView::set_video_fetch_provider(VideoFetchProvider f)
{
    video_fetch_provider_ = std::move(f);
}

void MessageListView::start_inline_video(const MessageRowData& m)
{
    if (!video_player_factory_ || !video_fetch_provider_)
    {
        return;
    }
    if (inline_players_.count(m.event_id))
    {
        return;
    }
    if (static_cast<int>(inline_players_.size()) >= kMaxInlinePlayers)
    {
        return;
    }

    auto player = video_player_factory_();
    if (!player)
    {
        return;
    }
    player->set_loop(m.video_loop);
    player->set_muted(m.video_no_audio);
    std::weak_ptr<bool> walive = alive_;
    player->on_frame = [this, walive]
    {
        if (walive.expired())
        {
            return;
        }
        if (request_repaint_)
        {
            request_repaint_();
        }
    };
    inline_players_[m.event_id] = {std::move(player)};

    const std::string eid = m.event_id;
    const std::string src = m.source ? m.source->fetch_token() : std::string{};
    const std::string mime = m.video_mime;
    const bool autoplay = m.video_autoplay;

    video_fetch_provider_(
        src,
        [this, walive, eid, mime, autoplay](std::vector<std::uint8_t> bytes)
        {
            // The view is destroyed on every room switch; the fetch may
            // still be in flight. Bail if we've been torn down.
            if (walive.expired())
            {
                return;
            }
            auto it = inline_players_.find(eid);
            if (it == inline_players_.end() || !it->second.player)
            {
                return;
            }
            if (bytes.empty())
            {
                inline_players_.erase(eid);
                return;
            }
            auto& p = *it->second.player;
            p.play(bytes.data(), bytes.size(), mime);
            if (!autoplay)
            {
                p.pause();
            }
        });
}

bool MessageListView::on_wheel(tk::Point local, float dx, float dy)
{
    if (gate_blocks_input_())
    {
        return false; // list not painted yet
    }

    // Map zoom: intercept wheel over a Kind::Location map tile area.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        std::size_t ri = hovered_row_geom_.row_index;
        if (ri < messages_.size() &&
            messages_[ri].kind == MessageRowData::Kind::Location)
        {
            auto it = map_rect_geom_.find(messages_[ri].event_id);
            if (it != map_rect_geom_.end() && rect_contains(it->second, world))
            {
                map_zoom_accum_ += dy;
                // Fire at most one zoom step per wheel event so a physical
                // mouse wheel (dy ≈ 90) doesn't jump many levels at once.
                constexpr float kZoomThreshold = 60.0f;
                if (map_zoom_accum_ >= kZoomThreshold)
                {
                    map_zoom_accum_ = 0.0f;
                    auto& vp = messages_[ri].map_viewport;
                    vp.zoom = std::min(19, vp.zoom + 1);
                    invalidate_data();
                }
                else if (map_zoom_accum_ <= -kZoomThreshold)
                {
                    map_zoom_accum_ = 0.0f;
                    auto& vp = messages_[ri].map_viewport;
                    vp.zoom = std::max(1, vp.zoom - 1);
                    invalidate_data();
                }
                return true;
            }
        }
    }

    return tk::ListView::on_wheel(local, dx, dy);
}

void MessageListView::on_pointer_drag(tk::Point local)
{
    if (gate_blocks_input_())
    {
        return;
    }
    if (press_audio_kind_ == AudioPressKind::ProgressTrack &&
        !press_audio_event_id_.empty())
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& row : messages_)
        {
            if (row.event_id == press_audio_event_id_ &&
                row.kind == MessageRowData::Kind::Audio)
            {
                handle_audio_scrub_at(row, world.x);
                break;
            }
        }
        return;
    }

    if (press_sel_ && sel_)
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        int row_idx = index_at(local);
        if (row_idx >= 0 && row_idx < static_cast<int>(messages_.size()))
        {
            const auto& m = messages_[static_cast<std::size_t>(row_idx)];
            if (m.kind == MessageRowData::Kind::Text ||
                m.kind == MessageRowData::Kind::Notice ||
                m.kind == MessageRowData::Kind::Emote)
            {
                auto it = link_layout_cache_.find(m.event_id);
                if (it != link_layout_cache_.end() && it->second.layout)
                {
                    tk::Point ll{world.x - it->second.origin.x,
                                 world.y - it->second.origin.y};
                    int idx = it->second.layout->char_index_at(ll);
                    if (idx >= 0)
                    {
                        bool changed = (sel_->head_event_id != m.event_id ||
                                        sel_->head_byte != idx);
                        sel_->head_event_id = m.event_id;
                        sel_->head_byte     = idx;
                        sel_is_dragging_ =
                            (sel_->head_event_id != sel_->anchor_event_id ||
                             sel_->head_byte     != sel_->anchor_byte);
                        if (changed && request_repaint_)
                            request_repaint_();
                    }
                }
            }
        }
        return;
    }

    if (press_voice_kind_ != VoicePressKind::Waveform)
    {
        tk::ListView::on_pointer_drag(local);
        return;
    }
    if (press_voice_event_id_.empty())
    {
        return;
    }
    tk::Point world{local.x + bounds().x, local.y + bounds().y};
    for (const auto& row : messages_)
    {
        if (row.event_id == press_voice_event_id_ &&
            row.kind == MessageRowData::Kind::Voice)
        {
            handle_voice_scrub_at(row, world.x);
            break;
        }
    }
}

void MessageListView::handle_voice_scrub_at(const MessageRowData& row,
                                            float world_x)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    auto it = voice_card_geom_.find(row.event_id);
    if (it == voice_card_geom_.end())
    {
        return;
    }
    const tk::Rect& strip = it->second.waveform_strip;
    if (strip.w <= 0.0f)
    {
        return;
    }

    float frac = (world_x - strip.x) / strip.w;
    if (frac < 0.0f)
    {
        frac = 0.0f;
    }
    if (frac > 1.0f)
    {
        frac = 1.0f;
    }

    // Resolve total duration: the sender's metadata if present, otherwise
    // whatever the backend has discovered after loading.
    std::uint64_t total = row.duration_ms > 0 ? row.duration_ms : 0;
    if (total == 0 && row.event_id == playing_event_id_)
    {
        total = audio_player_->duration_ms();
    }
    if (total == 0)
    {
        return;
    }

    const std::uint64_t target_ms =
        static_cast<std::uint64_t>(frac * static_cast<float>(total));

    if (row.event_id != playing_event_id_)
    {
        // Start playback at the clicked position. Same byte-cache path
        // as a regular play click; on cache miss the view stays idle and
        // the user can try again once the prefetch lands.
        std::vector<std::uint8_t> bytes =
            voice_bytes_provider_(row.audio_source ? row.audio_source->fetch_token() : std::string{});
        if (bytes.empty())
        {
            if (request_repaint_)
            {
                request_repaint_();
            }
            return;
        }
        if (!playing_event_id_.empty())
        {
            audio_player_->stop();
        }
        playing_event_id_ = row.event_id;
        playing_position_ms_ = target_ms;
        playing_is_active_ = true;
        playing_ever_active_ = false;
        audio_player_->set_playback_rate(playback_rate_);
        audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
        audio_player_->seek(target_ms);
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }

    audio_player_->seek(target_ms);
    playing_position_ms_ = target_ms;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::handle_voice_speed_click()
{
    if (!audio_player_)
    {
        return;
    }
    if (playback_rate_ < 1.49f)
    {
        playback_rate_ = 1.5f;
    }
    else if (playback_rate_ < 1.99f)
    {
        playback_rate_ = 2.0f;
    }
    else
    {
        playback_rate_ = 1.0f;
    }
    audio_player_->set_playback_rate(playback_rate_);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::on_audio_progress()
{
    if (audio_player_)
    {
        playing_position_ms_ = audio_player_->position_ms();
        playing_is_active_ = audio_player_->is_playing();
        if (playing_is_active_)
        {
            playing_ever_active_ = true;
        }
        // Only treat position-0 + not-playing as "clip ended" once the clip
        // was confirmed to have started.  Without this guard the same condition
        // fires during Qt's async-load window (is_playing()==false while the
        // FFmpeg probe is in flight), clearing playing_event_id_ before the
        // first repaint so the play button never flips to active.
        if (!playing_is_active_ && playing_position_ms_ == 0 && playing_ever_active_)
        {
            playing_event_id_.clear();
            playing_ever_active_ = false;
        }
    }
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::handle_voice_play_click(const MessageRowData& row)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }

    // Clicking the currently-active row's play button toggles pause.
    if (row.event_id == playing_event_id_)
    {
        if (audio_player_->is_playing())
        {
            audio_player_->pause();
        }
        else
        {
            audio_player_->resume();
        }
        on_audio_progress();
        return;
    }

    // Switching rows — stop the current clip cleanly first.
    if (!playing_event_id_.empty())
    {
        audio_player_->stop();
    }

    std::vector<std::uint8_t> bytes = voice_bytes_provider_(row.audio_source ? row.audio_source->fetch_token() : std::string{});
    if (bytes.empty())
    {
        // Cache miss — the SDK kicks off a background fetch on the first
        // call. Surface state honestly: nothing is loaded, repaint so the
        // pause glyph (if any) reverts to play, and let the user click
        // again once bytes arrive.
        playing_event_id_.clear();
        playing_is_active_ = false;
        playing_ever_active_ = false;
        playing_position_ms_ = 0;
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    playing_event_id_ = row.event_id;
    playing_position_ms_ = 0;
    playing_is_active_ = true;
    playing_ever_active_ = false;
    audio_player_->set_playback_rate(playback_rate_);
    audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::handle_audio_play_click(const MessageRowData& row)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    if (row.event_id == playing_event_id_)
    {
        if (audio_player_->is_playing())
        {
            audio_player_->pause();
        }
        else
        {
            audio_player_->resume();
        }
        on_audio_progress();
        return;
    }
    if (!playing_event_id_.empty())
    {
        audio_player_->stop();
    }
    std::vector<std::uint8_t> bytes = voice_bytes_provider_(row.audio_source ? row.audio_source->fetch_token() : std::string{});
    if (bytes.empty())
    {
        playing_event_id_.clear();
        playing_is_active_ = false;
        playing_ever_active_ = false;
        playing_position_ms_ = 0;
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    playing_event_id_ = row.event_id;
    playing_position_ms_ = 0;
    playing_is_active_ = true;
    playing_ever_active_ = false;
    audio_player_->set_playback_rate(1.0f);
    audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::handle_audio_scrub_at(const MessageRowData& row,
                                            float world_x)
{
    if (!audio_player_ || !voice_bytes_provider_)
    {
        return;
    }
    auto it = audio_card_geom_.find(row.event_id);
    if (it == audio_card_geom_.end())
    {
        return;
    }
    const tk::Rect& track = it->second.progress_track;
    if (track.w <= 0.0f)
    {
        return;
    }
    float frac = (world_x - track.x) / track.w;
    frac = std::max(0.0f, std::min(1.0f, frac));

    std::uint64_t total = row.duration_ms > 0 ? row.duration_ms : 0;
    if (total == 0 && row.event_id == playing_event_id_)
    {
        total = audio_player_->duration_ms();
    }
    if (total == 0)
    {
        return;
    }
    const std::uint64_t target_ms =
        static_cast<std::uint64_t>(frac * static_cast<float>(total));

    if (row.event_id != playing_event_id_)
    {
        std::vector<std::uint8_t> bytes =
            voice_bytes_provider_(row.audio_source ? row.audio_source->fetch_token() : std::string{});
        if (bytes.empty())
        {
            if (request_repaint_)
            {
                request_repaint_();
            }
            return;
        }
        if (!playing_event_id_.empty())
        {
            audio_player_->stop();
        }
        playing_event_id_ = row.event_id;
        playing_position_ms_ = target_ms;
        playing_is_active_ = true;
        playing_ever_active_ = false;
        audio_player_->set_playback_rate(1.0f);
        audio_player_->play(bytes.data(), bytes.size(), row.audio_mime);
        audio_player_->seek(target_ms);
        if (request_repaint_)
        {
            request_repaint_();
        }
        return;
    }
    audio_player_->seek(target_ms);
    playing_position_ms_ = target_ms;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

// Resolve which chip (if any) is under a widget-local point. `local`
// is in widget-local coordinates (relative to MessageListView::bounds_);
// the cached geometry is in world coordinates, so we add the widget
// origin back before comparing.
static MessageListView::HoverTarget
chip_hit_at(const MessageListView::RowChipGeom& g, tk::Rect widget_bounds,
            tk::Point local, int& out_chip_idx)
{
    out_chip_idx = -1;
    if (g.row_index == static_cast<std::size_t>(-1))
    {
        return MessageListView::HoverTarget::None;
    }
    tk::Point world{local.x + widget_bounds.x, local.y + widget_bounds.y};
    if (!rect_contains(g.row_bounds, world))
    {
        return MessageListView::HoverTarget::None;
    }
    for (std::size_t i = 0; i < g.chips.size(); ++i)
    {
        if (g.chips[i].w <= 0)
        {
            continue;
        }
        if (rect_contains(g.chips[i], world))
        {
            out_chip_idx = static_cast<int>(i);
            return MessageListView::HoverTarget::Chip;
        }
    }
    if (g.add_visible && rect_contains(g.add_button, world))
    {
        return MessageListView::HoverTarget::AddButton;
    }
    if (g.retry_button.w > 0 && rect_contains(g.retry_button, world))
    {
        return MessageListView::HoverTarget::RetryButton;
    }
    if (g.abort_button.w > 0 && rect_contains(g.abort_button, world))
    {
        return MessageListView::HoverTarget::AbortButton;
    }
    for (std::size_t i = 0; i < g.receipt_discs.size(); ++i)
    {
        if (rect_contains(g.receipt_discs[i], world))
        {
            out_chip_idx = static_cast<int>(i);
            return MessageListView::HoverTarget::Receipt;
        }
    }
    return MessageListView::HoverTarget::None;
}

bool MessageListView::on_pointer_move(tk::Point local)
{
    if (gate_blocks_input_())
    {
        return false;
    }
    last_pointer_local_ = local;
    tk::ListView::on_pointer_move(local);

    // Map pan: apply drag delta while a pan is active.
    if (map_active_row_ != kNoMapRow && map_active_row_ < messages_.size())
    {
        float dx = local.x - map_drag_start_pt_.x;
        float dy = local.y - map_drag_start_pt_.y;
        float new_wp_x = map_drag_start_vp_px_.x - dx;
        float new_wp_y = map_drag_start_vp_px_.y - dy;
        int zoom = messages_[map_active_row_].map_viewport.zoom;
        auto [lat, lon] = world_px_to_lat_lon(new_wp_x, new_wp_y, zoom);
        messages_[map_active_row_].map_viewport.lat = lat;
        messages_[map_active_row_].map_viewport.lon = lon;
        if (!hover_link_url_.empty())
        {
            hover_link_url_.clear();
            if (on_link_hovered)
            {
                on_link_hovered("");
            }
        }
        invalidate_data();
        return true;
    }

    // Row hover may have changed; if the new hovered row is different
    // from the one we have geometry for, invalidate so paint_row will
    // rebuild it. (Paint will populate hovered_row_geom_ on its next
    // frame; until then chip hit-tests will return None.)
    int row = hovered_row_index();
    if (row < 0 || static_cast<std::size_t>(row) != hovered_row_geom_.row_index)
    {
        hovered_row_geom_.row_index = static_cast<std::size_t>(-1);
        hovered_row_geom_.chips.clear();
        hovered_row_geom_.receipt_discs.clear();
        hovered_row_geom_.add_visible = false;
        hovered_row_geom_.retry_button = {};
        hovered_row_geom_.abort_button = {};
    }
    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(), local, chip_idx);
    if (t != hover_target_ || chip_idx != hover_chip_idx_)
    {
        hover_target_ = t;
        hover_chip_idx_ = chip_idx;
    }

    // Inline hyperlink hover — detect URL under pointer and fire
    // on_link_hovered when it changes so the shell can set the cursor.
    // Also fires a synthetic token when hovering a map tile area so the
    // shell switches to the pointing-hand cursor there too.
    std::string new_link_url;
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        std::size_t hrow = hovered_row_geom_.row_index;
        if (hrow < messages_.size())
        {
            const auto& m = messages_[hrow];
            // Map hover: report as a non-empty token so the shell shows a
            // grab cursor. Use a sentinel that is never a real URL.
            if (m.kind == MessageRowData::Kind::Location)
            {
                constexpr float kMapRowH = 240.0f;
                const tk::Rect& rb = hovered_row_geom_.row_bounds;
                tk::Rect map_rect{rb.x, rb.y, rb.w, kMapRowH};
                if (rect_contains(map_rect, world))
                {
                    new_link_url = "map://";
                }
            }
            // Inline hyperlink (overrides map sentinel when a link is found).
            auto it = link_layout_cache_.find(m.event_id);
            if (it != link_layout_cache_.end() && it->second.layout)
            {
                tk::Point ll{world.x - it->second.origin.x,
                             world.y - it->second.origin.y};
                std::string lurl = it->second.layout->link_at(ll);
                if (!lurl.empty())
                {
                    new_link_url = std::move(lurl);
                }
            }
        }
        // File card hover: reuse on_link_hovered with a "file://" sentinel
        // so the shell switches to the pointing-hand cursor.
        if (new_link_url.empty())
        {
            for (const auto& [eid, hit] : file_geom_)
            {
                if (rect_contains(hit.world_rect, world))
                {
                    new_link_url = "file://";
                    break;
                }
            }
        }
        // URL preview card hover: clicking the card opens the URL in the
        // browser (see on_pointer_up's press_preview_ branch), so report
        // the URL on hover too — the shell switches to the pointing-hand
        // cursor the same way it does for inline hyperlinks.
        if (new_link_url.empty())
        {
            for (const auto& [eid, hit] : preview_card_geom_)
            {
                if (!hit.url.empty() && rect_contains(hit.rect, world))
                {
                    new_link_url = hit.url;
                    break;
                }
            }
        }
    }
    if (new_link_url != hover_link_url_)
    {
        hover_link_url_ = new_link_url;
        if (on_link_hovered)
        {
            on_link_hovered(hover_link_url_);
        }
    }

    // Location description tooltip: show when hovering over the map area.
    {
        bool want_tooltip = false;
        tk::Rect anchor{};
        int row = hovered_row_index();
        if (row >= 0 && static_cast<std::size_t>(row) < messages_.size())
        {
            const auto& m = messages_[static_cast<std::size_t>(row)];
            if (m.kind == MessageRowData::Kind::Location &&
                !m.location_description.empty())
            {
                auto it = map_rect_geom_.find(m.event_id);
                if (it != map_rect_geom_.end())
                {
                    tk::Point world{local.x + bounds().x, local.y + bounds().y};
                    if (rect_contains(it->second, world))
                    {
                        want_tooltip = true;
                        anchor = it->second;
                    }
                }
            }
        }
        if (want_tooltip && !map_tooltip_showing_)
        {
            map_tooltip_showing_ = true;
            const auto& m = messages_[static_cast<std::size_t>(row)];
            if (on_show_tooltip)
            {
                on_show_tooltip(m.location_description, anchor);
            }
        }
        else if (!want_tooltip && map_tooltip_showing_)
        {
            map_tooltip_showing_ = false;
            if (on_hide_tooltip)
            {
                on_hide_tooltip();
            }
        }
    }
    return true;
}

void MessageListView::on_pointer_leave()
{
    if (gate_blocks_input_())
    {
        return;
    }
    tk::ListView::on_pointer_leave();
    hovered_row_geom_.row_index = static_cast<std::size_t>(-1);
    hovered_row_geom_.chips.clear();
    hovered_row_geom_.receipt_discs.clear();
    hovered_row_geom_.add_visible = false;
    hovered_row_geom_.retry_button = tk::Rect{};
    hovered_row_geom_.abort_button = tk::Rect{};
    hover_target_ = HoverTarget::None;
    hover_chip_idx_ = -1;
    press_pill_ = false;
    if (map_tooltip_showing_)
    {
        map_tooltip_showing_ = false;
        if (on_hide_tooltip)
        {
            on_hide_tooltip();
        }
    }
}

bool MessageListView::should_show_pill() const
{
    if (historical_mode_)
    {
        return true;
    }
    if (content_height() <= bounds().h)
    {
        return false;
    }
    return scroll_y() + bounds().h + 1.0f < content_height();
}

bool MessageListView::scroll_to_event_id(const std::string& event_id)
{
    if (event_id.empty())
    {
        return false;
    }
    for (std::size_t i = 0; i < messages_.size(); ++i)
    {
        if (messages_[i].event_id == event_id)
        {
            scroll_to_index(static_cast<int>(i), /*align_top=*/false);
            return true;
        }
    }
    return false;
}

void MessageListView::set_historical_mode(bool historical)
{
    historical_mode_ = historical;
    invalidate_data();
}

int MessageListView::message_index_of(const std::string& event_id) const
{
    for (int i = 0; i < static_cast<int>(messages_.size()); ++i)
        if (messages_[i].event_id == event_id)
            return i;
    return -1;
}

std::optional<MessageListView::OrderedSel>
MessageListView::selection_ordered() const
{
    if (!sel_) return std::nullopt;
    int ai = message_index_of(sel_->anchor_event_id);
    int hi = message_index_of(sel_->head_event_id);
    if (ai < 0 || hi < 0) return std::nullopt;
    if (ai < hi || (ai == hi && sel_->anchor_byte <= sel_->head_byte))
        return OrderedSel{ai, sel_->anchor_byte, hi, sel_->head_byte};
    return OrderedSel{hi, sel_->head_byte, ai, sel_->anchor_byte};
}

bool MessageListView::has_selection() const
{
    if (!sel_) return false;
    if (sel_->anchor_event_id != sel_->head_event_id) return true;
    return sel_->anchor_byte != sel_->head_byte;
}

void MessageListView::copy_selection()
{
    if (!has_selection() || !on_set_clipboard)
        return;
    auto ord = selection_ordered();
    if (!ord) return;

    std::string result;
    for (int i = ord->lo_idx; i <= ord->hi_idx; ++i)
    {
        const auto& m = messages_[static_cast<std::size_t>(i)];
        auto it = link_layout_cache_.find(m.event_id);
        if (it == link_layout_cache_.end() || !it->second.layout)
            continue;
        int lo_b = (i == ord->lo_idx) ? ord->lo_byte : 0;
        int hi_b = (i == ord->hi_idx)
                       ? ord->hi_byte
                       : static_cast<int>(it->second.plain.size());
        std::string seg = it->second.layout->text_range(lo_b, hi_b);
        if (!result.empty() && result.back() != '\n')
            result += '\n';
        result += std::move(seg);
    }
    if (!result.empty())
        on_set_clipboard(result);
}

bool MessageListView::on_right_click(tk::Point /*local*/)
{
    if (!has_selection())
        return false;
    if (on_show_copy_menu)
        on_show_copy_menu();
    else
        copy_selection();
    return true;
}

bool MessageListView::on_pointer_down(tk::Point local)
{
    if (gate_blocks_input_())
    {
        return false; // list not painted yet
    }

    // Any new press outside a text-body selection clears the old selection.
    if (sel_ && !press_sel_)
    {
        sel_.reset();
        sel_is_dragging_ = false;
        if (request_repaint_)
            request_repaint_();
    }

    if (pill_visible_)
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        if (rect_contains(pill_rect_, world))
        {
            press_pill_ = true;
            return true;
        }
    }

    // Map pan: start drag on Kind::Location rows.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        std::size_t ri = hovered_row_geom_.row_index;
        if (ri < messages_.size() &&
            messages_[ri].kind == MessageRowData::Kind::Location)
        {
            constexpr float kMapRowH = 240.0f;
            const tk::Rect& rb = hovered_row_geom_.row_bounds;
            tk::Rect map_rect{rb.x, rb.y, rb.w, kMapRowH};
            if (rect_contains(map_rect, world))
            {
                map_active_row_ = ri;
                map_drag_start_pt_ = local;
                map_drag_start_vp_px_ =
                    lat_lon_to_world_px(messages_[ri].map_viewport.lat,
                                        messages_[ri].map_viewport.lon,
                                        messages_[ri].map_viewport.zoom);
                return true;
            }
        }
    }

    // Voice card hit-test — handled before chips because the voice
    // controls sit in the body block, separate from the trailing chips
    // strip. Check pill (small) then play button then waveform strip.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [event_id, geom] : voice_card_geom_)
        {
            if (geom.speed_pill.w > 0 && rect_contains(geom.speed_pill, world))
            {
                press_voice_event_id_ = event_id;
                press_voice_kind_ = VoicePressKind::SpeedPill;
                return true;
            }
            if (rect_contains(geom.play_button, world))
            {
                press_voice_event_id_ = event_id;
                press_voice_kind_ = VoicePressKind::PlayButton;
                return true;
            }
            if (rect_contains(geom.waveform_strip, world))
            {
                press_voice_event_id_ = event_id;
                press_voice_kind_ = VoicePressKind::Waveform;
                // Immediate seek on press for snappy scrub-start, then
                // subsequent on_pointer_drag callbacks follow the finger.
                for (const auto& row : messages_)
                {
                    if (row.event_id == event_id &&
                        row.kind == MessageRowData::Kind::Voice)
                    {
                        handle_voice_scrub_at(row, world.x);
                        break;
                    }
                }
                return true;
            }
        }
    }

    // Audio card hit-test — play button or progress track.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [event_id, geom] : audio_card_geom_)
        {
            if (rect_contains(geom.play_button, world))
            {
                press_audio_event_id_ = event_id;
                press_audio_kind_ = AudioPressKind::PlayButton;
                return true;
            }
            if (geom.progress_track.w > 0 &&
                rect_contains(geom.progress_track, world))
            {
                press_audio_event_id_ = event_id;
                press_audio_kind_ = AudioPressKind::ProgressTrack;
                for (const auto& row : messages_)
                {
                    if (row.event_id == event_id &&
                        row.kind == MessageRowData::Kind::Audio)
                    {
                        handle_audio_scrub_at(row, world.x);
                        break;
                    }
                }
                return true;
            }
        }
    }

    // Reply button hit-test — check before reaction chips so it has priority.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& rb = hovered_row_geom_.reply_button;
        if (rb.w > 0 && rect_contains(rb, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                press_reply_btn_ = true;
                press_reply_event_id_ = messages_[row].event_id;
                return true;
            }
        }
    }

    // Edit button hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& eb = hovered_row_geom_.edit_button;
        if (eb.w > 0 && rect_contains(eb, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                press_edit_btn_ = true;
                press_edit_event_id_ = messages_[row].event_id;
                return true;
            }
        }
    }

    // Delete button hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& db = hovered_row_geom_.delete_button;
        if (db.w > 0 && rect_contains(db, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                press_delete_btn_ = true;
                press_delete_event_id_ = messages_[row].event_id;
                return true;
            }
        }
    }

    // Retry/abort pending send buttons.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        if (hovered_row_geom_.retry_button.w > 0 &&
            rect_contains(hovered_row_geom_.retry_button, world))
        {
            std::size_t ri = hovered_row_geom_.row_index;
            if (ri < messages_.size())
            {
                press_retry_btn_ = true;
                press_pending_txn_id_ = messages_[ri].pending_txn_id;
            }
            return true;
        }
        if (hovered_row_geom_.abort_button.w > 0 &&
            rect_contains(hovered_row_geom_.abort_button, world))
        {
            std::size_t ri = hovered_row_geom_.row_index;
            if (ri < messages_.size())
            {
                press_abort_btn_ = true;
                press_pending_txn_id_ = messages_[ri].pending_txn_id;
            }
            return true;
        }
    }

    // Quote-block hit-test — lets the user jump to the original message.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [eid, rect] : quote_block_geom_)
        {
            if (rect_contains(rect, world))
            {
                press_quote_ = true;
                press_quote_event_id_ = eid;
                return true;
            }
        }
    }

    // URL preview card hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [eid, hit] : preview_card_geom_)
        {
            if (rect_contains(hit.rect, world))
            {
                press_preview_ = true;
                press_preview_url_ = hit.url;
                return true;
            }
        }
    }

    // Inline hyperlink hit-test — capture press if pointer lands on a link
    // glyph in a rich-text body (avoids swallowing the event for non-links).
    {
        press_link_url_.clear();
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        std::size_t row = hovered_row_geom_.row_index;
        if (row < messages_.size())
        {
            const auto& m = messages_[row];
            auto it = link_layout_cache_.find(m.event_id);
            if (it != link_layout_cache_.end() && it->second.layout)
            {
                tk::Point ll{world.x - it->second.origin.x,
                             world.y - it->second.origin.y};
                press_link_url_ = it->second.layout->link_at(ll);
                if (!press_link_url_.empty())
                {
                    return true;
                }
            }
        }
    }

    // Video thumbnail click-to-view hit-test (before image so it wins when
    // video_geom_ and image_geom_ happen to overlap for the same event_id).
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [eid, hit] : video_geom_)
        {
            if (rect_contains(hit.world_rect, world))
            {
                press_video_ = true;
                press_video_eid_ = eid;
                return true;
            }
        }
    }

    // Image / sticker click-to-view hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [eid, hit] : image_geom_)
        {
            if (rect_contains(hit.world_rect, world))
            {
                press_image_ = true;
                press_image_eid_ = eid;
                return true;
            }
        }
    }

    // File card click-to-download hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [eid, hit] : file_geom_)
        {
            if (rect_contains(hit.world_rect, world))
            {
                press_file_ = true;
                press_file_eid_ = eid;
                return true;
            }
        }
    }

    // Sender avatar / name hit-test. Resolves the row from the click point
    // (index_at) so we don't depend on hovered_row_geom_ being repopulated by
    // a paint pass after the last hover change. Continuation status comes
    // straight from Adapter::is_cont — the same function paint uses to decide
    // whether to draw the avatar+name strip at all — so the hit-test rect
    // exactly matches what's on screen, including the time-window regrouping
    // that the previous heuristic ignored.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        int ri_int = index_at(local);
        if (ri_int >= 0 && adapter_)
        {
            std::size_t ri = static_cast<std::size_t>(ri_int);
            if (ri < messages_.size())
            {
                const auto& m = messages_[ri];
                using Kind = MessageRowData::Kind;
                const bool is_virtual = m.kind == Kind::DaySeparator ||
                                        m.kind == Kind::ReadMarker ||
                                        m.kind == Kind::TimelineStart;
                if (!is_virtual && !adapter_->is_cont(ri) && !m.sender.empty())
                {
                    const tk::Rect rb = row_world_rect(ri_int);
                    if (rb.w > 0)
                    {
                        const tk::Rect avatar_rect{rb.x + kPadX, rb.y + kPadY,
                                                   kAvatarSize, kAvatarSize};
                        const float col_x = rb.x + kPadX + kAvatarSize + kAvatarGap;
                        const float sender_y =
                            rb.y + kPadY + (kAvatarSize - kSenderH) * 0.5f;
                        // Cap at 200px so clicking message body doesn't trigger.
                        const float name_max_w =
                            std::min(200.0f, std::max(0.0f, rb.w - kPadX -
                                                                 kAvatarSize -
                                                                 kAvatarGap - kPadX));
                        const tk::Rect sender_name_rect{col_x, sender_y,
                                                        name_max_w, kSenderH};
                        if (rect_contains(avatar_rect, world) ||
                            (sender_name_rect.w > 0 &&
                             rect_contains(sender_name_rect, world)))
                        {
                            press_sender_ = true;
                            press_sender_user_id_ = m.sender;
                            press_sender_display_name_ = m.sender_name;
                            press_sender_avatar_url_ = m.sender_avatar_url;
                            return true;
                        }
                    }
                }
            }
        }
    }

    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(), local, chip_idx);
    if (t == HoverTarget::None)
    {
        // Spoiler reveal + text-selection anchor: capture click on the row
        // at the click point. Uses index_at(local) instead of
        // hovered_row_geom_.row_index because on_pointer_move() invalidates
        // the geom to (size_t)-1 on every hovered-row change and only the
        // next paint repopulates it — so a click landing before that next
        // paint would read SIZE_MAX and silently skip both blocks.
        int row_at = index_at(local);
        std::size_t row =
            row_at >= 0 ? static_cast<std::size_t>(row_at) : messages_.size();
        if (row < messages_.size())
        {
            const auto& m = messages_[row];
            if (m.formatted_body.find("data-mx-spoiler") != std::string::npos &&
                revealed_spoilers_.count(m.event_id) == 0)
            {
                press_spoiler_ = true;
                press_spoiler_eid_ = m.event_id;
                return true;
            }

            // Text selection anchor: single/double/triple click on text bodies.
            if (m.kind == MessageRowData::Kind::Text ||
                m.kind == MessageRowData::Kind::Notice ||
                m.kind == MessageRowData::Kind::Emote)
            {
                tk::Point world{local.x + bounds().x, local.y + bounds().y};
                auto it = link_layout_cache_.find(m.event_id);
                if (it != link_layout_cache_.end() && it->second.layout)
                {
                    tk::Point ll{world.x - it->second.origin.x,
                                 world.y - it->second.origin.y};
                    int idx = it->second.layout->char_index_at(ll);
                    if (idx >= 0)
                    {
                        // Multi-click detection.
                        int64_t now_ms = steady_ms_now();
                        float dx = world.x - last_down_pt_.x;
                        float dy = world.y - last_down_pt_.y;
                        bool same_spot = (dx * dx + dy * dy) < 64.0f;
                        click_count_ = (now_ms - last_down_time_ms_ < 500 &&
                                        same_spot)
                                           ? click_count_ + 1 : 1;
                        last_down_time_ms_ = now_ms;
                        last_down_pt_ = world;

                        const std::string& plain = it->second.plain;
                        if (click_count_ >= 3)
                        {
                            auto [lo, hi] = line_range_in_text(plain, idx);
                            sel_ = Selection{m.event_id, lo, m.event_id, hi};
                            sel_is_dragging_ = true;
                        }
                        else if (click_count_ == 2)
                        {
                            auto [lo, hi] = word_range_in_text(plain, idx);
                            sel_ = Selection{m.event_id, lo, m.event_id, hi};
                            sel_is_dragging_ = true;
                        }
                        else
                        {
                            sel_ = Selection{m.event_id, idx, m.event_id, idx};
                            sel_is_dragging_ = false;
                        }
                        press_sel_ = true;
                        if (request_repaint_)
                            request_repaint_();
                        return true;
                    }
                }
            }
        }
        return tk::ListView::on_pointer_down(local);
    }
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size())
    {
        return tk::ListView::on_pointer_down(local);
    }
    press_target_ = t;
    press_chip_idx_ = chip_idx;
    press_event_id_ = messages_[row].event_id;
    return true;
}

void MessageListView::on_pointer_up(tk::Point local, bool inside_self)
{
    if (gate_blocks_input_())
    {
        return;
    }

    // Selection drag ended.
    if (press_sel_)
    {
        press_sel_ = false;
        if (!sel_is_dragging_)
        {
            // Single click on text body: clear any selection and propagate as
            // a normal message click so on_message_clicked still fires.
            std::string fire_eid =
                (inside_self && sel_)
                    ? sel_->anchor_event_id
                    : std::string{};
            sel_.reset();
            if (request_repaint_)
                request_repaint_();
            sel_is_dragging_ = false;
            tk::ListView::on_pointer_up(local, inside_self);
            if (!fire_eid.empty() && on_message_clicked)
                on_message_clicked(fire_eid);
            return;
        }
        sel_is_dragging_ = false;
        tk::ListView::on_pointer_up(local, inside_self);
        return;
    }

    // Map pan: end drag.
    if (map_active_row_ != kNoMapRow)
    {
        map_active_row_ = kNoMapRow;
        tk::ListView::on_pointer_up(local, inside_self);
        return;
    }

    if (press_spoiler_)
    {
        bool fire = inside_self && !press_spoiler_eid_.empty();
        std::string eid = std::move(press_spoiler_eid_);
        press_spoiler_ = false;
        press_spoiler_eid_.clear();
        if (fire)
        {
            revealed_spoilers_.insert(eid);
            invalidate_data();
        }
        return;
    }
    if (press_sender_)
    {
        std::string uid  = std::move(press_sender_user_id_);
        std::string name = std::move(press_sender_display_name_);
        std::string aurl = std::move(press_sender_avatar_url_);
        press_sender_ = false;
        press_sender_user_id_.clear();
        press_sender_display_name_.clear();
        press_sender_avatar_url_.clear();
        if (inside_self && on_sender_clicked)
        {
            on_sender_clicked(std::move(uid), std::move(name), std::move(aurl));
        }
        return;
    }
    if (press_pill_)
    {
        bool fire = inside_self;
        press_pill_ = false;
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(pill_rect_, world))
            {
                if (historical_mode_ && on_return_to_live)
                {
                    on_return_to_live();
                }
                else
                {
                    scroll_to_bottom();
                }
            }
        }
        return;
    }
    if (press_audio_kind_ != AudioPressKind::None)
    {
        AudioPressKind kind = press_audio_kind_;
        std::string ev = std::move(press_audio_event_id_);
        press_audio_kind_ = AudioPressKind::None;
        press_audio_event_id_.clear();
        if (!inside_self || ev.empty())
        {
            return;
        }
        // Track scrub already moved the playhead on pointer-down/drag.
        if (kind == AudioPressKind::ProgressTrack)
        {
            return;
        }
        // PlayButton — confirm pointer is still over the button.
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        auto it = audio_card_geom_.find(ev);
        if (it == audio_card_geom_.end())
        {
            return;
        }
        if (!rect_contains(it->second.play_button, world))
        {
            return;
        }
        for (const auto& row : messages_)
        {
            if (row.event_id == ev && row.kind == MessageRowData::Kind::Audio)
            {
                handle_audio_play_click(row);
                return;
            }
        }
        return;
    }
    if (press_voice_kind_ != VoicePressKind::None)
    {
        VoicePressKind kind = press_voice_kind_;
        std::string ev = std::move(press_voice_event_id_);
        press_voice_kind_ = VoicePressKind::None;
        press_voice_event_id_.clear();
        if (!inside_self || ev.empty())
        {
            return;
        }
        // Scrub presses already moved the playhead in-flight; nothing left
        // to confirm on release.
        if (kind == VoicePressKind::Waveform)
        {
            return;
        }

        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        auto it = voice_card_geom_.find(ev);
        if (it == voice_card_geom_.end())
        {
            return;
        }
        const auto& geom = it->second;
        const tk::Rect& target = (kind == VoicePressKind::PlayButton)
                                     ? geom.play_button
                                     : geom.speed_pill;
        if (target.w <= 0 || !rect_contains(target, world))
        {
            return;
        }
        if (kind == VoicePressKind::SpeedPill)
        {
            handle_voice_speed_click();
            return;
        }
        // PlayButton.
        for (const auto& row : messages_)
        {
            if (row.event_id == ev && row.kind == MessageRowData::Kind::Voice)
            {
                handle_voice_play_click(row);
                return;
            }
        }
        return;
    }
    if (press_reply_btn_)
    {
        bool fire = inside_self && !press_reply_event_id_.empty();
        std::string ev = std::move(press_reply_event_id_);
        press_reply_btn_ = false;
        press_reply_event_id_.clear();
        if (fire)
        {
            // Confirm the pointer is still over the reply button.
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& rb = hovered_row_geom_.reply_button;
            if (rb.w > 0 && rect_contains(rb, world))
            {
                // Find the row to get sender_name + body_preview.
                for (const auto& row : messages_)
                {
                    if (row.event_id == ev)
                    {
                        if (on_reply_requested)
                        {
                            on_reply_requested(ev, row.sender_name, row.body);
                        }
                        break;
                    }
                }
            }
        }
        return;
    }
    if (press_edit_btn_)
    {
        bool fire = inside_self && !press_edit_event_id_.empty();
        std::string ev = std::move(press_edit_event_id_);
        press_edit_btn_ = false;
        press_edit_event_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& eb = hovered_row_geom_.edit_button;
            if (eb.w > 0 && rect_contains(eb, world))
            {
                for (const auto& row : messages_)
                {
                    if (row.event_id == ev)
                    {
                        if (on_edit_requested)
                        {
                            on_edit_requested(ev, row.body);
                        }
                        break;
                    }
                }
            }
        }
        return;
    }
    if (press_delete_btn_)
    {
        bool fire = inside_self && !press_delete_event_id_.empty();
        std::string ev = std::move(press_delete_event_id_);
        press_delete_btn_ = false;
        press_delete_event_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& db = hovered_row_geom_.delete_button;
            if (db.w > 0 && rect_contains(db, world))
            {
                if (on_delete_requested)
                {
                    on_delete_requested(ev);
                }
            }
        }
        return;
    }
    if (press_retry_btn_)
    {
        bool fire = inside_self && !press_pending_txn_id_.empty();
        std::string txn = std::move(press_pending_txn_id_);
        press_retry_btn_ = false;
        press_pending_txn_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& rb = hovered_row_geom_.retry_button;
            if (rb.w > 0 && rect_contains(rb, world))
            {
                if (on_retry_send)
                {
                    on_retry_send(txn);
                }
            }
        }
        return;
    }
    if (press_abort_btn_)
    {
        bool fire = inside_self && !press_pending_txn_id_.empty();
        std::string txn = std::move(press_pending_txn_id_);
        press_abort_btn_ = false;
        press_pending_txn_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& ab = hovered_row_geom_.abort_button;
            if (ab.w > 0 && rect_contains(ab, world))
            {
                if (on_abort_send)
                {
                    on_abort_send(txn);
                }
            }
        }
        return;
    }
    if (press_quote_)
    {
        bool fire = inside_self && !press_quote_event_id_.empty();
        std::string ev = std::move(press_quote_event_id_);
        press_quote_ = false;
        press_quote_event_id_.clear();
        if (fire)
        {
            for (std::size_t i = 0; i < messages_.size(); ++i)
            {
                if (messages_[i].event_id == ev)
                {
                    const std::string& orig_id = messages_[i].in_reply_to_id;
                    if (orig_id.empty())
                    {
                        break;
                    }
                    for (std::size_t j = 0; j < messages_.size(); ++j)
                    {
                        if (messages_[j].event_id == orig_id)
                        {
                            scroll_to_index(static_cast<int>(j));
                            return;
                        }
                    }
                    if (on_scroll_to_original)
                    {
                        on_scroll_to_original(orig_id);
                    }
                    break;
                }
            }
        }
        return;
    }

    if (!press_link_url_.empty())
    {
        std::string url = std::move(press_link_url_);
        press_link_url_.clear();
        if (inside_self && on_link_clicked)
        {
            on_link_clicked(url);
        }
        return;
    }

    if (press_preview_)
    {
        bool fire = inside_self && !press_preview_url_.empty();
        std::string url = std::move(press_preview_url_);
        press_preview_ = false;
        press_preview_url_.clear();
        if (fire)
        {
            // Confirm pointer is still inside the card rect on release.
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            for (const auto& [eid, hit] : preview_card_geom_)
            {
                if (hit.url == url && rect_contains(hit.rect, world))
                {
                    if (on_link_clicked)
                    {
                        on_link_clicked(url);
                    }
                    break;
                }
            }
        }
        return;
    }

    if (press_video_)
    {
        bool fire = inside_self && !press_video_eid_.empty();
        std::string eid = std::move(press_video_eid_);
        press_video_ = false;
        press_video_eid_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            auto it = video_geom_.find(eid);
            if (it != video_geom_.end() &&
                rect_contains(it->second.world_rect, world) && on_video_clicked)
            {
                on_video_clicked(it->second);
            }
        }
        return;
    }

    if (press_image_)
    {
        bool fire = inside_self && !press_image_eid_.empty();
        std::string eid = std::move(press_image_eid_);
        press_image_ = false;
        press_image_eid_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            auto it = image_geom_.find(eid);
            if (it != image_geom_.end() &&
                rect_contains(it->second.world_rect, world) && on_image_clicked)
            {
                on_image_clicked(it->second);
            }
        }
        return;
    }

    if (press_file_)
    {
        bool fire = inside_self && !press_file_eid_.empty();
        std::string eid = std::move(press_file_eid_);
        press_file_ = false;
        press_file_eid_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            auto it = file_geom_.find(eid);
            if (it != file_geom_.end() &&
                rect_contains(it->second.world_rect, world) && on_file_clicked)
            {
                on_file_clicked(it->second);
            }
        }
        return;
    }

    if (press_target_ == HoverTarget::None)
    {
        tk::ListView::on_pointer_up(local, inside_self);
        return;
    }
    HoverTarget t = press_target_;
    int idx = press_chip_idx_;
    std::string ev = std::move(press_event_id_);
    press_target_ = HoverTarget::None;
    press_chip_idx_ = -1;
    press_event_id_.clear();
    if (!inside_self)
    {
        return;
    }

    if (t == HoverTarget::Chip)
    {
        // Confirm the release still lands on the same chip.
        int now_idx = -1;
        HoverTarget now_t =
            chip_hit_at(hovered_row_geom_, bounds(), local, now_idx);
        if (now_t != HoverTarget::Chip || now_idx != idx)
        {
            return;
        }
        // Resolve the row by the event_id captured at press, NOT by the
        // positional row_index painted into hovered_row_geom_: an SDK
        // insert/remove between paint and this release can shift rows, and
        // a stale positional index would read the reaction key off the
        // wrong message.
        if (ev.empty())
        {
            return;
        }
        const MessageRowData* mrow = nullptr;
        for (const auto& m : messages_)
        {
            if (m.event_id == ev)
            {
                mrow = &m;
                break;
            }
        }
        if (!mrow)
        {
            return;
        }
        const auto& reactions = mrow->reactions;
        if (idx < 0 || static_cast<std::size_t>(idx) >= reactions.size())
        {
            return;
        }
        if (on_reaction_toggled)
        {
            on_reaction_toggled(ev, reactions[idx].key);
        }
    }
    else if (t == HoverTarget::AddButton)
    {
        int now_idx = -1;
        HoverTarget now_t =
            chip_hit_at(hovered_row_geom_, bounds(), local, now_idx);
        if (now_t != HoverTarget::AddButton)
        {
            return;
        }
        if (on_add_reaction_requested)
        {
            on_add_reaction_requested(ev, hovered_row_geom_.add_button);
        }
    }
}

std::string MessageListView::newest_visible_real_event_id() const
{
    auto [first, last] = visible_range();
    // Clamp: the adapter count includes the virtual typing row at index
    // messages_.size(), so visible_range() can return last == messages_.size().
    int actual_last = std::min(last, static_cast<int>(messages_.size()) - 1);
    if (actual_last < 0)
    {
        return {};
    }
    using Kind = MessageRowData::Kind;
    for (int i = actual_last; i >= first; --i)
    {
        const auto& row = messages_[static_cast<std::size_t>(i)];
        if (row.kind != Kind::DaySeparator && row.kind != Kind::ReadMarker &&
            row.kind != Kind::TimelineStart && !row.event_id.empty())
        {
            return row.event_id;
        }
    }
    return {};
}

void MessageListView::maybe_notify_receipt_() const
{
    if (!on_receipt_needed)
    {
        return;
    }
    auto eid = newest_visible_real_event_id();
    if (eid.empty() || eid == last_receipt_event_id_)
    {
        return;
    }
    last_receipt_event_id_ = eid;
    on_receipt_needed(eid);
}

void MessageListView::paint(tk::PaintCtx& ctx)
{
    // Sticker, voice, quote, and video rects are rebuilt per-paint by Adapter::paint_row.
    // Clear here so entries scrolled offscreen don't linger.
    sticker_geom_.clear();
    image_geom_.clear();
    video_geom_.clear();
    file_geom_.clear();
    voice_card_geom_.clear();
    audio_card_geom_.clear();
    quote_block_geom_.clear();
    preview_card_geom_.clear();

    // Room-switch gate: hold the list invisible until the rows that will
    // be visible have their height-affecting content loaded + measured, so
    // the room appears once, already correct, instead of reflowing as
    // async media / preview cards arrive.
    if (room_switch_gate_)
    {
        // Heights must be measured to know the visible band; this also
        // re-snaps to the bottom as async content grows it.
        tk::ListView::ensure_measured(ctx);
        if (!room_switch_gate_->evaluated)
        {
            collect_gate_deps_();
            room_switch_gate_->evaluated = true;
        }
        if (!room_switch_gate_->pending.empty())
        {
            // Paint only the background tk::ListView::paint would draw,
            // then skip the rows + every overlay below.
            ctx.canvas.fill_rect(bounds(), ctx.theme.palette.sidebar_bg);
            return;
        }
        reveal_room_switch_gate_(); // deps resolved (or timed out)
    }

    tk::ListView::paint(ctx);
    maybe_notify_receipt_();

    // After paint_row() has repopulated hovered_row_geom_, re-evaluate the
    // hover target in case it became stale. This fixes the receipt-disc tooltip
    // on platforms (Qt6) where multiple pointer-move events can arrive before a
    // repaint: each move clears receipt_discs when entering a new row, so
    // chip_hit_at returns None even though the pointer is over a disc. Once
    // the geom is fresh we correct the state here so the tooltip paints in the
    // same frame without needing another mouse event.
    if (hover_target_ == HoverTarget::None &&
        hovered_row_geom_.row_index != static_cast<std::size_t>(-1))
    {
        int re_idx = -1;
        HoverTarget re_t =
            chip_hit_at(hovered_row_geom_, bounds(), last_pointer_local_, re_idx);
        if (re_t != HoverTarget::None)
        {
            hover_target_    = re_t;
            hover_chip_idx_  = re_idx;
        }
    }

    // Scroll-to-bottom pill — overlays the bottom-right corner of the
    // viewport when the user is not pinned to the live tail. Painted
    // before the chip tooltip so the tooltip (rare, hover-only) wins on
    // any geometric overlap. Click handling lives in on_pointer_*.
    pill_visible_ = should_show_pill();
    if (pill_visible_)
    {
        constexpr float kSz = 36.0f, kInsetR = 12.0f, kInsetB = 16.0f;
        tk::Rect v = bounds();
        pill_rect_ = {v.x + v.w - kSz - kInsetR, v.y + v.h - kSz - kInsetB, kSz,
                      kSz};
        auto bg = press_pill_ ? ctx.theme.palette.subtle_pressed
                              : ctx.theme.palette.chrome_bg;
        ctx.canvas.fill_rounded_rect(pill_rect_, kSz * 0.5f, bg);
        ctx.canvas.stroke_rounded_rect(pill_rect_, kSz * 0.5f,
                                       ctx.theme.palette.border, 1.0f);
        tk::TextStyle gs{};
        gs.role = tk::FontRole::UiSemibold;
        gs.wrap = false;
        auto glyph = ctx.factory.build_text("\xE2\x86\x93", gs); // U+2193 ↓
        if (glyph)
        {
            tk::Size sz = glyph->measure();
            ctx.canvas.draw_text(*glyph,
                                 {pill_rect_.x + (kSz - sz.w) * 0.5f,
                                  pill_rect_.y + (kSz - sz.h) * 0.5f},
                                 ctx.theme.palette.text_primary);
        }
    }
    else
    {
        pill_rect_ = {};
    }

    // Tooltip overlay: paint a small panel listing senders of the
    // hovered reaction chip, or the display name of the hovered read-receipt
    // disc. We paint after rows so the panel sits on top of subsequent rows.
    if (hover_target_ == HoverTarget::Receipt)
    {
        if (hover_chip_idx_ < 0)
        {
            return;
        }
        std::size_t row = hovered_row_geom_.row_index;
        if (row >= messages_.size())
        {
            return;
        }
        const auto& rrs = messages_[row].read_receipts;
        const std::size_t total = rrs.size();
        const std::size_t visible = std::min(total, kReceiptCap);
        if (static_cast<std::size_t>(hover_chip_idx_) >= visible)
        {
            return;
        }
        // receipt_discs[i] corresponds to rrs[total - 1 - i] (newest first).
        const auto& rr =
            rrs[total - 1 - static_cast<std::size_t>(hover_chip_idx_)];
        const std::string& label =
            rr.display_name.empty() ? rr.user_id : rr.display_name;

        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        auto layout = ctx.factory.build_text(label, st);
        if (!layout)
        {
            return;
        }
        tk::Size sz = layout->measure();

        constexpr float kTipPadX = 8.0f;
        constexpr float kTipPadY = 6.0f;
        float panel_w = sz.w + kTipPadX * 2;
        float panel_h = sz.h + kTipPadY * 2;

        tk::Rect disc = hovered_row_geom_.receipt_discs[hover_chip_idx_];
        tk::Rect view = bounds();

        float panel_y = disc.y - panel_h - 4.0f;
        if (panel_y < view.y)
        {
            panel_y = disc.y + disc.h + 4.0f;
        }
        float panel_x = disc.x + disc.w * 0.5f - panel_w * 0.5f;
        if (panel_x + panel_w > view.x + view.w)
        {
            panel_x = view.x + view.w - panel_w - 4.0f;
        }
        if (panel_x < view.x + 4.0f)
        {
            panel_x = view.x + 4.0f;
        }

        tk::Rect panel{panel_x, panel_y, panel_w, panel_h};
        ctx.canvas.fill_rounded_rect(panel, 6.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(panel, 6.0f, ctx.theme.palette.border,
                                       1.0f);
        ctx.canvas.draw_text(*layout, {panel.x + kTipPadX, panel.y + kTipPadY},
                             ctx.theme.palette.text_primary);
        return;
    }

    if (hover_target_ != HoverTarget::Chip)
    {
        return;
    }
    if (hover_chip_idx_ < 0)
    {
        return;
    }
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size())
    {
        return;
    }
    const auto& reactions = messages_[row].reactions;
    if (static_cast<std::size_t>(hover_chip_idx_) >= reactions.size())
    {
        return;
    }
    const auto& r = reactions[hover_chip_idx_];
    if (r.senders.empty())
    {
        return;
    }

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
    for (const auto& s : r.senders)
    {
        lines.push_back(s);
    }

    tk::TextStyle st{};
    st.role = tk::FontRole::Small;
    st.wrap = false;

    struct LineLayout
    {
        std::unique_ptr<tk::TextLayout> layout;
        tk::Size size{};
    };
    std::vector<LineLayout> ls;
    ls.reserve(lines.size());
    float max_w = 0.0f;
    float total_h = 0.0f;
    for (const auto& line : lines)
    {
        auto layout = ctx.factory.build_text(line, st);
        if (!layout)
        {
            return;
        }
        tk::Size sz = layout->measure();
        max_w = std::max(max_w, sz.w);
        total_h += sz.h;
        ls.push_back({std::move(layout), sz});
    }

    constexpr float kTipPadX = 8.0f;
    constexpr float kTipPadY = 6.0f;
    float panel_w = max_w + kTipPadX * 2;
    float panel_h = total_h + kTipPadY * 2;

    tk::Rect chip = hovered_row_geom_.chips[hover_chip_idx_];
    tk::Rect view = bounds();

    // Prefer above the chip; flip below if it would clip the top.
    float panel_y = chip.y - panel_h - 4.0f;
    if (panel_y < view.y)
    {
        panel_y = chip.y + chip.h + 4.0f;
    }
    float panel_x = chip.x;
    if (panel_x + panel_w > view.x + view.w)
    {
        panel_x = view.x + view.w - panel_w - 4.0f;
    }
    if (panel_x < view.x + 4.0f)
    {
        panel_x = view.x + 4.0f;
    }

    tk::Rect panel{panel_x, panel_y, panel_w, panel_h};
    ctx.canvas.fill_rounded_rect(panel, 6.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(panel, 6.0f, ctx.theme.palette.border, 1.0f);

    float y = panel.y + kTipPadY;
    for (const auto& line : ls)
    {
        ctx.canvas.draw_text(*line.layout, {panel.x + kTipPadX, y},
                             ctx.theme.palette.text_primary);
        y += line.size.h;
    }
}

} // namespace tesseract::views
