#include "MessageListView.h"
#include "html_spans.h"
#include "map_tiles.h"
#include "media_utils.h"

#include "icons.h"
#include "tk/hash_combine.h"
#include "tk/i18n.h"
#include "tk/loading_spinner.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tesseract::views
{

// ── Multi-click selection helpers ────────────────────────────────────────────

static std::string spans_to_plain(const std::vector<tk::TextSpan>& spans)
{
    std::string out;
    for (const auto& s : spans)
        out += s.is_image ? s.image_alt : s.text;
    return out;
}

// Section-aware hit-testing: check sections first, fall back to flat layout.
static std::string link_at_world(const LinkLayout& le, tk::Point world)
{
    if (!le.sections.empty())
    {
        for (const auto& sec : le.sections)
        {
            if (!sec.layout) continue;
            tk::Point ll{world.x - sec.origin.x, world.y - sec.origin.y};
            if (ll.y < 0.0f || ll.y >= sec.height) continue;
            std::string url = sec.layout->link_at(ll);
            if (!url.empty()) return url;
        }
        return {};
    }
    if (!le.layout) return {};
    tk::Point ll{world.x - le.origin.x, world.y - le.origin.y};
    // Qt's FuzzyHit hit-test (canvas_qpainter.cpp) matches the nearest
    // character regardless of distance, so an unbounded call here would
    // resolve clicks anywhere below a caption's origin (e.g. on the image
    // itself) to whatever link is nearest. Bound to the layout's own height,
    // matching the sections branch above.
    if (ll.y < 0.0f || ll.y >= le.layout->measure().h) return {};
    return le.layout->link_at(ll);
}

static int char_at_world(const LinkLayout& le, tk::Point world)
{
    if (!le.sections.empty())
    {
        for (const auto& sec : le.sections)
        {
            if (!sec.layout) continue;
            tk::Point ll{world.x - sec.origin.x, world.y - sec.origin.y};
            if (ll.y < 0.0f || ll.y >= sec.height) continue;
            int idx = sec.layout->char_index_at(ll);
            if (idx >= 0) return idx;
        }
        return -1;
    }
    if (!le.layout) return -1;
    return le.layout->char_index_at(
        {world.x - le.origin.x, world.y - le.origin.y});
}

// If `url` is a matrix.to *user* permalink, return the Matrix user id
// (e.g. "@alice:example.org"); otherwise return "". Trailing query/fragment
// (?via=…) is dropped and a leading percent-encoded '@' (%40) is decoded.
static std::string mention_user_id_from_url(const std::string& url)
{
    static const std::string kHttps = "https://matrix.to/#/";
    static const std::string kHttp = "http://matrix.to/#/";
    std::string rest;
    if (url.rfind(kHttps, 0) == 0)
        rest = url.substr(kHttps.size());
    else if (url.rfind(kHttp, 0) == 0)
        rest = url.substr(kHttp.size());
    else
        return {};
    if (auto q = rest.find('?'); q != std::string::npos)
        rest = rest.substr(0, q);
    if (rest.rfind("%40", 0) == 0)
        rest = "@" + rest.substr(3);
    return (!rest.empty() && rest.front() == '@') ? rest : std::string{};
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
    if (!ev.in_reply_to_image_url.empty())
    {
        row.in_reply_to_image_source =
            ev.in_reply_to_image_encrypted_json.empty()
                ? tesseract::MediaSource::plain(ev.in_reply_to_image_url)
                : tesseract::MediaSource::encrypted(ev.in_reply_to_image_url,
                                                    ev.in_reply_to_image_encrypted_json);
    }
    row.is_edited = ev.is_edited;

    row.thread_root_id = ev.thread_root_id;
    row.is_thread_root = ev.is_thread_root;
    row.thread_reply_count = ev.thread_reply_count;
    row.thread_latest_sender_name = ev.thread_latest_sender_name;
    row.thread_latest_body = ev.thread_latest_body;
    row.thread_latest_ts = ev.thread_latest_ts;

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
        row.file_name   = f.filename.empty() ? f.body : f.filename;
        row.file_size   = f.file_size;
        row.has_filename_caption = !f.filename.empty();
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
    case tesseract::EventType::Utd:
        row.kind = Kind::Utd;
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
    case tesseract::EventType::PinnedEvent:
        row.kind = Kind::PinnedEvent;
        break;
    case tesseract::EventType::CallNotification:
        row.kind = Kind::CallNotification;
        break;
    case tesseract::EventType::Membership:
    {
        row.kind = Kind::Membership;
        const auto& mem = static_cast<const tesseract::MembershipStateEvent&>(ev);
        row.membership_action = mem.action;
        row.membership_target_user_id = mem.target_user_id;
        row.membership_target_name = mem.target_display_name;
        row.membership_target_avatar_url = mem.target_avatar_url;
        break;
    }
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

bool is_membership_group_start(const std::vector<MessageRowData>& msgs,
                               std::size_t index)
{
    if (msgs[index].kind != MessageRowData::Kind::Membership)
        return false;
    if (index == 0)
        return true;
    const auto& prev = msgs[index - 1];
    return prev.kind != MessageRowData::Kind::Membership ||
           prev.membership_action != msgs[index].membership_action;
}

std::size_t membership_group_end(const std::vector<MessageRowData>& msgs,
                                 std::size_t start)
{
    std::size_t j = start + 1;
    while (j < msgs.size() &&
           msgs[j].kind == MessageRowData::Kind::Membership &&
           msgs[j].membership_action == msgs[start].membership_action)
    {
        ++j;
    }
    return j;
}

std::size_t membership_group_start_of(const std::vector<MessageRowData>& msgs,
                                      std::size_t index)
{
    while (!is_membership_group_start(msgs, index))
    {
        --index;
    }
    return index;
}

namespace
{

constexpr float kMsgListPadX = tesseract::visual::kSpaceMD;                  // 12
constexpr float kMsgListPadY = tesseract::visual::kMsgRowVerticalPad;        // 6
constexpr float kMsgListAvatarSize = tesseract::visual::kMsgAvatarSize;      // 32
constexpr float kMsgListAvatarGap = tesseract::visual::kMsgAvatarGap;        // 8
constexpr float kSenderH = tesseract::visual::kMsgSenderNameHeight;   // 16
constexpr float kTimestampH = tesseract::visual::kMsgTimestampHeight; // 14
constexpr float kMsgListChipPadX = 10.0f;

// Block-level rendering constants.
constexpr float kSectionGap       =  4.0f; // vertical gap between body sections
constexpr float kBlockquoteIndent = 12.0f; // x-indent per blockquote nesting level
constexpr float kBlockquoteBarW   =  3.0f; // width of the blockquote accent bar
constexpr float kBlockquoteBarGap =  6.0f; // gap between bar right edge and text
constexpr float kListIndent       = 20.0f; // x-indent per list nesting level
constexpr float kBulletGap        = 10.0f; // gap between bullet/number right edge and text
constexpr float kHeadingRuleGap   =  4.0f; // space below h1/h2 text before rule

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

// URL preview card height accounting. The card's internal layout dimensions
// (width / thumb / padding) live with the paint in UrlPreviewCardDisplay;
// these two drive the Adapter's row-height math only.
constexpr float kMsgListPreviewCardH = 72.0f;
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
constexpr float kMsgListAudioPlayBtnSize = 32.0f;
constexpr float kMsgListAudioCardPadX = 8.0f;
constexpr float kMsgListAudioTrackH = 4.0f; // progress track height
constexpr float kMsgListVoicePlayBtnSize = 32.0f;
constexpr float kMsgListVoiceCardPadX = 8.0f;
constexpr float kMsgListVoiceBarW = 3.0f;
constexpr float kMsgListVoiceBarGap = 2.0f;
constexpr float kMsgListVoiceBarMinH = 3.0f;     // placeholder bar height
constexpr float kMsgListVoiceDurationW = 40.0f;  // reserved for "0:00" label
constexpr float kMsgListVoiceSpeedPillW = 30.0f; // "1×" / "1.5×" / "2×"
constexpr float kMsgListVoiceSpeedPillH = 20.0f;

// Reply quote block — painted above the body block when m.has_reply().
constexpr float kQuoteBlockH = 44.0f;          // total height of the quote band
constexpr float kQuoteImageBlockH = kQuoteBlockH * 2.0f; // quote block for image replies
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

// Thread preview chip ("N replies — latest reply preview") — painted at the
// bottom of thread-root rows that have at least one reply. Click fires
// MessageListView::on_thread_preview_clicked.
constexpr float kThreadChipH = 28.0f;
constexpr float kThreadChipGap = 4.0f; // gap between row body and chip
constexpr float kThreadChipPadX = 10.0f;
constexpr float kThreadChipRadius = 6.0f;

// Virtual timeline item heights.
constexpr float kDaySepH = 28.0f;
constexpr float kReadMarkerH = 20.0f;
constexpr float kTimelineStartH = 20.0f;
constexpr float kPinnedEventH   = 24.0f;  // m.room.pinned_events state row
constexpr float kTypingRowH = 20.0f;

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
    char buf[48];  // worst case: two 20-digit uint64 fields + ':' + NUL
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
    return std::max(0.0f, row_width - kMsgListPadX - kMsgListAvatarSize - kMsgListAvatarGap - kMsgListPadX);
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
        return tk::tr("Today");
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
        return tk::tr("Yesterday");
    }
    if (now_t > t && static_cast<std::uint64_t>(now_t - t) < 7u * 86400u)
    {
        constexpr const char* kDays[] = {"Sunday",    "Monday",   "Tuesday",
                                         "Wednesday", "Thursday", "Friday",
                                         "Saturday"};
        return tk::tr(kDays[sep_tm.tm_wday]);
    }
    constexpr const char* kMonths[] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    std::string month_str = tk::tr(kMonths[sep_tm.tm_mon]);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %d, %d", month_str.c_str(),
                  sep_tm.tm_mday, sep_tm.tm_year + 1900);
    return std::string(buf);
}

// Per-event expanded-line phrase for a single m.room.member row, e.g.
// "Alice joined the room" or "Bob was removed by Alice". The "by {actor}"
// clause is included only when the sender differs from the target (i.e.
// an admin acted on someone else) — self-service transitions (join, leave,
// accept, reject, knock, retract) never show an actor since sender==target.
// Rust never sends English prose here (see membership_action_str in
// sdk/src/client/timeline_convert.rs) — this is the sole place that
// composes the display string, entirely through tk::tr()/tk::trf().
std::string membership_expanded_phrase(const MessageRowData& m)
{
    using A = tesseract::MembershipAction;
    const std::string t = m.membership_target_name.empty()
                              ? m.membership_target_user_id
                              : m.membership_target_name;
    const std::string s = m.sender_name.empty() ? m.sender : m.sender_name;
    const bool by_actor = m.sender != m.membership_target_user_id;
    switch (m.membership_action)
    {
    case A::Joined:
        return tk::trf(tk::tr("{0} joined the room"), {t});
    case A::Left:
        return tk::trf(tk::tr("{0} left the room"), {t});
    case A::Banned:
        return by_actor ? tk::trf(tk::tr("{0} was banned by {1}"), {t, s})
                        : tk::trf(tk::tr("{0} was banned"), {t});
    case A::Unbanned:
        return by_actor ? tk::trf(tk::tr("{0} was unbanned by {1}"), {t, s})
                        : tk::trf(tk::tr("{0} is no longer banned"), {t});
    case A::Kicked:
        return by_actor ? tk::trf(tk::tr("{0} was removed by {1}"), {t, s})
                        : tk::trf(tk::tr("{0} was removed"), {t});
    case A::Invited:
        return by_actor ? tk::trf(tk::tr("{0} was invited by {1}"), {t, s})
                        : tk::trf(tk::tr("{0} received an invitation"), {t});
    case A::KickedAndBanned:
        return by_actor
                   ? tk::trf(tk::tr("{0} was removed and banned by {1}"), {t, s})
                   : tk::trf(tk::tr("{0} was kicked and banned"), {t});
    case A::InvitationAccepted:
        return tk::trf(tk::tr("{0} has accepted the invitation"), {t});
    case A::InvitationRejected:
        return tk::trf(tk::tr("{0} has rejected the invitation"), {t});
    case A::InvitationRevoked:
        return by_actor
                   ? tk::trf(tk::tr("{0}'s invitation was revoked by {1}"), {t, s})
                   : tk::trf(tk::tr("{0}'s invitation was revoked"), {t});
    case A::Knocked:
        return tk::trf(tk::tr("{0} requested to join"), {t});
    case A::KnockAccepted:
        return by_actor
                   ? tk::trf(tk::tr("{0}'s request to join was approved by {1}"),
                            {t, s})
                   : tk::trf(tk::tr("{0}'s join request was approved"), {t});
    case A::KnockRetracted:
        return tk::trf(tk::tr("{0} withdrew their request to join"), {t});
    case A::KnockDenied:
        return by_actor
                   ? tk::trf(tk::tr("{0}'s request to join was denied by {1}"),
                            {t, s})
                   : tk::trf(tk::tr("{0}'s join request was denied"), {t});
    }
    return t;
}

// Build the "Alice, Bob and 3 others" style name list for a collapsed
// membership-group summary.
std::string membership_names_label(const std::vector<std::string>& names)
{
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names[0];
    if (names.size() == 2)
        return tk::trf(tk::tr("{0} and {1}"), {names[0], names[1]});
    const long others = static_cast<long>(names.size() - 2);
    return tk::trf(tk::trn("{0}, {1} and {2} other", "{0}, {1} and {2} others",
                          others),
                   {names[0], names[1], std::to_string(others)});
}

// Collapsed-summary phrase for a membership group, e.g. "Alice, Bob and 3
// others joined the room". Pluralised via tk::trn() on the member count.
std::string membership_summary_phrase(tesseract::MembershipAction action,
                                      const std::vector<std::string>& names)
{
    const std::string label = membership_names_label(names);
    const long n = static_cast<long>(names.size());
    using A = tesseract::MembershipAction;
    switch (action)
    {
    case A::Joined:
        return tk::trf(tk::tr("{0} joined the room"), {label});
    case A::Left:
        return tk::trf(tk::tr("{0} left the room"), {label});
    case A::Banned:
        return tk::trf(tk::trn("{0} was banned from the room",
                              "{0} were banned from the room", n),
                       {label});
    case A::Unbanned:
        return tk::trf(tk::trn("{0} was unbanned", "{0} were unbanned", n),
                       {label});
    case A::Kicked:
        return tk::trf(tk::trn("{0} was removed from the room",
                              "{0} were removed from the room", n),
                       {label});
    case A::Invited:
        return tk::trf(tk::trn("{0} was invited", "{0} were invited", n),
                       {label});
    case A::KickedAndBanned:
        return tk::trf(tk::trn("{0} was removed and banned",
                              "{0} were removed and banned", n),
                       {label});
    case A::InvitationAccepted:
        return tk::trf(tk::trn("{0} accepted the invitation",
                              "{0} accepted their invitations", n),
                       {label});
    case A::InvitationRejected:
        return tk::trf(tk::trn("{0} rejected the invitation",
                              "{0} rejected their invitations", n),
                       {label});
    case A::InvitationRevoked:
        return tk::trf(tk::trn("{0} had their invitation revoked",
                              "{0} had their invitations revoked", n),
                       {label});
    case A::Knocked:
        return tk::trf(tk::tr("{0} requested to join"), {label});
    case A::KnockAccepted:
        return tk::trf(tk::trn("{0}'s request to join was approved",
                              "{0}'s requests to join were approved", n),
                       {label});
    case A::KnockRetracted:
        return tk::trf(tk::trn("{0} cancelled their request to join",
                              "{0} cancelled their requests to join", n),
                       {label});
    case A::KnockDenied:
        return tk::trf(tk::trn("{0}'s request to join was denied",
                              "{0}'s requests to join were denied", n),
                       {label});
    }
    return label;
}

} // namespace

static bool is_virtual_event(MessageRowData::Kind k)
{
    using Kind = MessageRowData::Kind;
    return k == Kind::DaySeparator || k == Kind::ReadMarker ||
           k == Kind::TimelineStart || k == Kind::PinnedEvent ||
           k == Kind::CallNotification || k == Kind::Membership;
}

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
        for (std::size_t j = index + 1; j < owner_.messages_.size(); ++j)
        {
            if (!is_virtual_event(owner_.messages_[j].kind))
                return true;
        }
        return false;
    }

    // Returns true only if there is at least one real content row between
    // this index and the next DaySeparator (or the end of the list). Used to
    // suppress day separators that have no visible events beneath them, so
    // consecutive empty-day separators collapse to just the last one.
    bool has_content_before_next_separator(std::size_t index) const
    {
        for (std::size_t j = index + 1; j < owner_.messages_.size(); ++j)
        {
            const MessageRowData::Kind k = owner_.messages_[j].kind;
            if (k == MessageRowData::Kind::DaySeparator)
                return false;
            if (!is_virtual_event(k))
                return true;
        }
        return false;
    }

    // Thin delegates over owner_.messages_ — the actual logic lives in the
    // free functions of the same name (declared in MessageListView.h) so it
    // is unit-testable without a live MessageListView/Adapter.
    bool is_membership_group_start(std::size_t index) const
    {
        return tesseract::views::is_membership_group_start(owner_.messages_,
                                                            index);
    }
    std::size_t membership_group_end(std::size_t start) const
    {
        return tesseract::views::membership_group_end(owner_.messages_, start);
    }
    std::size_t membership_group_start_of(std::size_t index) const
    {
        return tesseract::views::membership_group_start_of(owner_.messages_,
                                                            index);
    }

    std::size_t count() const override
    {
        return owner_.messages_.size() + 1; // +1 for always-present typing row
    }

    // Stable identity so ListView's scroll anchor can relocate the anchored
    // row after a prepend or media-driven height change shifts indices. Real
    // events use their event_id. Virtual rows (separators, markers) and local
    // echoes without an id get a synthetic key combining their kind with the
    // nearest preceding real event_id — stable because a prepend only adds
    // rows above. If that still cannot be matched, locate_anchor_ falls back
    // to the captured index. The \x01 prefix keeps synthetic keys from
    // colliding with real event ids.
    std::string row_key(std::size_t index) const override
    {
        if (is_typing_index(index))
        {
            return "\x01typing";
        }
        if (index >= owner_.messages_.size())
        {
            return {};
        }
        const auto& m = owner_.messages_[index];
        if (!m.event_id.empty())
        {
            return m.event_id;
        }
        std::string nbr;
        for (std::size_t j = index; j-- > 0;)
        {
            if (!owner_.messages_[j].event_id.empty())
            {
                nbr = owner_.messages_[j].event_id;
                break;
            }
        }
        return "\x01" + std::to_string(static_cast<int>(m.kind)) + ":" +
               (nbr.empty() ? std::to_string(index) : nbr);
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
        if (is_virtual_event(curr.kind))
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
        if (is_virtual_event(prev.kind))
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

    // Rows here are not independent: a row's height depends on its predecessor
    // (continuation grouping via is_cont) and adjacent virtual rows' visibility
    // depends on neighbouring content (day separators, read markers). So a
    // change at `i` can also change the row after it and any virtual rows
    // immediately before it. Widen the targeted-invalidation span to cover
    // that bounded neighbourhood. (Rare visibility flips of a *distant* trailing
    // read marker self-heal on the next full rebuild — width/theme change or
    // set_messages — and any read-marker update forces a full rebuild anyway.)
    void height_dependency_span(std::size_t i, std::size_t& lo,
                                std::size_t& hi) const override
    {
        const auto& msgs = owner_.messages_;
        const std::size_t n = msgs.size();
        if (n == 0)
        {
            lo = 0;
            hi = 0;
            return;
        }
        if (i >= n)
        {
            i = n - 1;
        }
        auto is_virtual = [](MessageRowData::Kind k) { return is_virtual_event(k); };
        // Backward: consecutive virtual rows immediately before i (their
        // visibility flips exactly when content appears/disappears beside them).
        std::size_t a = i;
        while (a > 0 && is_virtual(msgs[a - 1].kind))
        {
            --a;
        }
        // Forward: the next content row's continuation depends on row i; step
        // over any virtual rows (a suppressed read marker is skipped by is_cont)
        // to reach it.
        std::size_t b = i + 1;
        while (b < n && is_virtual(msgs[b].kind))
        {
            ++b;
        }
        if (b < n)
        {
            ++b; // include that next content row
        }
        lo = a;
        hi = b;
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
        if (m.kind == Kind::PinnedEvent)
        {
            return kPinnedEventH;
        }
        if (m.kind == Kind::CallNotification)
        {
            return kPinnedEventH;
        }
        if (m.kind == Kind::Membership)
        {
            // Group-start rows are always one line tall (either the
            // collapsed summary or the first member's own expanded line).
            // Non-start rows are 0 height while their group is collapsed
            // (absorbed into the start row's summary) and one line tall
            // once expanded.
            if (is_membership_group_start(index))
            {
                return kPinnedEventH;
            }
            std::size_t start = membership_group_start_of(index);
            return owner_.membership_groups_.is_expanded(
                       owner_.messages_[start].event_id)
                       ? kPinnedEventH
                       : 0.0f;
        }
        bool cont = is_cont(index);
        float body_w = std::max(0.0f, body_text_max_width(width) -
                                          receipt_reserve_width(m));
        float body_h = measure_body_block_height(m, ctx, body_w);
        float eff_chip_h = chip_h();
        float chips_h = !m.reactions.empty() ? eff_chip_h : 0.0f;
        float top_pad = cont ? kContPadY : kMsgListPadY;
        float header_h = cont ? 0.0f : kMsgListAvatarSize;
        float raw_h = top_pad + header_h + body_h + chips_h + kMsgListPadY;
        // Continuation rows without reactions must be at least chip_h() tall so
        // the hover action buttons (same height as chip_h) fit without overflow.
        if (cont && chips_h == 0.0f)
        {
            raw_h = std::max(raw_h, chip_h());
        }
        // Thread preview chip ("N replies"): adds a small band under any
        // thread-root row that has at least one reply. Click fires
        // on_thread_preview_clicked.
        if (m.is_thread_root && m.thread_reply_count > 0)
        {
            raw_h += kThreadChipH + kThreadChipGap;
        }
        return raw_h;
    }

    static tk::Color sender_color(const std::string& name, tk::ThemeMode mode)
    {
        static constexpr tk::Color kLight[] = {
            tk::Color::rgb(0xC0392B), tk::Color::rgb(0xD35400),
            tk::Color::rgb(0x6E7D00), tk::Color::rgb(0x1E8449),
            tk::Color::rgb(0x117A65), tk::Color::rgb(0x1565C0),
            tk::Color::rgb(0x6A1B9A), tk::Color::rgb(0xAD1457),
        };
        static constexpr tk::Color kDark[] = {
            tk::Color::rgb(0xFF8A80), tk::Color::rgb(0xFFAB40),
            tk::Color::rgb(0xD4E157), tk::Color::rgb(0x69F0AE),
            tk::Color::rgb(0x4DD0E1), tk::Color::rgb(0x82B1FF),
            tk::Color::rgb(0xCE93D8), tk::Color::rgb(0xF48FB1),
        };
        const std::size_t h = std::hash<std::string>{}(name);
        if (mode == tk::ThemeMode::Dark)
            return kDark[h % std::size(kDark)];
        return kLight[h % std::size(kLight)];
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
        if (m.kind == Kind::PinnedEvent)
        {
            paint_pinned_event(m, ctx, bounds);
            return;
        }
        if (m.kind == Kind::CallNotification)
        {
            paint_call_notification(m, ctx, bounds);
            return;
        }
        if (m.kind == Kind::Membership)
        {
            if (is_membership_group_start(index))
            {
                std::size_t end = membership_group_end(index);
                bool single = (end - index) == 1;
                if (single || !owner_.membership_groups_.is_expanded(m.event_id))
                {
                    paint_membership_summary(index, end, ctx, bounds);
                }
                else
                {
                    paint_membership_line(m, ctx, bounds);
                }
            }
            else
            {
                std::size_t start = membership_group_start_of(index);
                if (owner_.membership_groups_.is_expanded(
                        owner_.messages_[start].event_id))
                {
                    paint_membership_line(m, ctx, bounds);
                }
            }
            return;
        }

        bool cont = is_cont(index);

        // Search match tint — subtle accent fill behind the row.
        // Painted before the hover highlight so hover layers on top.
        if (!owner_.search_match_ids_.empty() &&
            !m.event_id.empty() &&
            owner_.search_match_ids_.count(m.event_id))
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.accent.with_alpha(25));
        }

        if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.subtle_hover);
            owner_.hovered_row_geom_.row_index = index;
            owner_.hovered_row_geom_.row_bounds = bounds;
            owner_.hovered_row_geom_.chips.clear();
            owner_.hovered_row_geom_.receipt_discs.clear();
            owner_.hovered_row_geom_.add_button = tk::Rect{};
            owner_.hovered_row_geom_.add_visible = false;
            owner_.hovered_row_geom_.react_button = tk::Rect{};
            owner_.hovered_row_geom_.reply_button = tk::Rect{};
            owner_.hovered_row_geom_.edit_button = tk::Rect{};
            owner_.hovered_row_geom_.more_button = tk::Rect{};
            owner_.hovered_row_geom_.retry_button = tk::Rect{};
            owner_.hovered_row_geom_.abort_button = tk::Rect{};
        }

        // Avatar column centre — used both for painting and for the
        // hover timestamp (continuation rows skip the avatar itself).
        float avatar_cx = bounds.x + kMsgListPadX + kMsgListAvatarSize * 0.5f;
        float avatar_cy = bounds.y + kMsgListPadY + kMsgListAvatarSize * 0.5f;

        // Right-of-avatar column (same indent for cont + non-cont so
        // body text aligns with the row above in a continuation group).
        float col_x = bounds.x + kMsgListPadX + kMsgListAvatarSize + kMsgListAvatarGap;
        float col_w = std::max(0.0f, bounds.x + bounds.w - col_x - kMsgListPadX -
                                         receipt_reserve_width(m));

        if (!cont)
        {
            // Avatar disc / initials.
            const tk::Image* avatar = nullptr;
            if (owner_.avatar_provider_ && !m.sender_avatar_url.empty())
            {
                avatar = owner_.avatar_provider_(m.sender_avatar_url);
            }
            draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kMsgListAvatarSize,
                        m.sender_name.empty() ? m.sender : m.sender_name,
                        ctx.theme.palette.avatar_initials_bg,
                        ctx.theme.palette.avatar_initials_text);

            // Sender name — vertically centered against the avatar disc.
            float sender_y = bounds.y + kMsgListPadY + (kMsgListAvatarSize - kSenderH) * 0.5f;
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
                                     sender_color(m.sender, ctx.theme.mode));
            }
        }

        // Body block: below avatar for full rows, tight to top for continuations.
        float body_top =
            cont ? (bounds.y + kContPadY) : (bounds.y + kMsgListPadY + kMsgListAvatarSize);
        float cursor = body_top;
        cursor = paint_body_block(m, ctx, col_x, cursor, col_w);
        float eff_chip_h = chip_h();
        float eff_chip_r = eff_chip_h * 0.5f;

        // ── Action pill (top-right, all hovered rows) ───────────────────────
        // One compact pill of square cells — react / reply / thread / edit /
        // delete / pin — anchored to the top-right of the row. Each enabled
        // action is a w==h cell separated by a 1px divider; the whole strip
        // uses one rounded background and one outer stroke. The trailing +
        // chip at the end of the reactions strip (painted below) is a
        // secondary entry point to the same reaction picker.
        if (hovered && m.pending_state == MessageRowData::PendingState::None)
        {
            static_cache_.ensure(ctx.factory);

            // Cell list in display order. Conditions match the original
            // per-button gates (own/text for edit, own/non-redacted for
            // delete, can_pin_ for pin, thread visibility + MSC3440 for
            // thread). react is the leftmost cell on any row that can accept
            // reactions; reply is unconditional.
            // Lucide line icons, tinted to the secondary text colour so they
            // read as quiet affordances and adapt to the theme. Each cell holds
            // its IconCache + svg; the icon is drawn (centred) once the cell
            // rect is known below.
            constexpr float kActionIconPx = 16.0f;
            const tk::Color action_tint = ctx.theme.palette.text_secondary;
            struct ActionCell
            {
                tk::IconCache* cache;
                std::span<const std::uint8_t> svg;
                tk::Rect* geom_out;
            };
            std::vector<ActionCell> cells;
            cells.reserve(6);
            if (m.kind != MessageRowData::Kind::Redacted &&
                m.kind != MessageRowData::Kind::Utd)
            {
                cells.push_back({&ic_react_, kEmojiSvg,
                                 &owner_.hovered_row_geom_.react_button});
            }
            cells.push_back(
                {&ic_reply_, kReplySvg, &owner_.hovered_row_geom_.reply_button});
            const bool can_thread =
                m.is_thread_root ||
                (m.in_reply_to_id.empty() && m.thread_root_id.empty());
            if (m.kind != MessageRowData::Kind::Redacted &&
                owner_.thread_button_visible_ && can_thread)
            {
                cells.push_back({&ic_thread_, kThreadSvg,
                                 &owner_.hovered_row_geom_.thread_button});
            }
            if (m.is_own && m.kind == MessageRowData::Kind::Text)
            {
                cells.push_back({&ic_edit_, kEditSvg,
                                 &owner_.hovered_row_geom_.edit_button});
            }
            if (m.kind != MessageRowData::Kind::Redacted &&
                m.kind != MessageRowData::Kind::Utd &&
                (m.is_own || owner_.can_pin_ ||
                 m.pending_state == MessageRowData::PendingState::None))
            {
                cells.push_back({&ic_more_, kMoreSvg,
                                 &owner_.hovered_row_geom_.more_button});
            }

            if (!cells.empty())
            {
                const float cell_side = chip_h();
                const float div_w = 1.0f;
                const float pill_w =
                    cell_side * static_cast<float>(cells.size()) +
                    div_w * static_cast<float>(cells.size() - 1);
                const float pill_h = cell_side;
                const float pill_r = chip_radius();

                const float pill_y =
                    cont ? (bounds.y + (bounds.h - pill_h) * 0.5f)
                         : (bounds.y + kMsgListPadY +
                            (kMsgListAvatarSize - pill_h) * 0.5f);
                float right_edge = bounds.x + bounds.w - kMsgListPadX;
                if (!m.read_receipts.empty())
                {
                    const std::size_t n =
                        std::min(m.read_receipts.size(), kReceiptCap);
                    float cluster_w = kReceiptSize +
                                      static_cast<float>(n - 1) *
                                          kReceiptStride;
                    right_edge -= cluster_w + chip_gap();
                }
                right_edge -= pill_r;
                tk::Rect pill{right_edge - pill_w, pill_y, pill_w, pill_h};
                tk::Rect pill_visual{pill.x - pill_r, pill.y,
                                     pill.w + 2.0f * pill_r, pill.h};

                ctx.canvas.fill_rounded_rect(
                    pill_visual, pill_r, ctx.theme.palette.subtle_pressed);

                // Pointer in world-coords for per-cell hover detection.
                // The reply/edit/delete/pin/thread cells use press_*_btn_
                // rather than HoverTarget, so the hover indicator can't be
                // derived from hover_target_ — read the latest pointer
                // position directly.
                const tk::Point world_ptr{
                    owner_.last_pointer_local_.x + owner_.bounds().x,
                    owner_.last_pointer_local_.y + owner_.bounds().y};

                for (std::size_t i = 0; i < cells.size(); ++i)
                {
                    tk::Rect cell_rect{
                        pill.x +
                            static_cast<float>(i) * (cell_side + div_w),
                        pill.y, cell_side, pill.h};

                    if (rect_contains(cell_rect, world_ptr))
                    {
                        ctx.canvas.fill_rect(
                            cell_rect, ctx.theme.palette.subtle_pressed);
                    }

                    if (cells[i].cache)
                        cells[i].cache->draw(ctx.canvas, ctx.factory,
                                             cells[i].svg, cell_rect,
                                             kActionIconPx, action_tint);
                    *cells[i].geom_out = cell_rect;
                }

                // 1px dividers between cells. Use border colour at reduced
                // alpha so they read as subtle separators rather than the
                // pill's own outline.
                const tk::Color div_col =
                    ctx.theme.palette.border.with_alpha(static_cast<
                        std::uint8_t>(ctx.theme.palette.border.a * 0.6f));
                for (std::size_t i = 1; i < cells.size(); ++i)
                {
                    float x = pill.x +
                              static_cast<float>(i) * cell_side +
                              static_cast<float>(i - 1) * div_w;
                    ctx.canvas.fill_rect({x, pill.y + 2.0f, div_w,
                                          pill.h - 4.0f},
                                         div_col);
                }

                ctx.canvas.stroke_rounded_rect(
                    pill_visual, pill_r, ctx.theme.palette.border, 1.0f);
            }
        }

        // Disc centre Y for receipts. Receipts always overlay the row — they
        // never expand it. Default: centre in the bottom kMsgListPadY strip (cursor
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
                        est.role = tk::FontRole::ReactionEmoji;
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
                const bool show_count = (r.count > 1);
                tk::Size csz = show_count ? count_layout->measure() : tk::Size{};

                const tk::TextLayout* emoji_layout = nullptr;
                tk::Size esz{};
                float content_w;
                if (is_img)
                {
                    float img_side = eff_chip_h - kImgPad * 2;
                    content_w = img_side +
                                (show_count ? kChipInnerGap + csz.w : 0.0f);
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
                    content_w = esz.w +
                                (show_count ? kChipInnerGap + csz.w : 0.0f);
                }
                float w =
                    std::max(content_w + kMsgListChipPadX * 2, eff_chip_h + 8.0f);
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

                float left_x = pill.x + kMsgListChipPadX;
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
                    if (show_count)
                    {
                        ctx.canvas.draw_text(
                            *count_layout,
                            {left_x + img_side + kChipInnerGap, count_y}, text);
                    }
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
                    if (show_count)
                    {
                        ctx.canvas.draw_text(
                            *count_layout,
                            {left_x + esz.w + kChipInnerGap, count_y}, text);
                    }
                }
                if (hovered)
                {
                    owner_.hovered_row_geom_.chips.push_back(pill);
                }
                chip_x += w + chip_gap();
            }

            // Trailing "+" pseudo-chip: only painted while the row is
            // hovered. Reads as a discoverable affordance, not a real
            // reaction — muted background, subtle border. The other action
            // buttons live in the top-right action pill (painted above);
            // only the +react chip belongs at the end of the reactions row.
            // Redacted and UTD rows can't accept reactions, so the affordance
            // would be misleading on those.
            if (hovered && m.kind != MessageRowData::Kind::Utd &&
                m.kind != MessageRowData::Kind::Redacted)
            {
                static_cache_.ensure(ctx.factory);
                if (const auto* layout = static_cache_.plus.get())
                {
                    tk::Size sz = layout->measure();
                    float w = std::max(sz.w + kMsgListChipPadX * 2, eff_chip_h + 8.0f);
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
                        {pill.x + kMsgListChipPadX, pill.y + (pill.h - sz.h) * 0.5f},
                        ctx.theme.palette.text_secondary);
                    owner_.hovered_row_geom_.add_button = pill;
                    owner_.hovered_row_geom_.add_visible = true;
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

            float right_edge = bounds.x + bounds.w - kMsgListPadX;
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
                draw_avatar(ctx.canvas, img, centre, kReceiptSize,
                            rr.display_name.empty() ? rr.user_id
                                                    : rr.display_name,
                            ctx.theme.palette.avatar_initials_bg,
                            ctx.theme.palette.avatar_initials_text);
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
                    float pill_w = sz.w + kMsgListChipPadX;
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

                float right_edge = bounds.x + bounds.w - kMsgListPadX;
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
                        auto btn_lo = ctx.factory.build_text(tk::tr("Retry"), small_st);
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

        // Event timestamp, painted in the left column centred on the avatar
        // column. The first message of a group (the full row, which draws the
        // avatar) always shows it tucked beneath the disc; continuation rows
        // show it only on hover, centred against the message body line.
        if (!cont || hovered)
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
                    float ty;
                    if (cont)
                    {
                        // Centre against the first line of the message body
                        // (the only thing in a continuation row).
                        static_cache_.ensure(ctx.factory);
                        float line_h = static_cache_.body_line_h > 0.0f
                                           ? static_cache_.body_line_h
                                           : sz.h;
                        ty = bounds.y + kContPadY + (line_h - sz.h) * 0.5f;
                    }
                    else
                    {
                        ty = bounds.y + kMsgListPadY + kMsgListAvatarSize + 2.0f;
                    }
                    ctx.canvas.draw_text(*layout, {tx, ty},
                                         ctx.theme.palette.text_muted);
                }
            }
        }

        // Thread preview chip — drawn at the bottom of the row when this
        // row roots a thread with at least one reply. Records its world-
        // space rect in owner_.chip_hit_rects_ so on_pointer_down can fire
        // on_thread_preview_clicked when the user taps it.
        if (m.is_thread_root && m.thread_reply_count > 0)
        {
            paint_thread_chip(m, ctx, bounds, col_x, col_w);
        }
    }

    // Helper: render the "N replies — <latest sender>: <snippet>" chip and
    // register its hit rect. Called from paint_row when a row is a thread
    // root with reply_count > 0. Chip height/gap are reserved in
    // measure_row_height so the chip lives in row-local padding (no overlap
    // with the body block or the read-receipt cluster above).
    void paint_thread_chip(const MessageRowData& m, tk::PaintCtx& ctx,
                           tk::Rect bounds, float col_x, float col_w)
    {
        tk::Rect chip{col_x,
                      bounds.y + bounds.h - kMsgListPadY - kThreadChipH,
                      std::min(col_w, 360.0f),
                      kThreadChipH};
        if (chip.w <= 0.0f)
        {
            return;
        }

        // Background — slightly darker when the list is dimmed so the chip
        // stays distinguishable from the dim overlay.
        tk::Color bg = owner_.dimmed_
                           ? ctx.theme.palette.subtle_pressed
                           : ctx.theme.palette.subtle_hover;
        ctx.canvas.fill_rounded_rect(chip, kThreadChipRadius, bg);
        ctx.canvas.stroke_rounded_rect(chip, kThreadChipRadius,
                                       ctx.theme.palette.border, 1.0f);

        // Right-side reply-count label ("N replies"). Reserve its width
        // before laying out the left-side latest-reply preview.
        char count_buf[48];
        std::snprintf(count_buf, sizeof(count_buf),
                      m.thread_reply_count == 1 ? "%llu reply" : "%llu replies",
                      static_cast<unsigned long long>(m.thread_reply_count));
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Small;
        cs.wrap = false;
        auto count_layout = ctx.factory.build_text(count_buf, cs);
        float count_w = 0.0f;
        if (count_layout)
        {
            tk::Size csz = count_layout->measure();
            count_w = csz.w;
            float cx = chip.x + chip.w - kThreadChipPadX - csz.w;
            float cy = chip.y + (chip.h - csz.h) * 0.5f;
            ctx.canvas.draw_text(*count_layout, {cx, cy},
                                 ctx.theme.palette.text_secondary);
        }

        // Left-side preview: "<sender>: <body snippet…>".
        // Truncate the body to ~80 chars (UTF-8-safe: clip at byte 80 and back
        // off until we land on a UTF-8 start byte) before letting the text
        // backend ellipsise on width.
        std::string preview;
        if (!m.thread_latest_sender_name.empty())
        {
            preview = m.thread_latest_sender_name + ": ";
        }
        {
            std::string body = m.thread_latest_body;
            // Collapse newlines (chip is one line).
            for (char& c : body)
            {
                if (c == '\n' || c == '\r')
                {
                    c = ' ';
                }
            }
            constexpr std::size_t kMaxBodyBytes = 80;
            if (body.size() > kMaxBodyBytes)
            {
                std::size_t cut = kMaxBodyBytes;
                // Back off into a UTF-8 start byte.
                while (cut > 0 &&
                       (static_cast<unsigned char>(body[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                body.resize(cut);
                body += "...";
            }
            preview += body;
        }
        if (!preview.empty())
        {
            tk::TextStyle ps{};
            ps.role = tk::FontRole::Small;
            ps.wrap = false;
            ps.trim = tk::TextTrim::Ellipsis;
            float preview_max =
                std::max(0.0f, chip.w - 2.0f * kThreadChipPadX - count_w -
                                   (count_w > 0.0f ? 8.0f : 0.0f));
            ps.max_width = preview_max;
            auto p_layout = ctx.factory.build_text(preview, ps);
            if (p_layout)
            {
                tk::Size psz = p_layout->measure();
                float py = chip.y + (chip.h - psz.h) * 0.5f;
                ctx.canvas.draw_text(*p_layout,
                                     {chip.x + kThreadChipPadX, py},
                                     ctx.theme.palette.text_primary);
            }
        }

        owner_.chip_hit_rects_.push_back({m.event_id, chip});
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
        if (label_l > bounds.x + kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {bounds.x + kMsgListPadX, line_y, label_l - bounds.x - kMsgListPadX, 1.0f},
                ctx.theme.palette.border);
        }
        if (label_r < bounds.x + bounds.w - kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {label_r, line_y, bounds.x + bounds.w - kMsgListPadX - label_r, 1.0f},
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
        auto lo = ctx.factory.build_text(tk::tr("New messages"), st);
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
        if (lx > bounds.x + kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {bounds.x + kMsgListPadX, ly, lx - bounds.x - kMsgListPadX, 1.0f},
                ctx.theme.palette.accent);
        }
        if (rx < bounds.x + bounds.w - kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {rx, ly, bounds.x + bounds.w - kMsgListPadX - rx, 1.0f},
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
        auto lo = ctx.factory.build_text(tk::tr("Start of conversation"), st);
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

    void paint_pinned_event(const MessageRowData& m, tk::PaintCtx& ctx,
                            tk::Rect bounds) const
    {
        // "{sender_name} {body}" e.g. "Alice pinned a message"
        std::string label = m.sender_name.empty()
            ? m.body
            : m.sender_name + " " + m.body;
        if (label.empty())
        {
            return;
        }
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, bounds.w - kMsgListPadX * 2);
        auto lo = ctx.factory.build_text(label, st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        constexpr float kLabelPadX = 8.0f;
        float cx = bounds.x + bounds.w * 0.5f;
        float cy = bounds.y + kPinnedEventH * 0.5f;
        float label_l = cx - sz.w * 0.5f - kLabelPadX;
        float label_r = cx + sz.w * 0.5f + kLabelPadX;
        float line_y = std::round(cy);
        if (label_l > bounds.x + kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {bounds.x + kMsgListPadX, line_y, label_l - bounds.x - kMsgListPadX, 1.0f},
                ctx.theme.palette.border);
        }
        if (label_r < bounds.x + bounds.w - kMsgListPadX)
        {
            ctx.canvas.fill_rect(
                {label_r, line_y, bounds.x + bounds.w - kMsgListPadX - label_r, 1.0f},
                ctx.theme.palette.border);
        }
        ctx.canvas.draw_text(*lo, {cx - sz.w * 0.5f, cy - sz.h * 0.5f},
                             ctx.theme.palette.text_muted);
    }

    void paint_call_notification(const MessageRowData& m, tk::PaintCtx& ctx,
                                 tk::Rect bounds) const
    {
        const bool is_video = (m.body == "video");

        const std::string intent_label =
            is_video
                ? tk::tr("started a video call")
                : (m.body == "audio"
                       ? tk::tr("started a voice call")
                       : tk::tr("started a call"));
        const std::string label =
            m.sender_name.empty()
                ? intent_label
                : m.sender_name + " " + intent_label;

        constexpr float kIconSz    = 12.0f;
        constexpr float kIconGap   = 4.0f;
        constexpr float kLabelPadX = 8.0f;

        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, bounds.w - kMsgListPadX * 2 - kIconSz - kIconGap);
        auto lo = ctx.factory.build_text(label, st);
        if (!lo)
            return;

        const tk::Size sz      = lo->measure();
        const float content_w  = kIconSz + kIconGap + sz.w;
        const float cx         = bounds.x + bounds.w * 0.5f;
        const float cy         = bounds.y + kPinnedEventH * 0.5f;
        const float icon_x     = cx - content_w * 0.5f;
        const float text_x     = icon_x + kIconSz + kIconGap;
        const float label_l    = cx - content_w * 0.5f - kLabelPadX;
        const float label_r    = cx + content_w * 0.5f + kLabelPadX;
        const float line_y     = std::round(cy);

        if (label_l > bounds.x + kMsgListPadX)
            ctx.canvas.fill_rect(
                {bounds.x + kMsgListPadX, line_y, label_l - bounds.x - kMsgListPadX, 1.0f},
                ctx.theme.palette.border);
        if (label_r < bounds.x + bounds.w - kMsgListPadX)
            ctx.canvas.fill_rect(
                {label_r, line_y, bounds.x + bounds.w - kMsgListPadX - label_r, 1.0f},
                ctx.theme.palette.border);

        const tk::Rect icon_rect{icon_x, cy - kIconSz * 0.5f, kIconSz, kIconSz};
        if (is_video)
            ic_call_video_.draw(ctx.canvas, ctx.factory, kVideoSvg, icon_rect,
                                kIconSz, ctx.theme.palette.text_muted);
        else
            ic_call_phone_.draw(ctx.canvas, ctx.factory, kPhoneSvg, icon_rect,
                                kIconSz, ctx.theme.palette.text_muted);
        ctx.canvas.draw_text(*lo, {text_x, cy - sz.h * 0.5f},
                             ctx.theme.palette.text_muted);
    }

    // One expanded membership-group member: a small target avatar, the
    // per-event phrase (see membership_expanded_phrase), and the event
    // timestamp right-aligned. Left-aligned like a compact row header
    // (rather than the pinned/call-notification centred-banner style) so a
    // stack of these reads naturally as a small list.
    void paint_membership_line(const MessageRowData& m, tk::PaintCtx& ctx,
                               tk::Rect bounds) const
    {
        constexpr float kAvatarD = 18.0f;
        constexpr float kGap = 6.0f;
        const float cy = bounds.y + kPinnedEventH * 0.5f;

        const tk::Image* img = nullptr;
        if (owner_.avatar_provider_ && !m.membership_target_avatar_url.empty())
        {
            img = owner_.avatar_provider_(m.membership_target_avatar_url);
        }
        draw_avatar(ctx.canvas, img, {bounds.x + kMsgListPadX + kAvatarD * 0.5f, cy},
                    kAvatarD,
                    m.membership_target_name.empty()
                        ? m.membership_target_user_id
                        : m.membership_target_name,
                    ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        const float text_x = bounds.x + kMsgListPadX + kAvatarD + kGap;

        const std::string ts = format_hhmm(m.timestamp_ms);
        tk::TextStyle ts_st{};
        ts_st.role = tk::FontRole::Timestamp;
        auto ts_layout = ts.empty() ? nullptr : ctx.factory.build_text(ts, ts_st);
        const float ts_w =
            ts_layout ? ts_layout->measure().w + kMsgListPadX : 0.0f;

        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width =
            std::max(0.0f, bounds.x + bounds.w - kMsgListPadX - ts_w - text_x);
        auto lo = ctx.factory.build_text(membership_expanded_phrase(m), st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo, {text_x, cy - sz.h * 0.5f},
                                 ctx.theme.palette.text_muted);
        }
        if (ts_layout)
        {
            tk::Size tsz = ts_layout->measure();
            ctx.canvas.draw_text(
                *ts_layout,
                {bounds.x + bounds.w - kMsgListPadX - tsz.w, cy - tsz.h * 0.5f},
                ctx.theme.palette.text_muted);
        }
    }

    // Collapsed membership-group summary: up to 3 stacked target avatars
    // followed by a pluralised summary phrase built from every member's
    // name (e.g. "Alice, Bob and 3 others joined the room"). Covers rows
    // [start, end) of messages_, all sharing the same membership_action.
    void paint_membership_summary(std::size_t start, std::size_t end,
                                  tk::PaintCtx& ctx, tk::Rect bounds) const
    {
        const auto& msgs = owner_.messages_;
        constexpr float kAvatarD = 18.0f;
        constexpr float kStride = 12.0f;
        constexpr std::size_t kCap = 3;
        const std::size_t total = end - start;
        const std::size_t visible = std::min(total, kCap);
        const float cy = bounds.y + kPinnedEventH * 0.5f;

        std::vector<std::string> names;
        names.reserve(total);
        for (std::size_t i = start; i < end; ++i)
        {
            const auto& mm = msgs[i];
            names.push_back(mm.membership_target_name.empty()
                                 ? mm.membership_target_user_id
                                 : mm.membership_target_name);
        }

        float cx = bounds.x + kMsgListPadX + kAvatarD * 0.5f;
        for (std::size_t i = 0; i < visible; ++i)
        {
            const auto& mm = msgs[start + i];
            const tk::Image* img = nullptr;
            if (owner_.avatar_provider_ &&
                !mm.membership_target_avatar_url.empty())
            {
                img = owner_.avatar_provider_(mm.membership_target_avatar_url);
            }
            draw_avatar(ctx.canvas, img, {cx, cy}, kAvatarD, names[i],
                        ctx.theme.palette.avatar_initials_bg,
                        ctx.theme.palette.avatar_initials_text);
            cx += kStride;
        }

        const float text_x =
            bounds.x + kMsgListPadX + kAvatarD +
            (visible > 1 ? static_cast<float>(visible - 1) * kStride : 0.0f) +
            6.0f;

        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, bounds.x + bounds.w - kMsgListPadX - text_x);
        auto lo = ctx.factory.build_text(
            membership_summary_phrase(msgs[start].membership_action, names), st);
        if (lo)
        {
            tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo, {text_x, cy - sz.h * 0.5f},
                                 ctx.theme.palette.text_muted);
        }
    }

    // Trailing synthetic "X is typing…" row. Left-aligned + ellipsized
    // muted text, matching the look the old RoomView strip had.
    void paint_typing_row(tk::PaintCtx& ctx, tk::Rect bounds) const
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, bounds.w - kMsgListPadX * 2);
        auto lo = ctx.factory.build_text(owner_.typing_text_, st);
        if (!lo)
        {
            return;
        }
        tk::Size sz = lo->measure();
        ctx.canvas.draw_text(
            *lo, {bounds.x + kMsgListPadX, bounds.y + (kTypingRowH - sz.h) * 0.5f},
            ctx.theme.palette.text_muted);
    }

    // ── Message row paint helpers ─────────────────────────────────────────────

    float measure_body_block_height(const MessageRowData& m, tk::LayoutCtx& ctx,
                                    float col_w) const
    {
        const float qbh = m.has_reply_image() ? kQuoteImageBlockH : kQuoteBlockH;
        float quote_h = m.has_reply() ? (qbh + kQuoteGapAfter) : 0.0f;
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
            if (owner_.previews_.has_preview(m))
            {
                preview_h = kPreviewCardGapTop + kMsgListPreviewCardH;
            }
            return quote_h + th + badge_h + preview_h;
        }
        case MessageRowData::Kind::Redacted:
            return quote_h +
                   measure_text_height(
                       m.body.empty() ? std::string("(empty message)") : m.body,
                       ctx, col_w);

        case MessageRowData::Kind::Utd:
            return quote_h + measure_text_height(m.body, ctx, col_w);

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
                h += 4.0f + measure_body_text(m, ctx, col_w);
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
        {
            float h = kFileCardH;
            if (m.has_filename_caption && !m.body.empty())
                h += 4.0f + measure_body_text(m, ctx, col_w);
            return quote_h + h;
        }
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
                h += 4.0f + measure_body_text(m, ctx, col_w);
            }
            return quote_h + h;
        }
        case MessageRowData::Kind::Emote:
        {
            bool revealed = owner_.spoilers_.is_revealed(m.event_id);
            auto spans = build_emote_spans(
                m, revealed, ctx.theme.mode == tk::ThemeMode::Dark);
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
            if (owner_.previews_.has_preview(m))
            {
                preview_h = kPreviewCardGapTop + kMsgListPreviewCardH;
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
                    kMsgListPadY;
            }
            return quote_h + kMapRowH + desc_h;
        }
        // Virtual items are handled before this function is called.
        case MessageRowData::Kind::DaySeparator:
        case MessageRowData::Kind::ReadMarker:
        case MessageRowData::Kind::TimelineStart:
        case MessageRowData::Kind::PinnedEvent:
        case MessageRowData::Kind::CallNotification:
        case MessageRowData::Kind::Membership:
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
            if (!m.first_url.empty())
            {
                const auto* p = owner_.previews_.lookup(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    owner_.previews_.paint_card(m, *p, ctx, x, end_y, col_w);
                    end_y += kMsgListPreviewCardH;
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
            if (!m.first_url.empty())
            {
                const auto* p = owner_.previews_.lookup(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    owner_.previews_.paint_card(m, *p, ctx, x, end_y, col_w);
                    end_y += kMsgListPreviewCardH;
                }
            }
            return end_y;
        }
        case MessageRowData::Kind::Emote:
        {
            bool revealed = owner_.spoilers_.is_revealed(m.event_id);
            auto spans = build_emote_spans(
                m, revealed, ctx.theme.mode == tk::ThemeMode::Dark);
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
                    // Emote bodies aren't routed through body_layout_for; this
                    // entry exists only for link/selection hit-testing. Tag it
                    // with a fresh LRU tick so the shared cache's eviction does
                    // not reclaim a still-visible row's layout.
                    owner_.link_cache_.put_unkeyed(
                        m.event_id,
                        [&](LinkLayout& le)
                        {
                            le.layout = std::move(layout);
                            le.origin = {x, y};
                            le.plain = spans_to_plain(spans);
                            le.spans.clear();
                        });
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
            if (!m.first_url.empty())
            {
                const auto* p = owner_.previews_.lookup(m.first_url);
                if (p && p->has_content())
                {
                    end_y += kPreviewCardGapTop;
                    owner_.previews_.paint_card(m, *p, ctx, x, end_y, col_w);
                    end_y += kMsgListPreviewCardH;
                }
            }
            return end_y;
        }
        case MessageRowData::Kind::Redacted:
        {
            float h = paint_wrapped_text(tk::tr("Message deleted"), ctx, x, y, col_w,
                                         ctx.theme.palette.text_muted);
            return y + h;
        }
        case MessageRowData::Kind::Utd:
        {
            // `body` already carries the cause-aware reason from the Rust
            // converter (e.g. "🔒 Unable to decrypt"). Render muted, single
            // line — same style as the redacted tombstone above.
            float h = paint_wrapped_text(m.body, ctx, x, y, col_w,
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
            if (m.image_animated && !owner_.media_is_hidden_(m))
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
                float ch = paint_body_text(m, ctx, x, cursor, col_w,
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
            float cursor = y + kFileCardH;
            if (m.has_filename_caption && !m.body.empty())
            {
                cursor += 4.0f;
                float ch = paint_body_text(m, ctx, x, cursor, col_w,
                                           ctx.theme.palette.text_primary);
                cursor += ch;
            }
            return cursor;
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
                float ch = paint_body_text(m, ctx, x, cursor, col_w,
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
        case MessageRowData::Kind::PinnedEvent:
        case MessageRowData::Kind::CallNotification:
        case MessageRowData::Kind::Membership:
            break;
        }
        return y;
    }

    // Paint the reply quote block (left accent stripe + sender + snippet/image).
    // Returns the y-coordinate directly below the block.
    float paint_quote_block(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                            float y, float col_w) const
    {
        const float block_h = m.has_reply_image() ? kQuoteImageBlockH : kQuoteBlockH;
        tk::Rect card{x, y, col_w, block_h};

        // Record world-coord rect so on_pointer_down can hit-test it.
        owner_.quote_block_geom_[m.event_id] = card;

        // Background + border
        ctx.canvas.fill_rounded_rect(card, 4.0f,
                                     ctx.theme.palette.subtle_hover);
        ctx.canvas.stroke_rounded_rect(card, 4.0f, ctx.theme.palette.border,
                                       1.0f);

        // Left accent stripe (3 px, full height of the card, rounded left edge)
        tk::Rect stripe{x, y, kQuoteAccentW, block_h};
        ctx.canvas.fill_rounded_rect(stripe, 4.0f, ctx.theme.palette.accent);

        // Text column starts to the right of the stripe + gap
        float tx = x + kQuoteAccentW + kQuotePadX;
        float tw = std::max(0.0f, col_w - kQuoteAccentW - kQuotePadX * 2);

        const bool unresolved = m.in_reply_to_sender_name.empty();

        if (m.has_reply_image())
        {
            // Image reply: sender name at the top, image thumbnail below.
            constexpr float kPadT = 8.0f;
            constexpr float kGap  = 4.0f;
            constexpr float kPadB = 6.0f;

            const std::string sname =
                unresolved ? std::string{} : m.in_reply_to_sender_name;
            tk::TextStyle name_st{};
            name_st.role      = tk::FontRole::UiSemibold;
            name_st.trim      = tk::TextTrim::Ellipsis;
            name_st.max_width = tw;
            auto name_lo =
                sname.empty() ? nullptr : ctx.factory.build_text(sname, name_st);

            float name_h = name_lo ? name_lo->measure().h : 0.0f;
            if (name_lo)
                ctx.canvas.draw_text(*name_lo, {tx, y + kPadT},
                                     ctx.theme.palette.text_secondary);

            float img_y = y + kPadT + name_h + kGap;
            float img_h = std::max(0.0f, (y + block_h) - img_y - kPadB);
            tk::Rect img_rect{tx, img_y, tw, img_h};

            const auto* look = m.in_reply_to_image_source.get();
            const std::string fetch_key = look ? look->fetch_token() : std::string{};
            const tk::Image* img = nullptr;
            if (owner_.image_provider_ && !fetch_key.empty())
                img = owner_.image_provider_(fetch_key);

            if (img)
            {
                float iw = static_cast<float>(img->width());
                float ih = static_cast<float>(img->height());
                float s  = (iw > 0.0f && ih > 0.0f)
                               ? std::min(img_rect.w / iw, img_rect.h / ih)
                               : 1.0f;
                float dw = iw * s;
                float dh = ih * s;
                tk::Rect fit{img_rect.x,
                             img_rect.y + (img_rect.h - dh) * 0.5f, dw, dh};
                ctx.canvas.push_clip_rounded_rect(img_rect, 4.0f);
                ctx.canvas.draw_image(*img, fit);
                ctx.canvas.pop_clip();
            }
            else
            {
                ctx.canvas.fill_rounded_rect(img_rect, 4.0f,
                                             ctx.theme.palette.chrome_bg);
                ctx.canvas.stroke_rounded_rect(img_rect, 4.0f,
                                               ctx.theme.palette.border, 1.0f);
            }
        }
        else
        {
            // Text reply: vertically centre sender name + body snippet.
            // When the replied-to event isn't in the local timeline cache the
            // SDK sends empty sender_name + body — fall back to a single muted
            // placeholder line instead of showing the raw event id.
            const std::string sname =
                unresolved ? std::string{} : m.in_reply_to_sender_name;
            const std::string sbody =
                unresolved ? std::string("Original message unavailable")
                           : m.in_reply_to_body;

            tk::TextStyle name_st{};
            name_st.role      = tk::FontRole::UiSemibold;
            name_st.trim      = tk::TextTrim::Ellipsis;
            name_st.max_width = tw;
            auto name_lo =
                sname.empty() ? nullptr : ctx.factory.build_text(sname, name_st);

            tk::TextStyle body_st{};
            body_st.role      = tk::FontRole::Body;
            body_st.trim      = tk::TextTrim::Ellipsis;
            body_st.max_width = tw;
            auto body_lo =
                sbody.empty() ? nullptr : ctx.factory.build_text(sbody, body_st);

            constexpr float kLineGap = 2.0f;
            float name_h  = name_lo ? name_lo->measure().h : 0.0f;
            float body_h  = body_lo ? body_lo->measure().h : 0.0f;
            float total_h = name_h + (body_h > 0.0f ? kLineGap + body_h : 0.0f);
            float text_y  = y + (block_h - total_h) * 0.5f;

            if (name_lo)
                ctx.canvas.draw_text(*name_lo, {tx, text_y},
                                     ctx.theme.palette.text_secondary);
            if (body_lo)
                ctx.canvas.draw_text(*body_lo, {tx, text_y + name_h + kLineGap},
                                     ctx.theme.palette.text_muted);
        }

        return y + block_h;
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

    // Split a single TextSpan into sub-spans at emoji/text boundaries so that
    // emoji grapheme clusters can be rendered at FontRole::InlineEmoji size.
    // Code and code_block spans are returned unsplit (monospace stays body size).
    // All formatting properties (bold, colour, url, etc.) are inherited by each
    // sub-span; only `is_emoji_run` differs between them.
    static std::vector<tk::TextSpan> segment_emoji_runs(const tk::TextSpan& src)
    {
        if (src.code || src.code_block || src.text.empty())
            return {src};

        // Decode UTF-8 to codepoints, recording the byte offset after each.
        struct CpEntry { uint32_t cp; std::size_t byte_end; };
        std::vector<CpEntry> cps;
        cps.reserve(src.text.size());
        const auto* p    = reinterpret_cast<const unsigned char*>(src.text.data());
        const auto* end  = p + src.text.size();
        const auto* base = p;
        while (p < end)
        {
            uint32_t cp;
            const unsigned char c = *p;
            if      (c < 0x80)                              { cp = c; p += 1; }
            else if ((c & 0xE0) == 0xC0 && p+1 < end)      { cp = uint32_t(c&0x1F)<<6  |(p[1]&0x3F);                                          p += 2; }
            else if ((c & 0xF0) == 0xE0 && p+2 < end)      { cp = uint32_t(c&0x0F)<<12 |uint32_t(p[1]&0x3F)<<6 |(p[2]&0x3F);                  p += 3; }
            else if ((c & 0xF8) == 0xF0 && p+3 < end)      { cp = uint32_t(c&0x07)<<18 |uint32_t(p[1]&0x3F)<<12|uint32_t(p[2]&0x3F)<<6|(p[3]&0x3F); p += 4; }
            else                                             { cp = c; p += 1; }
            cps.push_back({cp, static_cast<std::size_t>(p - base)});
        }

        // True for codepoints that always attach to the preceding cluster.
        auto is_cluster_cont = [](uint32_t cp) -> bool {
            return cp == 0x200D ||
                   (cp >= 0xFE00 && cp <= 0xFE0F) ||
                   cp == 0x20E3 ||
                   (cp >= 0x1F3FB && cp <= 0x1F3FF);
        };
        // Emoji base codepoints (same ranges as is_emoji_only above).
        auto is_emoji_base = [](uint32_t cp) -> bool {
            return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
                   cp == 0x2122 || cp == 0x2139 ||
                   (cp >= 0x2194 && cp <= 0x2199) || (cp >= 0x21A9 && cp <= 0x21AA) ||
                   (cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 ||
                   cp == 0x23CF || (cp >= 0x23E9 && cp <= 0x23FA) ||
                   cp == 0x24C2 || (cp >= 0x25AA && cp <= 0x25FE) ||
                   (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2934 && cp <= 0x2935) ||
                   (cp >= 0x2B05 && cp <= 0x2B55) || cp == 0x3030 ||
                   cp == 0x303D || cp == 0x3297 || cp == 0x3299 || cp == 0x1F004 ||
                   cp == 0x1F0CF || (cp >= 0x1F170 && cp <= 0x1F171) ||
                   (cp >= 0x1F17E && cp <= 0x1F17F) || cp == 0x1F18E ||
                   (cp >= 0x1F191 && cp <= 0x1F19A) || (cp >= 0x1F1E0 && cp <= 0x1F1FF) ||
                   (cp >= 0x1F201 && cp <= 0x1F251) ||
                   (cp >= 0x1F300 && cp <= 0x1F9FF) ||
                   (cp >= 0x1FA00 && cp <= 0x1FAFF);
        };

        // Build runs: (is_emoji, byte_end_of_last_cp_in_run).
        // Cluster-continuation codepoints extend the current run.
        // Keycap sequences ([0-9*#] + optional FE0F + 20E3) count as emoji.
        struct Run { bool emoji; std::size_t end; };
        std::vector<Run> runs;
        std::size_t i = 0;
        while (i < cps.size())
        {
            const uint32_t cp = cps[i].cp;
            if (is_cluster_cont(cp))
            {
                if (runs.empty())
                    runs.push_back({false, cps[i].byte_end});
                else
                    runs.back().end = cps[i].byte_end;
                ++i;
                continue;
            }
            if ((cp >= 0x30 && cp <= 0x39) || cp == 0x2A || cp == 0x23)
            {
                std::size_t j = i + 1;
                if (j < cps.size() && cps[j].cp == 0xFE0F) ++j;
                if (j < cps.size() && cps[j].cp == 0x20E3)
                {
                    if (!runs.empty() && runs.back().emoji)
                        runs.back().end = cps[j].byte_end;
                    else
                        runs.push_back({true, cps[j].byte_end});
                    i = j + 1;
                    continue;
                }
            }
            const bool emoji = is_emoji_base(cp);
            if (!runs.empty() && runs.back().emoji == emoji)
                runs.back().end = cps[i].byte_end;
            else
                runs.push_back({emoji, cps[i].byte_end});
            ++i;
        }

        if (runs.empty())
            return {src};
        // Single run — no split needed; just tag if it's all emoji.
        if (runs.size() == 1)
        {
            if (!runs[0].emoji)
                return {src};
            tk::TextSpan r = src;
            r.is_emoji_run = true;
            return {r};
        }

        std::vector<tk::TextSpan> result;
        result.reserve(runs.size());
        std::size_t byte_pos = 0;
        for (const auto& run : runs)
        {
            tk::TextSpan sp  = src;
            sp.text          = src.text.substr(byte_pos, run.end - byte_pos);
            sp.is_emoji_run  = run.emoji;
            result.push_back(std::move(sp));
            byte_pos = run.end;
        }
        return result;
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
                                            bool revealed, bool dark) const
    {
        auto spans = html_to_spans(m.formatted_body, dark);
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
        substitute_image_placeholders(spans);
        // Segment each span into emoji/text sub-spans for InlineEmoji sizing.
        std::vector<tk::TextSpan> segmented;
        segmented.reserve(spans.size());
        for (const auto& sp : spans)
            for (auto& sub : segment_emoji_runs(sp))
                segmented.push_back(std::move(sub));
        return segmented;
    }

    // Expand a span vector in-place, splitting each span at emoji/text
    // boundaries so backends can render emoji runs at InlineEmoji size.
    static void apply_emoji_segmentation(std::vector<tk::TextSpan>& spans)
    {
        std::vector<tk::TextSpan> out;
        out.reserve(spans.size());
        for (const auto& sp : spans)
            for (auto& sub : segment_emoji_runs(sp))
                out.push_back(std::move(sub));
        spans = std::move(out);
    }

    // html_spans.cpp leaves an is_image span's text empty (it carries no
    // text content of its own — only image_mxc/image_alt). Give it exactly
    // one U+FFFC OBJECT REPLACEMENT CHARACTER — the standard Unicode
    // convention for "one code unit stands in for an embedded object" (this
    // codebase already uses it the same way for RichEdit's embedded OLE
    // objects) — as a carrier code unit for each backend's native inline-
    // object mechanism (IDWriteInlineObject / PangoAttrShape /
    // QTextObjectInterface / CTRunDelegate) to attach to in build_rich_text.
    // That mechanism reserves an app-defined box and never considers any
    // font's fallback glyph for the range, so nothing is ever drawn there
    // by the normal text pass — paint_span_images draws the resolved bitmap
    // into the same box afterward. This must run before
    // apply_emoji_segmentation()/segment_emoji_runs(): U+FFFC isn't
    // classified as an emoji codepoint, so segment_emoji_runs' early-return
    // path leaves the span otherwise untouched, which is what we want here
    // (sizing comes from the inline object's metrics, not FontRole::
    // InlineEmoji's font-size bump).
    static void substitute_image_placeholders(std::vector<tk::TextSpan>& spans)
    {
        for (auto& sp : spans)
        {
            if (sp.is_image && sp.text.empty())
            {
                sp.text = "\xEF\xBF\xBC"; // U+FFFC OBJECT REPLACEMENT CHARACTER
            }
        }
    }

    std::vector<BodyBlock> prepare_blocks(const MessageRowData& m,
                                          bool revealed, bool dark) const
    {
        auto blocks = html_to_blocks(m.formatted_body, dark);
        if (!revealed)
        {
            for (auto& block : blocks)
                for (auto& sp : block.spans)
                {
                    if (!sp.spoiler)
                        continue;
                    sp.text = sp.spoiler_reason.empty()
                                  ? "[Spoiler]"
                                  : "[Spoiler: " + sp.spoiler_reason + "]";
                    sp.bold          = true;
                    sp.italic        = false;
                    sp.strikethrough = false;
                    sp.url           = {};
                }
        }
        for (auto& block : blocks)
            substitute_image_placeholders(block.spans);
        for (auto& block : blocks)
            apply_emoji_segmentation(block.spans);
        return blocks;
    }

    // Build italic TextSpan vector for an m.emote row:
    // "* SenderName body_text" with every span forced italic.
    std::vector<tk::TextSpan> build_emote_spans(const MessageRowData& m,
                                                bool revealed, bool dark) const
    {
        const std::string prefix =
            "* " + (m.sender_name.empty() ? m.sender : m.sender_name) + " ";
        std::vector<tk::TextSpan> result;
        if (!m.formatted_body.empty())
        {
            auto body = prepare_spans(m, revealed, dark);
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


    // Hash the inputs that determine a body's shaped text (so an edit or a
    // formatting change invalidates the cached layout).
    static std::size_t hash_body(const MessageRowData& m)
    {
        std::size_t h = std::hash<std::string>{}(m.formatted_body);
        std::size_t b = std::hash<std::string>{}(m.body);
        return tk::hash_combine(h, b);
    }

    // Build-or-reuse the shaped body layout for a message. Both the measure
    // and paint paths funnel through here so a body is shaped at most once per
    // (content, width, theme, spoiler) state and shared across repaints. The
    // returned entry is owned by link_cache_ (also used for link/selection
    // hit-testing); callers must not move its layout out.
    //
    // The cache (validity-key check, LRU bump + eviction, max size) lives in
    // LinkLayoutCache; this method supplies the build pipeline as the builder
    // callback so the span machinery stays on the view.
    // Compute the x-indentation for a block section.
    static float section_x_offset(const BodyBlock& b)
    {
        switch (b.kind)
        {
        case BodyBlock::Kind::Blockquote:
            return static_cast<float>(b.level) * kBlockquoteIndent;
        case BodyBlock::Kind::UnorderedItem:
        case BodyBlock::Kind::OrderedItem:
            return static_cast<float>(b.level) * kListIndent;
        default:
            return 0.0f;
        }
    }

    LinkLayout& body_layout_for(const MessageRowData& m, tk::CanvasFactory& f,
                                float w, bool dark, bool revealed) const
    {
        LinkLayoutCache::Key key{w, dark, revealed, hash_body(m)};
        return owner_.link_cache_.get_or_build(
            m.event_id, key,
            [&](LinkLayout& slot)
            {
                const bool eo = is_emoji_only(m.body);
                slot.layout.reset();
                slot.spans.clear();
                slot.plain.clear();
                slot.sections.clear();

                if (!m.formatted_body.empty())
                {
                    auto blocks = prepare_blocks(m, revealed, dark);

                    // Use block-section path when there is real block structure
                    // (more than a single Paragraph, or a non-Paragraph block).
                    bool has_structure = false;
                    if (blocks.size() > 1)
                        has_structure = true;
                    else if (blocks.size() == 1 &&
                             blocks[0].kind != BodyBlock::Kind::Paragraph)
                        has_structure = true;

                    if (has_structure)
                    {
                        float y_off = 0.0f;
                        std::string plain_all;
                        for (auto& b : blocks)
                        {
                            float xo  = section_x_offset(b);
                            float avw = std::max(1.0f, w - xo);
                            SectionLayout sec;
                            sec.kind     = b.kind;
                            sec.level    = b.level;
                            sec.index    = b.index;
                            sec.x_offset = xo;
                            sec.y_offset = y_off;
                            sec.spans    = b.spans;
                            sec.layout =
                                f.build_rich_text(b.spans, body_style(avw));
                            if (!sec.layout)
                                continue;
                            float sh = sec.layout->measure().h;
                            // h1/h2 need extra room for the horizontal rule.
                            if (b.kind == BodyBlock::Kind::Heading &&
                                b.level <= 2)
                                sh += kHeadingRuleGap + 1.0f;
                            sec.height = sh;
                            y_off += sh + kSectionGap;

                            // Plain text for clipboard — annotate structure.
                            switch (b.kind)
                            {
                            case BodyBlock::Kind::UnorderedItem:
                                plain_all += "• ";
                                break;
                            case BodyBlock::Kind::OrderedItem:
                                plain_all += std::to_string(b.index) + ". ";
                                break;
                            case BodyBlock::Kind::Blockquote:
                                plain_all +=
                                    std::string(
                                        static_cast<std::size_t>(b.level),
                                        '>') +
                                    " ";
                                break;
                            default:
                                break;
                            }
                            plain_all += spans_to_plain(b.spans);
                            plain_all += '\n';
                            slot.sections.push_back(std::move(sec));
                        }
                        if (!slot.sections.empty())
                        {
                            slot.plain = std::move(plain_all);
                            // Leave slot.layout null; sections path is active.
                            return;
                        }
                    }

                    // Fallback: single-block or empty — use flat span path.
                    if (!blocks.empty())
                    {
                        auto& b = blocks.front();
                        if (!b.spans.empty())
                        {
                            slot.layout =
                                f.build_rich_text(b.spans, body_style(w, eo));
                            if (slot.layout)
                            {
                                slot.plain = spans_to_plain(b.spans);
                                slot.spans = std::move(b.spans);
                            }
                        }
                    }
                }
                // Plain text: autolink bare URLs so they render as rich text.
                if (!slot.layout && slot.sections.empty() && !eo &&
                    !m.body.empty())
                {
                    auto link_spans = autolink_plain_to_spans(m.body);
                    if (!link_spans.empty())
                    {
                        apply_emoji_segmentation(link_spans);
                        slot.layout =
                            f.build_rich_text(link_spans, body_style(w, eo));
                        if (slot.layout)
                        {
                            slot.plain = m.body;
                            slot.spans = std::move(link_spans);
                        }
                    }
                }
                // Plain text with no URLs: try emoji-segmented rich text so
                // inline emoji render at InlineEmoji size, then fall back to
                // the plain flat layout (e.g. for emoji-only messages already
                // handled by BigEmoji, or when segmentation finds no emoji).
                if (!slot.layout && slot.sections.empty())
                {
                    std::string plain_text =
                        m.body.empty() ? std::string("(empty message)")
                                       : m.body;
                    if (!eo && !plain_text.empty())
                    {
                        tk::TextSpan whole;
                        whole.text    = plain_text;
                        auto segs     = segment_emoji_runs(whole);
                        bool has_emoji = false;
                        for (const auto& s : segs)
                            if (s.is_emoji_run) { has_emoji = true; break; }
                        if (has_emoji)
                        {
                            slot.layout =
                                f.build_rich_text(segs, body_style(w, eo));
                            if (slot.layout)
                            {
                                slot.plain = plain_text;
                                slot.spans = std::move(segs);
                            }
                        }
                    }
                    if (!slot.layout)
                    {
                        slot.layout = f.build_text(plain_text, body_style(w, eo));
                        slot.plain  = std::move(plain_text);
                        slot.spans.clear();
                    }
                }
            });
    }

    // Measure height for a text message body — uses the shared body layout
    // cache so the row is shaped at most once per content/width/theme state.
    float measure_body_text(const MessageRowData& m, tk::LayoutCtx& ctx,
                            float w) const
    {
        const bool dark = ctx.theme.mode == tk::ThemeMode::Dark;
        const bool revealed = owner_.spoilers_.is_revealed(m.event_id);
        LinkLayout& e = body_layout_for(m, ctx.factory, w, dark, revealed);
        if (!e.sections.empty())
        {
            const SectionLayout& last = e.sections.back();
            return last.y_offset + last.height;
        }
        return e.layout ? e.layout->measure().h : 0.0f;
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

    // Paint span backgrounds (mention pills, code blocks, inline code) for one
    // rich-text section at world-space origin (ox, oy).
    // Draws mention-pill / code-block / inline-code backgrounds. Must run
    // BEFORE the layout's own text draw — these are meant to sit behind the
    // glyphs. Image spans are handled separately by paint_span_images()
    // (see its comment for why they can't share this pass).
    void paint_span_backgrounds(const std::vector<tk::TextSpan>& spans,
                                tk::TextLayout& layout,
                                tk::PaintCtx& ctx,
                                float ox, float oy) const
    {
        int boff = 0;
        for (std::size_t si = 0; si < spans.size();)
        {
            const auto& sp  = spans[si];
            int          len = static_cast<int>(sp.text.size());
            if (sp.is_image && len > 0)
            {
                boff += len;
                ++si;
            }
            else if (sp.is_mention && sp.has_background && len > 0)
            {
                for (const tk::Rect& r :
                     layout.selection_rects(boff, boff + len))
                {
                    tk::Rect pill{r.x + ox - 2.0f, r.y + oy, r.w + 4.0f, r.h};
                    ctx.canvas.fill_rounded_rect(
                        pill, std::min(7.0f, pill.h * 0.5f), sp.background);
                }
                boff += len;
                ++si;
            }
            else if (sp.code_block)
            {
                int run_start = boff;
                while (si < spans.size() && spans[si].code_block)
                {
                    boff += static_cast<int>(spans[si].text.size());
                    ++si;
                }
                bool     any = false;
                tk::Rect u{};
                for (const tk::Rect& r :
                     layout.selection_rects(run_start, boff))
                {
                    if (!any)
                    {
                        u = r;
                        any = true;
                        continue;
                    }
                    float x0 = std::min(u.x, r.x);
                    float y0 = std::min(u.y, r.y);
                    float x1 = std::max(u.x + u.w, r.x + r.w);
                    float y1 = std::max(u.y + u.h, r.y + r.h);
                    u        = {x0, y0, x1 - x0, y1 - y0};
                }
                if (any)
                {
                    tk::Rect panel{u.x + ox - 6.0f, u.y + oy - 4.0f,
                                   u.w + 12.0f, u.h + 8.0f};
                    ctx.canvas.fill_rounded_rect(
                        panel, 6.0f, ctx.theme.palette.code_bg);
                }
            }
            else if (sp.code && len > 0)
            {
                for (const tk::Rect& r :
                     layout.selection_rects(boff, boff + len))
                {
                    ctx.canvas.fill_rect(
                        {r.x + ox - 2.0f, r.y + oy - 1.0f, r.w + 4.0f,
                         r.h + 2.0f},
                        ctx.theme.palette.code_bg);
                }
                boff += len;
                ++si;
            }
            else
            {
                boff += len;
                ++si;
            }
        }
    }

    // Draws resolved bitmaps for is_image spans (MSC2545 custom emoticons).
    // Runs after the layout's own text draw, purely by convention matching
    // paint_span_backgrounds' call sites — the text pass itself never draws
    // anything at this span's position in the first place (see
    // substitute_image_placeholders' comment: each backend's native inline-
    // object mechanism reserves the box without considering any fallback
    // glyph), so there's nothing to paint over here.
    void paint_span_images(const std::vector<tk::TextSpan>& spans,
                           tk::TextLayout& layout, tk::PaintCtx& ctx,
                           float ox, float oy) const
    {
        int boff = 0;
        for (const auto& sp : spans)
        {
            int len = static_cast<int>(sp.text.size());
            if (sp.is_image && len > 0)
            {
                const tk::Image* img = owner_.image_provider_
                    ? owner_.image_provider_(sp.image_mxc) : nullptr;
                if (img)
                {
                    for (const tk::Rect& r :
                         layout.selection_rects(boff, boff + len))
                    {
                        ctx.canvas.draw_image(
                            *img, {r.x + ox, r.y + oy, r.w, r.h});
                    }
                }
            }
            boff += len;
        }
    }

    // Paint a text message body — uses rich text when formatted_body is
    // present, otherwise falls back to plain text.
    float paint_body_text(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                          float y, float w, tk::Color color) const
    {
        const bool dark     = ctx.theme.mode == tk::ThemeMode::Dark;
        const bool revealed = owner_.spoilers_.is_revealed(m.event_id);
        LinkLayout& e = body_layout_for(m, ctx.factory, w, dark, revealed);

        auto ord     = owner_.selection_ordered();
        int  msg_idx = owner_.message_index_of(m.event_id);
        auto draw_with_selection = [&](tk::TextLayout& lay, float ox, float oy,
                                       int plain_len)
        {
            if (ord && msg_idx >= 0 &&
                msg_idx >= ord->lo_idx && msg_idx <= ord->hi_idx)
            {
                int lo_b = (msg_idx == ord->lo_idx) ? ord->lo_byte : 0;
                int hi_b =
                    (msg_idx == ord->hi_idx) ? ord->hi_byte : plain_len;
                if (lo_b != hi_b)
                    for (const tk::Rect& r : lay.selection_rects(lo_b, hi_b))
                        ctx.canvas.fill_rect({r.x + ox, r.y + oy, r.w, r.h},
                                             ctx.theme.palette.selection);
            }
            ctx.canvas.draw_text(lay, {ox, oy}, color);
        };

        // ── Multi-section path (block structure: headings, lists, etc.) ───────
        if (!e.sections.empty())
        {
            float total_h = 0.0f;
            for (SectionLayout& sec : e.sections)
            {
                if (!sec.layout)
                    continue;
                const float ox = x + sec.x_offset;
                const float oy = y + sec.y_offset;
                sec.origin     = {ox, oy};
                const float sh = sec.layout->measure().h;

                // Span backgrounds.
                paint_span_backgrounds(sec.spans, *sec.layout, ctx, ox, oy);

                // Block decoration (behind text).
                switch (sec.kind)
                {
                case BodyBlock::Kind::Blockquote:
                {
                    tk::Rect bar{ox - kBlockquoteBarGap - kBlockquoteBarW,
                                 oy, kBlockquoteBarW, sh};
                    ctx.canvas.fill_rounded_rect(
                        bar, kBlockquoteBarW * 0.5f,
                        ctx.theme.palette.separator);
                    break;
                }
                case BodyBlock::Kind::UnorderedItem:
                {
                    tk::TextSpan bullet;
                    bullet.text = "\xe2\x80\xa2"; // U+2022 BULLET
                    std::vector<tk::TextSpan> bspans{bullet};
                    tk::TextStyle st;
                    st.role      = tk::FontRole::Body;
                    st.wrap      = false;
                    st.max_width = kListIndent;
                    auto blay    = ctx.factory.build_rich_text(bspans, st);
                    if (blay)
                        ctx.canvas.draw_text(
                            *blay,
                            {ox - kBulletGap - blay->measure().w, oy},
                            color);
                    break;
                }
                case BodyBlock::Kind::OrderedItem:
                {
                    tk::TextSpan num_span;
                    num_span.text = std::to_string(sec.index) + ".";
                    std::vector<tk::TextSpan> nspans{num_span};
                    tk::TextStyle st;
                    st.role      = tk::FontRole::Body;
                    st.wrap      = false;
                    st.max_width = kListIndent;
                    auto nlay    = ctx.factory.build_rich_text(nspans, st);
                    if (nlay)
                        ctx.canvas.draw_text(
                            *nlay,
                            {ox - kBulletGap - nlay->measure().w, oy},
                            color);
                    break;
                }
                default:
                    break;
                }

                draw_with_selection(*sec.layout, ox, oy,
                                    static_cast<int>(spans_to_plain(sec.spans).size()));
                ctx.canvas.draw_text(*sec.layout, {ox, oy}, color);
                paint_span_images(sec.spans, *sec.layout, ctx, ox, oy);

                // Post-text decoration: horizontal rule below h1/h2.
                if (sec.kind == BodyBlock::Kind::Heading && sec.level <= 2)
                {
                    float rule_y = oy + sh + kHeadingRuleGap * 0.5f;
                    ctx.canvas.fill_rect(
                        {x, rule_y, w, 1.0f},
                        ctx.theme.palette.separator);
                }

                total_h = sec.y_offset + sec.height;
            }
            return total_h;
        }

        // ── Flat path (plain/autolink/single-paragraph) ───────────────────────
        if (!e.layout)
            return 0.0f;
        tk::TextLayout& layout = *e.layout;
        e.origin               = {x, y};

        if (!e.spans.empty())
            paint_span_backgrounds(e.spans, layout, ctx, x, y);

        draw_with_selection(layout, x, y, static_cast<int>(e.plain.size()));

        if (!e.spans.empty())
            paint_span_images(e.spans, layout, ctx, x, y);
        return layout.measure().h;
    }

    // MSC4278: draw a suppressed-media tile (blurhash if decoded, else a
    // neutral box) with a centred "Load …" pill. The whole tile is the
    // click target (handled via the existing image/video hit geometry, which
    // fires on_reveal_media when the row is hidden).
    void paint_hidden_media_placeholder(const MessageRowData& m,
                                        tk::PaintCtx& ctx, tk::Rect dst) const
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
            ctx.canvas.fill_rounded_rect(dst, 8.0f, ctx.theme.palette.chrome_bg);
            ctx.canvas.stroke_rounded_rect(dst, 8.0f, ctx.theme.palette.border,
                                           1.0f);
        }
        // Dim scrim so the pill reads over any blurhash.
        ctx.canvas.fill_rounded_rect(dst, 8.0f, tk::Color{0, 0, 0, 90});

        const char* label = (m.kind == MessageRowData::Kind::Video)
                                 ? "Load video"
                                 : (m.kind == MessageRowData::Kind::Sticker)
                                       ? "Load sticker"
                                       : "Load image";
        tk::TextStyle ts{};
        ts.role = tk::FontRole::UiSemibold;
        auto lo = ctx.factory.build_text(label, ts);
        if (!lo)
        {
            return;
        }
        tk::Size lsz = lo->measure();
        constexpr float kMsgListPadX = 12.0f, kMsgListPadY = 7.0f;
        float pw = lsz.w + kMsgListPadX * 2.0f;
        float ph = lsz.h + kMsgListPadY * 2.0f;
        // Keep the pill inside the tile.
        pw = std::min(pw, dst.w);
        tk::Rect pill{dst.x + (dst.w - pw) * 0.5f, dst.y + (dst.h - ph) * 0.5f,
                      pw, ph};
        ctx.canvas.fill_rounded_rect(pill, ph * 0.5f, ctx.theme.palette.accent);
        ctx.canvas.draw_text(*lo,
                             {pill.x + (pw - lsz.w) * 0.5f,
                              pill.y + (ph - lsz.h) * 0.5f},
                             ctx.theme.palette.text_on_accent);
    }

    void paint_inline_media(const MessageRowData& m, tk::PaintCtx& ctx,
                            tk::Rect dst) const
    {
        if (owner_.media_is_hidden_(m))
        {
            paint_hidden_media_placeholder(m, ctx, dst);
            return;
        }
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
            if (ctx.anim_damage)
            {
                ctx.anim_damage->note_image(display_key, dst);
            }
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
    // The pan/zoom/tooltip + tile-fetch logic lives in LocationMapPanner;
    // this just delegates (the panner records the hit-test rect).
    void paint_location_map(const MessageRowData& m, tk::PaintCtx& ctx,
                            tk::Rect map_rect) const
    {
        owner_.map_panner_.paint(m, ctx, map_rect);
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
        // Card draw + geometry capture live in the playback controller; the
        // icon cache stays Adapter-local (one rasterised glyph per backend).
        owner_.media_.paint_voice_card(m, ctx, dst, ic_play_voice_);
    }

    void paint_audio_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const
    {
        owner_.media_.paint_audio_card(m, ctx, dst, ic_play_audio_);
    }

    void paint_video_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const
    {
        if (owner_.media_is_hidden_(m))
        {
            paint_hidden_media_placeholder(m, ctx, dst);
            return;
        }
        // Live inline player frame takes priority over the static thumbnail.
        const tk::Image* live_frame =
            owner_.video_playlist_.live_frame(m.event_id);

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

            // Play glyph (▶): Lucide play icon, near-white over the dark disc.
            ic_play_video_.draw(ctx.canvas, ctx.factory, kPlaySvg, disc,
                                kDiscD * 0.5f, tk::Color{255, 255, 255, 230});
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

    // Glyphs that never change across rows — built once on first paint. The
    // hover-bar action icons moved to SVG (see ic_* CachedIcon members); only
    // the reaction "+" chip remains a text glyph here.
    struct StaticLayouts
    {
        std::unique_ptr<tk::TextLayout> plus;
        // Height of a single line of body text — used to vertically centre the
        // gutter timestamp against the message line on continuation rows.
        float body_line_h = 0.0f;

        void ensure(tk::CanvasFactory& f)
        {
            if (plus)
            {
                return;
            }
            tk::TextStyle bst{};
            bst.role = tk::FontRole::Body;
            if (auto bl = f.build_text("Ag", bst))
            {
                body_line_h = bl->measure().h;
            }
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            plus       = f.build_text("+", st);
        }
        void clear()
        {
            plus.reset();
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

    // Hover-bar action icons (tinted text_secondary) + play glyphs for the
    // voice / audio / inline-video cards (tinted per their disc colour). Each
    // tk::IconCache is tint-aware, so they recolor on theme switch and stay
    // crisp across DPI.
    mutable tk::IconCache ic_react_, ic_reply_, ic_thread_, ic_edit_, ic_more_;
    mutable tk::IconCache ic_play_voice_, ic_play_audio_, ic_play_video_;
    mutable tk::IconCache ic_call_phone_, ic_call_video_;

    MessageListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
// Scroll-to-bottom pill — floats over the bottom-right corner of the
// message list. Lives in the widget child tree so dispatch_pointer_move
// naturally routes events to it, preventing hover from leaking to rows
// painted beneath the pill.
// ─────────────────────────────────────────────────────────────────────────

struct ScrollPillWidget : tk::Widget
{
    bool pressed_ = false;
    std::function<void()> on_clicked;

    void set_bounds(tk::Rect b) { bounds_ = b; }

    bool on_pointer_move(tk::Point) override { return true; }
    bool on_pointer_down(tk::Point) override
    {
        pressed_ = true;
        return true;
    }
    void on_pointer_up(tk::Point, bool inside) override
    {
        bool was = std::exchange(pressed_, false);
        if (was && inside && on_clicked)
            on_clicked();
    }
    void on_pointer_leave() override { pressed_ = false; }
    tk::Size measure(tk::LayoutCtx&, tk::Size s) override { return s; }
    void arrange(tk::LayoutCtx&, tk::Rect b) override { bounds_ = b; }

    void paint(tk::PaintCtx& ctx) override
    {
        constexpr float kSz = 36.0f;
        auto bg = pressed_ ? ctx.theme.palette.subtle_pressed
                           : ctx.theme.palette.chrome_bg;
        ctx.canvas.fill_rounded_rect(bounds_, kSz * 0.5f, bg);
        ctx.canvas.stroke_rounded_rect(bounds_, kSz * 0.5f,
                                       ctx.theme.palette.border, 1.0f);
        tk::TextStyle gs{};
        gs.role = tk::FontRole::UiSemibold;
        gs.wrap = false;
        auto glyph = ctx.factory.build_text("\xE2\x86\x93", gs); // U+2193 ↓
        if (glyph)
        {
            tk::Size sz = glyph->measure();
            ctx.canvas.draw_text(*glyph,
                                 {bounds_.x + (kSz - sz.w) * 0.5f,
                                  bounds_.y + (kSz - sz.h) * 0.5f},
                                 ctx.theme.palette.text_primary);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────

MessageListView::~MessageListView()
{
    // Invalidate the liveness sentinel BEFORE any member is torn down. Deferred
    // UI-thread callbacks (post_delayed_ timers, async media-fetch results)
    // capture a weak_ptr to `alive_` and check `*alive_` before touching
    // `this`. (The inline video players own their own sentinel inside
    // video_playlist_.) Clearing the flag here — rather than
    // relying on `alive_`'s own destruction at the end of member teardown —
    // closes the window in which a callback could observe a half-destroyed
    // object while earlier-declared members are still being destroyed.
    *alive_ = false;
}

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

    // Wire the location-map panner to MessageListView's (later-assigned)
    // callbacks through indirection so RoomView's assignment of the public
    // tile/tooltip callbacks and set_image_provider() are picked up.
    map_panner_.set_tile_image_provider(
        [this](const std::string& key) -> const tk::Image*
        { return image_provider_ ? image_provider_(key) : nullptr; });
    map_panner_.set_tile_request(
        [this](int z, int x, int y)
        {
            if (on_tile_needed)
                on_tile_needed(z, x, y);
        });
    map_panner_.set_tooltip_callbacks(
        [this](std::string text, tk::Rect anchor)
        {
            if (on_show_tooltip)
                on_show_tooltip(std::move(text), anchor);
        },
        [this]
        {
            if (on_hide_tooltip)
                on_hide_tooltip();
        });

    // The preview card draws its thumbnail via the same image provider as the
    // rest of the timeline. Wire it through indirection so RoomView's later
    // set_image_provider() is reflected (matching the historical live read of
    // image_provider_ inside the card paint).
    previews_.set_image_provider([this](const std::string& k) -> const tk::Image*
                                 { return image_provider_ ? image_provider_(k)
                                                          : nullptr; });

    // Wire the room-switch gate keeper through indirection so RoomView's
    // later assignment of the providers / repaint / post_delayed callbacks is
    // always reflected. The keeper owns the gate state machine; this view
    // drives evaluate()/try_reveal() from paint().
    room_switch_gate_.set_providers(
        [this](const std::string& k) -> const tk::Image*
        { return image_provider_ ? image_provider_(k) : nullptr; },
        [this](const std::string& url) -> const UrlPreviewData*
        { return previews_.lookup(url); });
    room_switch_gate_.set_scroll_callbacks(
        [this](const std::string& event_id) { scroll_to_event_id(event_id); },
        [this] { scroll_to_bottom(); });
    room_switch_gate_.set_post_delayed(
        [this](int ms, std::function<void()> cb)
        {
            if (post_delayed_)
                post_delayed_(ms, std::move(cb));
        });
    room_switch_gate_.set_repaint(
        [this]
        {
            if (request_repaint_)
                request_repaint_();
        });
    room_switch_gate_.set_alive(alive_);

    auto pw = std::make_unique<ScrollPillWidget>();
    scroll_pill_ = pw.get();
    scroll_pill_->set_visible(false);
    scroll_pill_->on_clicked = [this]
    {
        if (historical_mode_ && on_return_to_live)
            on_return_to_live();
        else
            scroll_to_bottom();
    };
    add_child(std::move(pw));
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
    // The new room's content has arrived — leave the room-switch loading state
    // (clean background + delayed spinner) and bump the epoch so any pending
    // delayed-spinner timer from the switch is neutralised. The gate armed below
    // takes over from here for height-stability holding.
    if (switch_loading_)
    {
        switch_loading_     = false;
        switch_spinner_due_ = false;
        ++switch_epoch_;
    }
    // Defence-in-depth: any timeline reset cancels a pending nav-load overlay.
    if (nav_loading_)
    {
        nav_loading_     = false;
        nav_spinner_due_ = false;
        ++nav_epoch_;
    }
    // Defence-in-depth: a room switch invalidates any carried-over pagination
    // spinner state from the previous room (the new room's shell path manages
    // its own paginating flag via set_paginating()).
    if (room_switch && paginating_)
        paginating_ = false;

    // Defence-in-depth: drop any in-thread replies. The main timeline
    // shows only top-level events + thread roots; in-thread replies are
    // rendered in the ThreadPanel. ShellBase already routes inserts away
    // from this view, but we strip again here in case a caller bypasses
    // that path (tests, future code).
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(),
                              [](const MessageRowData& m)
                              { return !m.thread_root_id.empty(); }),
               msgs.end());

    video_playlist_.clear();
    spoilers_.clear();
    membership_groups_.clear();
    // NB: do NOT clear link_cache_ here. It is the content-addressed body-text
    // layout cache (keyed by {width, theme, revealed, hash_body}), so retaining
    // it across a room switch lets a return to a previously-viewed room reuse
    // the already-shaped bodies instead of re-shaping every visible line; its
    // 128-entry LRU bounds memory. The adapter's layout_cache_ below is
    // index-aligned to messages_, so it must still be cleared on a full replace.
    adapter_->clear_layout_cache();
    suppress_read_marker_ = false;
    sel_.reset();
    sel_is_dragging_ = false;
    press_sel_ = false;
    // Whole-room pinning (the try_acquire_image_ loop): hold an ImageRef for
    // every row that displays a cached image so it survives eviction while this
    // room is open. Rows whose image is not yet decoded acquire null here and
    // re-pin on notify_*_ready.
    pending_scroll_event_id_.clear();
    if (room_switch)
    {
        // Discard any voice/audio play armed in the previous room so a clip
        // whose fetch is still in flight can't auto-start here.
        media_.reset_pending_play();
        // Do NOT clear pinned_event_ids_/can_pin_ here: ShellBase already
        // pushed the new room's correct values via set_pinned_event_ids() /
        // set_can_pin() (refresh_pinned_for_current_room_(), called
        // synchronously on room switch, before this async timeline-reset
        // callback can land). Clearing them here would clobber that correct
        // state back to "no pin permission, nothing pinned" until the next
        // unrelated room-list refresh happens to run.
        messages_ = std::move(msgs);
        for (auto& m : messages_)
        {
            try_acquire_image_(m);
        }
        invalidate_data();
        scroll_to_bottom();
    }
    else
    {
        // Backfill: capture the scroll anchor against the *current* model
        // before swapping, so the row the user is looking at is located by key
        // before its index shifts under the prepended history. The swap lives
        // inside the lambda; arrange() restores the anchor once the new heights
        // are measured.
        preserve_top_through(
            [&]
            {
                messages_ = std::move(msgs);
                for (auto& m : messages_)
                {
                    try_acquire_image_(m);
                }
                invalidate_data();
            });
    }
    // Start inline players for animated video rows near the bottom (most
    // recently received), up to the cap.
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    {
        if (video_playlist_.size() >=
            TimelineVideoPlaylist::kMaxInlinePlayers)
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
    // keeper bumps its epoch (neutralising any outstanding timeout closure)
    // and clears for the non-gated case.
    if (!room_switch || messages_.empty())
    {
        room_switch_gate_.clear();
        return; // nothing to gate
    }
    // Arms a fresh gate + its 400ms timeout fallback. Dependencies are
    // collected on the first paint (the visible band needs a measure pass).
    room_switch_gate_.begin_room_switch();
}

void MessageListView::begin_focused_gate(const std::string& focus_event_id)
{
    room_switch_gate_.set_focus_event(focus_event_id);
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

void MessageListView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    post_delayed_ = std::move(f);
}

void MessageListView::insert_message(std::size_t index, MessageRowData msg)
{
    // Defence-in-depth: drop in-thread replies (handled by ThreadPanel).
    if (!msg.thread_root_id.empty())
    {
        return;
    }
    if (index > messages_.size())
    {
        index = messages_.size();
    }

    const bool animated = msg.kind == MessageRowData::Kind::Video &&
                          (msg.video_autoplay || msg.video_gif);

    // Suppress the read marker while the SDK catches up to the new position.
    // update_message() clears this flag when it delivers the updated marker.
    bool suppress_flipped = false;
    if (!is_virtual_event(msg.kind))
    {
        suppress_flipped = !suppress_read_marker_;
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
        try_acquire_image_(messages_.back());
        insert_row(messages_.size() - 1); // targeted: only the new tail row
        if (suppress_flipped)
        {
            // Flipping suppress_read_marker_ changes every row's is_cont
            // skip-over and any existing read marker's height — a global
            // effect the targeted insert above cannot cover.
            invalidate_data();
        }
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
            try_acquire_image_(messages_[index]);
            insert_row(index); // targeted: re-measure only the inserted gap
            if (suppress_flipped)
            {
                invalidate_data(); // global suppress_read_marker_ effect
            }
        });
}

void MessageListView::update_message(std::size_t index, MessageRowData msg)
{
    // Defence-in-depth: in-thread replies don't live in this view.
    if (!msg.thread_root_id.empty())
    {
        return;
    }
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
    // A read marker moving (in or out of this row) flips suppress_read_marker_
    // and the is_cont skip-over logic for *other* rows, so it needs a full
    // rebuild rather than a targeted one.
    const bool touches_read_marker =
        msg.kind == MessageRowData::Kind::ReadMarker ||
        messages_[index].kind == MessageRowData::Kind::ReadMarker;
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
        video_playlist_.drop(old_eid);
    }
    // On a local-echo -> remote event_id swap, drop the body-layout cache entry
    // keyed by the old id so it doesn't occupy an LRU slot (the new layout will
    // be cached under msg.event_id).
    if (old_eid != msg.event_id)
    {
        link_cache_.invalidate(old_eid);
    }
    if (now_animated && !video_playlist_.has(msg.event_id))
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
                              auto live = walive.lock();
                              if (!live || !*live)
                              {
                                  return;
                              }
                              clear_just_sent(eid);
                          });
        }
    }

    // Preserve the pinned image across the update when the displayed key is
    // unchanged (reaction / receipt / edit on an image row), avoiding a
    // release-then-reacquire gap. Otherwise the row re-pins below.
    const std::string old_key = messages_[index].owned_image_key;
    if (messages_[index].owned_image && !old_key.empty() &&
        row_image_key_(msg) == old_key)
    {
        msg.owned_image = std::move(messages_[index].owned_image);
        msg.owned_image_key = old_key;
    }
    // Transfer the avatar pin when the sender URL is unchanged (e.g. a
    // read-receipt or reaction update on an existing row).
    if (messages_[index].owned_avatar &&
        messages_[index].owned_avatar_key == msg.sender_avatar_url)
    {
        msg.owned_avatar     = std::move(messages_[index].owned_avatar);
        msg.owned_avatar_key = messages_[index].owned_avatar_key;
    }
    messages_[index] = std::move(msg);
    try_acquire_image_(messages_[index]);
    if (touches_read_marker)
    {
        invalidate_data(); // global effect — full rebuild
    }
    else
    {
        invalidate_row(index); // targeted: this row + its dependency span
    }
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
    video_playlist_.drop(messages_[index].event_id);
    adapter_->erase_layout_cache_at(index);
    preserve_top_through(
        [&]
        {
            messages_.erase(messages_.begin() + index);
            erase_row(index); // targeted: re-measure only around the gap
        });
}

void MessageListView::append_message(MessageRowData msg)
{
    insert_message(messages_.size(), std::move(msg));
}

void MessageListView::set_dimmed(bool dimmed)
{
    if (dimmed_ == dimmed)
    {
        return;
    }
    dimmed_ = dimmed;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::set_highlighted_event(const std::string& event_id)
{
    if (highlighted_event_id_ == event_id)
    {
        return;
    }
    highlighted_event_id_ = event_id;
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::set_search_matches(std::unordered_set<std::string> ids)
{
    if (ids == search_match_ids_)
        return;
    search_match_ids_ = std::move(ids);
    if (request_repaint_)
        request_repaint_();
}

void MessageListView::clear_search_matches()
{
    if (search_match_ids_.empty())
        return;
    search_match_ids_.clear();
    if (request_repaint_)
        request_repaint_();
}

void MessageListView::set_pinned_event_ids(std::unordered_set<std::string> ids)
{
    if (pinned_event_ids_ == ids)
    {
        return;
    }
    pinned_event_ids_ = std::move(ids);
    if (request_repaint_)
    {
        request_repaint_();
    }
}

void MessageListView::set_can_pin(bool can_pin)
{
    if (can_pin_ == can_pin)
    {
        return;
    }
    can_pin_ = can_pin;
    if (request_repaint_)
    {
        request_repaint_();
    }
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

void MessageListView::set_shortcode_provider(ShortcodeProvider p)
{
    shortcode_provider_ = std::move(p);
}

void MessageListView::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

bool MessageListView::media_is_hidden_(const MessageRowData& m) const
{
    if (!media_hidden_ || m.event_id.empty())
    {
        return false;
    }
    switch (m.kind)
    {
    case MessageRowData::Kind::Image:
    case MessageRowData::Kind::Sticker:
    case MessageRowData::Kind::Video:
        return media_hidden_(m.event_id, m.is_own);
    default:
        return false;
    }
}

bool MessageListView::media_is_hidden_by_eid_(const std::string& event_id) const
{
    if (!media_hidden_ || event_id.empty())
    {
        return false;
    }
    for (const auto& m : messages_)
    {
        if (m.event_id == event_id)
        {
            return media_is_hidden_(m);
        }
    }
    return false;
}

void MessageListView::set_image_acquirer(ImageAcquirer a)
{
    image_acquirer_ = std::move(a);
}

std::string MessageListView::row_image_key_(const MessageRowData& m) const
{
    using Kind = MessageRowData::Kind;
    switch (m.kind)
    {
    case Kind::Image:
    case Kind::Sticker:
    case Kind::Video:
    {
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        return look ? look->fetch_token() : std::string{};
    }
    default:
        break;
    }
    // URL-preview image (text / notice rows carry it via the preview cache).
    if (!m.first_url.empty())
    {
        if (const auto* p = previews_.lookup(m.first_url))
        {
            return p->image_mxc;
        }
    }
    return std::string{};
}

void MessageListView::try_acquire_image_(MessageRowData& m)
{
    if (!image_acquirer_)
    {
        return;
    }
    const std::string key = row_image_key_(m);
    if (key.empty())
    {
        // Nothing pinnable for inline media (text/audio/file/etc. rows).
        m.owned_image.reset();
        m.owned_image_key.clear();
        // fall through to avatar pin below
    }
    else if (!m.owned_image || m.owned_image_key != key)
    {
        m.owned_image = image_acquirer_(key); // null when not yet decoded — fine
        m.owned_image_key = key;
    }
    // Pin sender avatar so it survives thumbnail cache sweeps during idle
    // periods. image_acquirer_ probes thumbnail_cache() as its fallback, so
    // it correctly acquires avatar entries. Returns null when not yet decoded;
    // re-pinned by notify_image_ready() once the fetch completes.
    if (!m.sender_avatar_url.empty() &&
        (!m.owned_avatar || m.owned_avatar_key != m.sender_avatar_url))
    {
        m.owned_avatar     = image_acquirer_(m.sender_avatar_url);
        m.owned_avatar_key = m.sender_avatar_url;
    }
}

void MessageListView::set_preview_provider(PreviewProvider p)
{
    previews_.set_provider(std::move(p));
}

void MessageListView::reset_hovered_row_geom_()
{
    hovered_row_geom_ = RowChipGeom{};
}

void MessageListView::clear_scroll_hit_geometry_()
{
    clear_hit_geometry_();
    reset_hovered_row_geom_();
    hover_target_   = HoverTarget::None;
    hover_chip_idx_ = -1;
}

void MessageListView::on_anchored_relayout_()
{
    if (gate_blocks_input_())
    {
        return;
    }
    // Only re-resolve hover when the pointer is already over a row. If nothing
    // was hovered, last_pointer_local_ is stale and re-running the hit-test
    // would establish a phantom highlight. (A hovered row is also exactly the
    // case the anchor preserved, so this is where re-resolution matters.)
    if (hovered_row_index() < 0)
    {
        return;
    }
    // The anchored row kept its screen position, but its index may have
    // shifted (prepend) and the hovered-row geometry is stale. Re-resolve the
    // hovered row from the last pointer position; the post-paint chip-hit
    // re-evaluation then restores the chip target once geometry is repainted.
    refresh_hover_at(last_pointer_local_);
    reset_hovered_row_geom_();
    hover_target_   = HoverTarget::None;
    hover_chip_idx_ = -1;
}

void MessageListView::notify_url_preview_ready(const std::string& url)
{
    room_switch_gate_.notify_loaded(url);
    for (std::size_t i = 0; i < messages_.size(); ++i)
    {
        if (messages_[i].first_url == url)
        {
            // Preview data (hence image_mxc) is now known; pin the image if it
            // is already cached. Otherwise notify_image_ready re-pins on decode.
            try_acquire_image_(messages_[i]);
            // Anchor unconditionally: the card grows the row downward, so a
            // card loading anywhere above the anchored row would otherwise
            // push the viewport. preserve_top_through still no-ops on
            // stick-to-bottom / at-top, preserving live-tail and top-reveal.
            preserve_top_through([&] { invalidate_data(); });
            return;
        }
    }
    invalidate_data();
}

void MessageListView::notify_image_ready(const std::string& url)
{
    room_switch_gate_.notify_loaded(url);
    bool matched = false;
    for (std::size_t i = 0; i < messages_.size(); ++i)
    {
        // Match either the full-res token or the thumbnail token.
        // ShellBase pre-fetches whichever is present (thumbnail wins), so an
        // Image/Sticker row whose only cached representation is the thumbnail
        // would otherwise never remeasure when bytes arrive.
        auto& m = messages_[i];
        const bool src_match = m.source && m.source->fetch_token() == url;
        const bool thumb_match = m.thumbnail && m.thumbnail->fetch_token() == url;
        const bool fsrc_match = m.file_source && m.file_source->fetch_token() == url;
        // Preview-image rows carry their image under a key the media-source
        // match above does not see; check it only when nothing else matched.
        const bool preview_match =
            !src_match && !thumb_match && !fsrc_match && !m.first_url.empty() &&
            !url.empty() && row_image_key_(m) == url;
        // MSC2545 inline custom emoticons (<img data-mx-emoticon src=mxc>)
        // live inside the message's own HTML body, not as a tracked
        // attachment/preview field — none of the matches above can see them.
        // A substring check against the raw HTML is sufficient: html_spans.cpp
        // only ever treats a src as a renderable emoticon when it's an exact
        // mxc:// URL, so a match here can't be a false positive from
        // surrounding markup.
        const bool emoticon_match =
            !src_match && !thumb_match && !fsrc_match && !preview_match &&
            !url.empty() && m.formatted_body.find(url) != std::string::npos;
        if (src_match || thumb_match || fsrc_match || preview_match ||
            emoticon_match)
        {
            // Newly decoded → re-pin this row now that the image exists.
            try_acquire_image_(m);
            matched = true;
        }
        // Pin newly-decoded sender avatar. Avatars don't change row height so
        // this doesn't contribute to the relayout-triggering `matched` flag.
        if (!url.empty() && m.sender_avatar_url == url &&
            (!m.owned_avatar || m.owned_avatar_key != url))
        {
            m.owned_avatar     = image_acquirer_(url);
            m.owned_avatar_key = url;
        }
    }
    if (!matched)
    {
        return;
    }
    // Anchor unconditionally: a decoded image grows its row downward, so an
    // image loading anywhere above the anchored row would otherwise push the
    // viewport. preserve_top_through no-ops on stick-to-bottom / at-top.
    preserve_top_through([&] { invalidate_data(); });
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
            // Anchor unconditionally: the waveform fills in the voice card and
            // can change row height; keep the anchored row pixel-stable.
            preserve_top_through([this] { invalidate_data(); });
            return;
        }
    }
}

void MessageListView::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    // Ownership + the on_progress wiring move into the playback controller.
    media_.set_player(std::move(player));
    media_.set_next_voice_lookup(
        [this](const std::string& eid)
        { return find_next_voice_from_same_sender_(eid); });
}

const MessageRowData* MessageListView::find_next_voice_from_same_sender_(
    const std::string& finished_event_id) const
{
    auto it = std::find_if(messages_.begin(), messages_.end(),
                           [&](const MessageRowData& r)
                           { return r.event_id == finished_event_id; });
    if (it == messages_.end() || it->kind != MessageRowData::Kind::Voice)
    {
        return nullptr;
    }
    const std::string& sender = it->sender;
    for (auto next = std::next(it); next != messages_.end(); ++next)
    {
        if (next->kind == MessageRowData::Kind::Voice && next->sender == sender)
        {
            return &*next;
        }
    }
    return nullptr;
}

void MessageListView::set_voice_bytes_provider(VoiceBytesProvider provider)
{
    media_.set_bytes_provider(std::move(provider));
}

void MessageListView::set_repaint_requester(
    std::function<void()> request_repaint)
{
    request_repaint_ = request_repaint;
    // The inline video players drive their own repaints on each new frame.
    video_playlist_.set_repaint(request_repaint);
    // The playback controller drives its own repaints (play/pause/scrub/speed
    // and the on_progress tick), so hand it the same requester.
    media_.set_repaint(std::move(request_repaint));
}

void MessageListView::set_video_player_factory(VideoPlayerFactory f)
{
    video_playlist_.set_player_factory(std::move(f));
}

void MessageListView::set_video_fetch_provider(VideoFetchProvider f)
{
    video_playlist_.set_fetch_provider(std::move(f));
}

void MessageListView::start_inline_video(const MessageRowData& m)
{
    // MSC4278: don't auto-play a clip whose preview is suppressed. This guard
    // depends on view-private visibility state, so it stays here; the playlist
    // applies the remaining gates (active / already-playing / cap).
    if (media_is_hidden_(m))
    {
        return;
    }
    VideoSourceInfo info;
    info.event_id = m.event_id;
    info.source_token = m.source ? m.source->fetch_token() : std::string{};
    info.mime = m.video_mime;
    info.autoplay = m.video_autoplay;
    info.loop = m.video_loop;
    info.muted = m.video_no_audio;
    video_playlist_.ensure_playing(info);
}

void MessageListView::set_pending_scroll_event_id(const std::string& event_id)
{
    pending_scroll_event_id_ = event_id;
}

void MessageListView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    set_near_top_threshold_px(bounds.h);
    tk::ListView::arrange(ctx, bounds);
    // Apply any deferred scroll now that row_offsets_ are rebuilt and the
    // anchor-adjustment (preserve_top_through) has run. Only clear on success
    // so that if the event hasn't arrived yet (paginating in) we retry next pass.
    if (!pending_scroll_event_id_.empty())
    {
        if (scroll_to_event_id(pending_scroll_event_id_))
        {
            pending_scroll_event_id_.clear();
        }
    }
    // A voice/audio play click on a not-yet-cached clip arms a pending play.
    // The fetch's on_ready re-runs measure + arrange (this pass), by which
    // point the bytes are warm — start playback so a single click suffices.
    // But arrange() also runs on scroll / new messages, so only retry while the
    // armed clip is still on screen: if the user scrolled away before the bytes
    // warmed, abandon the pending play rather than auto-starting audio for a
    // clip they are no longer looking at. Auto-advance-armed plays skip this
    // check — the user never looked at that row to begin with, and the whole
    // point is to keep playing regardless of what's currently scrolled into view.
    if (media_.has_pending_play())
    {
        if (media_.pending_play_skip_visibility_gate() ||
            is_event_visible_(media_.pending_play_event_id()))
        {
            media_.retry_pending_voice_play();
        }
        else
        {
            media_.reset_pending_play();
        }
    }
}

bool MessageListView::is_event_visible_(const std::string& event_id) const
{
    if (event_id.empty())
    {
        return false;
    }
    auto [first, last] = visible_range();
    int actual_last = std::min(last, static_cast<int>(messages_.size()) - 1);
    for (int i = std::max(first, 0); i <= actual_last; ++i)
    {
        if (messages_[static_cast<std::size_t>(i)].event_id == event_id)
        {
            return true;
        }
    }
    return false;
}

bool MessageListView::on_wheel(tk::Point local, float dx, float dy)
{
    if (gate_blocks_input_())
    {
        return false; // list not painted yet
    }
    if (nav_loading_)
        return false;

    // Map zoom: intercept wheel over a Kind::Location map tile area.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        std::size_t ri = hovered_row_geom_.row_index;
        if (ri < messages_.size() &&
            messages_[ri].kind == MessageRowData::Kind::Location)
        {
            const tk::Rect* g =
                map_panner_.geom_at(messages_[ri].event_id);
            if (g && rect_contains(*g, world))
            {
                // Fire at most one zoom step per wheel event so a physical
                // mouse wheel (dy ≈ 90) doesn't jump many levels at once.
                if (map_panner_.zoom(dy, messages_[ri].map_viewport))
                {
                    invalidate_data();
                }
                return true;
            }
        }
    }

    const float before = scroll_y();
    const bool scrolled = tk::ListView::on_wheel(local, dx, dy);
    if (scrolled && scroll_y() != before)
    {
        clear_scroll_hit_geometry_();
        if (request_repaint_)
            request_repaint_();
    }
    return scrolled;
}

void MessageListView::on_pointer_drag(tk::Point local)
{
    if (gate_blocks_input_())
    {
        return;
    }
    if (nav_loading_)
        return;
    if (press_audio_kind_ == AudioPressKind::ProgressTrack &&
        !press_audio_event_id_.empty())
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& row : messages_)
        {
            if (row.event_id == press_audio_event_id_ &&
                row.kind == MessageRowData::Kind::Audio)
            {
                media_.handle_audio_scrub_at(row, world.x);
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
                const LinkLayout* le = link_cache_.peek(m.event_id);
                if (le)
                {
                    int idx = char_at_world(*le, world);
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
        const float before = scroll_y();
        tk::ListView::on_pointer_drag(local);
        if (scroll_y() != before)
        {
            clear_scroll_hit_geometry_();
            if (request_repaint_)
                request_repaint_();
        }
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
            media_.handle_voice_scrub_at(row, world.x);
            break;
        }
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
    if (nav_loading_)
        return false;
    last_pointer_local_ = local;
    tk::ListView::on_pointer_move(local);

    // Map pan: apply drag delta while a pan is active.
    if (map_panner_.panning() && map_panner_.active_row() < messages_.size())
    {
        map_panner_.drag_pan(local,
                             messages_[map_panner_.active_row()].map_viewport);
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
        reset_hovered_row_geom_();
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
            const LinkLayout* le = link_cache_.peek(m.event_id);
            if (le)
            {
                std::string lurl = link_at_world(*le, world);
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
            for (const auto& [eid, hit] : previews_.geometry())
            {
                if (!hit.url.empty() && rect_contains(hit.rect, world))
                {
                    new_link_url = hit.url;
                    break;
                }
            }
        }
        // Quote block hover: clicking jumps to the original message, so show
        // the pointing-hand cursor the same way file and preview cards do.
        if (new_link_url.empty())
        {
            for (const auto& [eid, rect] : quote_block_geom_)
            {
                if (rect_contains(rect, world))
                {
                    new_link_url = "quote://";
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
        std::string desc;
        int row = hovered_row_index();
        if (row >= 0 && static_cast<std::size_t>(row) < messages_.size())
        {
            const auto& m = messages_[static_cast<std::size_t>(row)];
            if (m.kind == MessageRowData::Kind::Location &&
                !m.location_description.empty())
            {
                if (const tk::Rect* g = map_panner_.geom_at(m.event_id))
                {
                    tk::Point world{local.x + bounds().x, local.y + bounds().y};
                    if (rect_contains(*g, world))
                    {
                        want_tooltip = true;
                        anchor = *g;
                        desc = m.location_description;
                    }
                }
            }
        }
        if (want_tooltip)
        {
            map_panner_.show_tooltip(desc, anchor);
        }
        else
        {
            map_panner_.hide_tooltip();
        }
    }

    // Action pill button tooltips — same change-on-transition pattern as ComposeBar.
    {
        const tk::Point world{local.x + bounds().x, local.y + bounds().y};
        ActionTooltip next      = ActionTooltip::None;
        tk::Rect      tip_anchor{};

        if (hovered_row_geom_.row_index != static_cast<std::size_t>(-1))
        {
            struct { const tk::Rect& rect; ActionTooltip btn; } checks[] = {
                {hovered_row_geom_.react_button,  ActionTooltip::React},
                {hovered_row_geom_.reply_button,  ActionTooltip::Reply},
                {hovered_row_geom_.thread_button, ActionTooltip::Thread},
                {hovered_row_geom_.edit_button,   ActionTooltip::Edit},
                {hovered_row_geom_.more_button,   ActionTooltip::More},
            };
            for (const auto& c : checks)
            {
                if (c.rect.w > 0.0f && rect_contains(c.rect, world))
                {
                    next       = c.btn;
                    tip_anchor = c.rect;
                    break;
                }
            }
        }

        if (next != action_tooltip_)
        {
            if (action_tooltip_ != ActionTooltip::None && on_hide_tooltip)
                on_hide_tooltip();
            action_tooltip_ = next;
            if (next != ActionTooltip::None && on_show_tooltip)
            {
                const char* src =
                    next == ActionTooltip::React  ? "Add reaction"
                    : next == ActionTooltip::Reply  ? "Reply"
                    : next == ActionTooltip::Thread ? "Reply in thread"
                    : next == ActionTooltip::Edit   ? "Edit"
                    :                                 "More";
                on_show_tooltip(tk::tr(src), tip_anchor);
            }
        }
    }
    return true;
}

tk::Widget* MessageListView::dispatch_pointer_move(tk::Point world, bool* dirty)
{
    tk::Widget* result = tk::Widget::dispatch_pointer_move(world, dirty);
    // If a child widget (the pill) absorbed the event, ListView::on_pointer_move
    // was skipped — hovered_index_ retains its old value. Clear it so no row
    // shows the action bar while the mouse is inside the pill.
    if (result && result != this && hovered_row_index() >= 0)
    {
        clear_hover_();
        reset_hovered_row_geom_();
        if (dirty)
            *dirty = true;
    }
    return result;
}

void MessageListView::on_pointer_leave()
{
    if (gate_blocks_input_())
        return;
    if (nav_loading_)
        return;
    if (hover_locked_)
        return;
    tk::ListView::on_pointer_leave();
    hovered_row_geom_.row_index = static_cast<std::size_t>(-1);
    hovered_row_geom_.chips.clear();
    hovered_row_geom_.receipt_discs.clear();
    hovered_row_geom_.add_visible = false;
    hovered_row_geom_.retry_button = tk::Rect{};
    hovered_row_geom_.abort_button = tk::Rect{};
    hover_target_ = HoverTarget::None;
    hover_chip_idx_ = -1;
    map_panner_.hide_tooltip();
    if (action_tooltip_ != ActionTooltip::None)
    {
        if (on_hide_tooltip)
            on_hide_tooltip();
        action_tooltip_ = ActionTooltip::None;
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
            clear_scroll_hit_geometry_();
            if (request_repaint_)
                request_repaint_();
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

void MessageListView::prepend_messages(std::vector<MessageRowData> rows)
{
    if (rows.empty())
        return;
    const std::size_t n = rows.size();
    for (std::size_t i = 0; i < n; ++i)
        adapter_->insert_layout_cache_at(0);
    preserve_top_through(
        [&]
        {
            messages_.insert(messages_.begin(), rows.begin(), rows.end());
            for (std::size_t i = 0; i < n; ++i)
                try_acquire_image_(messages_[i]);
            invalidate_data();
        });
}

void MessageListView::append_messages(std::vector<MessageRowData> rows)
{
    if (rows.empty())
        return;
    // Drop in-thread replies (defence-in-depth; callers should not send them).
    rows.erase(
        std::remove_if(rows.begin(), rows.end(),
                       [](const MessageRowData& r)
                       { return !r.thread_root_id.empty(); }),
        rows.end());
    if (rows.empty())
        return;

    // While browsing history (forward-pagination results, not a live tail),
    // don't follow the appended rows down — the "near bottom" trigger that
    // requested this page fires exactly when the user is at/near the bottom
    // of the loaded window, so at_bottom would almost always be true here
    // and yank the view to the new bottom instead of leaving it in place.
    const bool at_bottom = !historical_mode_ &&
                          scroll_y() + bounds().h + 1.0f >= content_height();
    // New real messages require the read marker to be repositioned.
    suppress_read_marker_ = true;
    for (auto& row : rows)
    {
        const bool animated = row.kind == MessageRowData::Kind::Video &&
                              (row.video_autoplay || row.video_gif);
        if (animated)
            start_inline_video(row);
        messages_.push_back(std::move(row));
        try_acquire_image_(messages_.back());
        insert_row(messages_.size() - 1);
    }
    if (at_bottom)
        scroll_to_bottom();
}

void MessageListView::set_paginating(bool paginating)
{
    if (paginating_ == paginating)
        return;
    paginating_ = paginating;
    if (paginating)
        paginate_start_ = std::chrono::steady_clock::now();
    if (request_repaint_)
        request_repaint_();
}

void MessageListView::draw_pagination_spinner_(tk::PaintCtx& ctx)
{
    // Top-of-viewport indicator while a back-paginate is in flight.
    draw_spinner_dots_(ctx, bounds_.x + bounds_.w * 0.5f, bounds_.y + 20.0f,
                       paginate_start_, /*radius=*/10.0f, /*dot_r=*/2.5f);
}

void MessageListView::clear_hit_geometry_()
{
    sticker_geom_.clear();
    image_geom_.clear();
    video_geom_.clear();
    file_geom_.clear();
    media_.clear_geometry();
    map_panner_.clear_geometry();
    quote_block_geom_.clear();
    previews_.clear_geometry();
    chip_hit_rects_.clear();
}

void MessageListView::begin_switch_loading()
{
    // Supersede any prior loading state / display gate, and bump the epoch so an
    // outstanding delayed-spinner timer from a previous switch is neutralised.
    ++switch_epoch_;
    switch_loading_       = true;
    switch_spinner_due_   = false;
    switch_spinner_start_ = std::chrono::steady_clock::now();
    room_switch_gate_.clear();

    // Clear the previous room's rows and hit-test geometry immediately.
    // The animation-tick guard added in c2157add skips the paint-time clear
    // when anim_damage != nullptr, so an ongoing animation can leave stale
    // geometry from the old room alive past a switch — clearing here ensures
    // no old entry survives to misdirect a click in the new room.
    messages_.clear();
    clear_hit_geometry_();
    adapter_->clear_layout_cache();
    video_playlist_.clear();
    invalidate_data();

    // Arm the delayed spinner: only surface it if the load outlasts the delay,
    // so fast / warm switches show nothing transient. Guarded by alive_ (the
    // view may be destroyed before the timer fires) and the epoch.
    if (post_delayed_)
    {
        std::weak_ptr<bool> walive = alive_;
        const std::uint64_t ep     = switch_epoch_;
        post_delayed_(kSwitchSpinnerDelayMs,
                      [this, walive, ep]()
                      {
                          auto live = walive.lock();
                          if (!live || !*live)
                              return;
                          if (ep != switch_epoch_ || !switch_loading_)
                              return;
                          switch_spinner_due_   = true;
                          switch_spinner_start_ = std::chrono::steady_clock::now();
                          if (request_repaint_)
                              request_repaint_();
                      });
    }
}

void MessageListView::end_switch_loading()
{
    if (!switch_loading_)
        return;
    // Bump the epoch so any pending delayed-spinner timer is neutralised, then
    // settle on the (already-empty) row set. No populated snapshot is coming.
    ++switch_epoch_;
    switch_loading_     = false;
    switch_spinner_due_ = false;
    invalidate_data();
    if (request_repaint_)
        request_repaint_();
}

void MessageListView::begin_nav_loading()
{
    ++nav_epoch_;
    nav_loading_     = true;
    nav_spinner_due_ = false;
    if (request_repaint_)
        request_repaint_();

    if (post_delayed_)
    {
        std::weak_ptr<bool> walive = alive_;
        const std::uint64_t ep    = nav_epoch_;
        post_delayed_(kNavSpinnerDelayMs,
                      [this, walive, ep]()
                      {
                          auto live = walive.lock();
                          if (!live || !*live)
                              return;
                          if (ep != nav_epoch_ || !nav_loading_)
                              return;
                          nav_spinner_due_   = true;
                          nav_spinner_start_ = std::chrono::steady_clock::now();
                          if (request_repaint_)
                              request_repaint_();
                      });
    }
}

void MessageListView::end_nav_loading()
{
    if (!nav_loading_)
        return;
    ++nav_epoch_;
    nav_loading_     = false;
    nav_spinner_due_ = false;
    if (request_repaint_)
        request_repaint_();
}

void MessageListView::draw_nav_spinner_(tk::PaintCtx& ctx)
{
    draw_spinner_dots_(ctx,
                       bounds_.x + bounds_.w * 0.5f,
                       bounds_.y + bounds_.h * 0.5f,
                       nav_spinner_start_,
                       /*radius=*/12.0f, /*dot_r=*/3.0f);
}

void MessageListView::draw_spinner_dots_(tk::PaintCtx& ctx, float cx, float cy,
                                         std::chrono::steady_clock::time_point
                                             start,
                                         float radius, float dot_r)
{
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
    tk::draw_spinner_dots(ctx.canvas, {cx, cy}, phase, radius, dot_r,
                          ctx.theme.palette.text_muted);
    if (request_repaint_)
        request_repaint_(); // self-animate the next frame while visible
}

void MessageListView::draw_switch_spinner_(tk::PaintCtx& ctx)
{
    draw_spinner_dots_(ctx, bounds_.x + bounds_.w * 0.5f,
                       bounds_.y + bounds_.h * 0.5f, switch_spinner_start_,
                       /*radius=*/12.0f, /*dot_r=*/3.0f);
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
        const LinkLayout* le = link_cache_.peek(m.event_id);
        if (!le)
            continue;
        int lo_b = (i == ord->lo_idx) ? ord->lo_byte : 0;
        int hi_b = (i == ord->hi_idx)
                       ? ord->hi_byte
                       : static_cast<int>(le->plain.size());
        std::string seg = le->layout->text_range(lo_b, hi_b);
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
    if (nav_loading_)
        return false;

    // Scrollbar thumb wins any hit test — it overlaps row content visually.
    if (thumb_hit(local))
        return tk::ListView::on_pointer_down(local);

    // Any new press outside a text-body selection clears the old selection.
    if (sel_ && !press_sel_)
    {
        sel_.reset();
        sel_is_dragging_ = false;
        if (request_repaint_)
            request_repaint_();
    }

    // Thread-preview chip hit: fires on_thread_preview_clicked(root_event_id)
    // and consumes the event so the message-row click handler doesn't also
    // see it. World coordinates because chip_hit_rects_ is recorded in
    // world space by paint_row.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& hit : chip_hit_rects_)
        {
            if (rect_contains(hit.rect, world))
            {
                if (on_thread_preview_clicked)
                {
                    on_thread_preview_clicked(hit.root_event_id);
                }
                return true;
            }
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
                map_panner_.begin_pan(ri, local, messages_[ri].map_viewport);
                return true;
            }
        }
    }

    // Voice card hit-test — handled before chips because the voice
    // controls sit in the body block, separate from the trailing chips
    // strip. Check pill (small) then play button then waveform strip.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        for (const auto& [event_id, geom] : media_.voice_geom())
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
                        media_.handle_voice_scrub_at(row, world.x);
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
        for (const auto& [event_id, geom] : media_.audio_geom())
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
                        media_.handle_audio_scrub_at(row, world.x);
                        break;
                    }
                }
                return true;
            }
        }
    }

    // React button hit-test — leftmost cell of the action pill. Checked
    // before reaction chips so the pill cell wins over an underlying chip.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& rkb = hovered_row_geom_.react_button;
        if (rkb.w > 0 && rect_contains(rkb, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                press_react_btn_ = true;
                press_react_event_id_ = messages_[row].event_id;
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

    // Thread button hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& tb = hovered_row_geom_.thread_button;
        if (tb.w > 0 && rect_contains(tb, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                press_thread_btn_ = true;
                press_thread_event_id_ = messages_[row].event_id;
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

    // More (⋯) button hit-test.
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        const tk::Rect& mb = hovered_row_geom_.more_button;
        if (mb.w > 0 && rect_contains(mb, world))
        {
            std::size_t row = hovered_row_geom_.row_index;
            if (row < messages_.size())
            {
                const auto& m = messages_[row];
                press_more_btn_         = true;
                press_more_event_id_    = m.event_id;
                press_more_can_delete_  = m.is_own;
                press_more_can_pin_     = can_pin_;
                press_more_is_pinned_   = pinned_event_ids_.find(m.event_id) !=
                                          pinned_event_ids_.end();
                press_more_can_forward_ =
                    m.kind != MessageRowData::Kind::Redacted &&
                    m.pending_state == MessageRowData::PendingState::None;
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
        for (const auto& [eid, hit] : previews_.geometry())
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
            const LinkLayout* le = link_cache_.peek(m.event_id);
            if (le)
            {
                press_link_url_ = link_at_world(*le, world);
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
                const bool is_virtual = is_virtual_event(m.kind);
                if (!is_virtual && !adapter_->is_cont(ri) && !m.sender.empty())
                {
                    const tk::Rect rb = row_world_rect(ri_int);
                    if (rb.w > 0)
                    {
                        const tk::Rect avatar_rect{rb.x + kMsgListPadX, rb.y + kMsgListPadY,
                                                   kMsgListAvatarSize, kMsgListAvatarSize};
                        const float col_x = rb.x + kMsgListPadX + kMsgListAvatarSize + kMsgListAvatarGap;
                        const float sender_y =
                            rb.y + kMsgListPadY + (kMsgListAvatarSize - kSenderH) * 0.5f;
                        // Cap at 200px so clicking message body doesn't trigger.
                        const float name_max_w =
                            std::min(200.0f, std::max(0.0f, rb.w - kMsgListPadX -
                                                                 kMsgListAvatarSize -
                                                                 kMsgListAvatarGap - kMsgListPadX));
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
            if (m.kind == MessageRowData::Kind::Membership)
            {
                std::size_t start = adapter_->membership_group_start_of(row);
                std::size_t end = adapter_->membership_group_end(start);
                // Single-member "groups" have nothing to expand; leave the
                // click unconsumed (falls through, same as other system rows).
                if (end - start > 1)
                {
                    press_membership_group_ = true;
                    press_membership_group_key_ = messages_[start].event_id;
                    return true;
                }
            }
            if (m.formatted_body.find("data-mx-spoiler") != std::string::npos &&
                !spoilers_.is_revealed(m.event_id))
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
                const LinkLayout* le = link_cache_.peek(m.event_id);
                if (le)
                {
                    int idx = char_at_world(*le, world);
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

                        const std::string& plain = le->plain;
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
    if (nav_loading_)
        return;

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

    // Map pan: end drag. A release with no meaningful movement is treated
    // as a click, opening the location on openstreetmap.org.
    if (map_panner_.panning())
    {
        std::size_t ri = map_panner_.active_row();
        bool was_click = map_panner_.end_pan(local);
        tk::ListView::on_pointer_up(local, inside_self);
        if (was_click && inside_self && ri < messages_.size() &&
            on_link_clicked)
        {
            const MessageRowData& m = messages_[ri];
            on_link_clicked(osm_view_url(m.location_lat, m.location_lon,
                                          m.map_viewport.zoom));
        }
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
            spoilers_.reveal(eid);
            invalidate_data();
        }
        return;
    }
    if (press_membership_group_)
    {
        bool fire = inside_self && !press_membership_group_key_.empty();
        std::string key = std::move(press_membership_group_key_);
        press_membership_group_ = false;
        press_membership_group_key_.clear();
        if (fire)
        {
            membership_groups_.toggle(key);
            int start_idx = message_index_of(key);
            if (start_idx >= 0)
            {
                std::size_t start = static_cast<std::size_t>(start_idx);
                std::size_t end = adapter_->membership_group_end(start);
                invalidate_rows(start, end);
            }
            else
            {
                invalidate_data();
            }
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
        const auto* geom = media_.audio_geom_at(ev);
        if (!geom)
        {
            return;
        }
        if (!rect_contains(geom->play_button, world))
        {
            return;
        }
        for (const auto& row : messages_)
        {
            if (row.event_id == ev && row.kind == MessageRowData::Kind::Audio)
            {
                media_.handle_audio_play_click(row);
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
        const auto* geom = media_.voice_geom_at(ev);
        if (!geom)
        {
            return;
        }
        const tk::Rect& target = (kind == VoicePressKind::PlayButton)
                                     ? geom->play_button
                                     : geom->speed_pill;
        if (target.w <= 0 || !rect_contains(target, world))
        {
            return;
        }
        if (kind == VoicePressKind::SpeedPill)
        {
            media_.handle_voice_speed_click();
            return;
        }
        // PlayButton.
        for (const auto& row : messages_)
        {
            if (row.event_id == ev && row.kind == MessageRowData::Kind::Voice)
            {
                media_.handle_voice_play_click(row);
                return;
            }
        }
        return;
    }
    if (press_react_btn_)
    {
        bool fire = inside_self && !press_react_event_id_.empty();
        std::string ev = std::move(press_react_event_id_);
        press_react_btn_ = false;
        press_react_event_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& rkb = hovered_row_geom_.react_button;
            if (rkb.w > 0 && rect_contains(rkb, world))
            {
                if (on_add_reaction_requested)
                {
                    on_add_reaction_requested(ev, rkb);
                }
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
    if (press_thread_btn_)
    {
        bool fire = inside_self && !press_thread_event_id_.empty();
        std::string ev = std::move(press_thread_event_id_);
        press_thread_btn_ = false;
        press_thread_event_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& tb = hovered_row_geom_.thread_button;
            if (tb.w > 0 && rect_contains(tb, world))
            {
                if (on_thread_preview_clicked)
                {
                    on_thread_preview_clicked(ev);
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
    if (press_more_btn_)
    {
        bool fire = inside_self && !press_more_event_id_.empty();
        std::string ev = std::move(press_more_event_id_);
        press_more_btn_ = false;
        press_more_event_id_.clear();
        if (fire)
        {
            tk::Point world{local.x + bounds().x, local.y + bounds().y};
            const tk::Rect& mb = hovered_row_geom_.more_button;
            if (mb.w > 0 && rect_contains(mb, world))
            {
                if (on_more_requested)
                {
                    on_more_requested(ev, mb,
                                      press_more_can_delete_,
                                      press_more_can_pin_,
                                      press_more_is_pinned_,
                                      press_more_can_forward_);
                }
            }
        }
        press_more_can_delete_  = false;
        press_more_can_pin_     = false;
        press_more_is_pinned_   = false;
        press_more_can_forward_ = false;
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
        if (inside_self)
        {
            // A matrix.to user link is a mention pill — open the profile panel
            // instead of a browser.
            std::string uid = mention_user_id_from_url(url);
            if (!uid.empty() && on_mention_clicked)
            {
                on_mention_clicked(uid);
            }
            else if (on_link_clicked)
            {
                on_link_clicked(url);
            }
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
            for (const auto& [eid, hit] : previews_.geometry())
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
                rect_contains(it->second.world_rect, world))
            {
                // MSC4278: reveal a suppressed video instead of opening it.
                if (media_is_hidden_by_eid_(eid))
                {
                    if (on_reveal_media)
                        on_reveal_media(eid);
                }
                else if (on_video_clicked)
                {
                    on_video_clicked(it->second);
                }
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
                rect_contains(it->second.world_rect, world))
            {
                // MSC4278: a click on a suppressed-media tile reveals it
                // instead of opening the viewer.
                if (media_is_hidden_by_eid_(eid))
                {
                    if (on_reveal_media)
                        on_reveal_media(eid);
                }
                else if (on_image_clicked)
                {
                    on_image_clicked(it->second);
                }
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
            const auto& r = reactions[idx];
            const std::string src =
                r.source ? r.source->mxc_url() : std::string();
            on_reaction_toggled(ev, r.key, src);
        }
    }
    else if (t == HoverTarget::AddButton)
    {
        // Re-confirm the release lands on the in-strip + chip before firing.
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
    if (!receipt_tracker_.should_fire(eid))
    {
        return;
    }
    receipt_tracker_.mark(eid);
    on_receipt_needed(eid);
}

std::vector<std::string> MessageListView::collect_visible_media_keys_() const
{
    std::vector<std::string> keys;
    auto [first, last] = visible_range();
    // visible_range() can return last == messages_.size() (the virtual typing
    // row); clamp to the real rows.
    int actual_last = std::min(last, static_cast<int>(messages_.size()) - 1);
    if (first < 0 || actual_last < first)
    {
        return keys;
    }
    using Kind = MessageRowData::Kind;
    for (int i = first; i <= actual_last; ++i)
    {
        const auto& m = messages_[static_cast<std::size_t>(i)];
        if (m.kind == Kind::DaySeparator || m.kind == Kind::ReadMarker ||
            m.kind == Kind::TimelineStart)
        {
            continue;
        }
        // The image provider displays thumbnail-if-present, else the source;
        // its fetch_token() is exactly the key the shell registered for the
        // fetch, so this is the token to re-prioritize.
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        if (look)
        {
            std::string tok = look->fetch_token();
            if (!tok.empty())
            {
                keys.push_back(std::move(tok));
            }
        }
    }
    return keys;
}

std::vector<std::string> MessageListView::collect_visible_avatar_urls_() const
{
    std::vector<std::string> urls;
    auto [first, last] = visible_range();
    int actual_last = std::min(last, static_cast<int>(messages_.size()) - 1);
    if (first < 0 || actual_last < first)
    {
        return urls;
    }
    std::unordered_set<std::string> seen;
    for (int i = first; i <= actual_last; ++i)
    {
        const auto& m = messages_[static_cast<std::size_t>(i)];
        if (!m.sender_avatar_url.empty() && seen.insert(m.sender_avatar_url).second)
        {
            urls.push_back(m.sender_avatar_url);
        }
        if (!m.membership_target_avatar_url.empty() &&
            seen.insert(m.membership_target_avatar_url).second)
        {
            urls.push_back(m.membership_target_avatar_url);
        }
        for (const auto& rr : m.read_receipts)
        {
            if (!rr.avatar_url.empty() && seen.insert(rr.avatar_url).second)
            {
                urls.push_back(rr.avatar_url);
            }
        }
    }
    return urls;
}

void MessageListView::maybe_notify_visible_range_() const
{
    if (on_visible_range_changed)
    {
        auto keys = collect_visible_media_keys_();
        if (keys != last_visible_media_keys_)
        {
            last_visible_media_keys_ = keys;
            if (!keys.empty())
            {
                on_visible_range_changed(keys);
            }
        }
    }
    if (on_visible_avatars_changed)
    {
        auto urls = collect_visible_avatar_urls_();
        if (urls != last_visible_avatar_urls_)
        {
            last_visible_avatar_urls_ = urls;
            if (!urls.empty())
            {
                on_visible_avatars_changed(urls);
            }
        }
    }
}

void MessageListView::paint(tk::PaintCtx& ctx)
{
    // Geometry maps are rebuilt by Adapter::paint_row for every painted row.
    // Skip the clear during animation-tick partial repaints (anim_damage != nullptr):
    // the clip covers only the dirty region, so rows outside it aren't repainted and
    // would lose their hit-test entries until the next full repaint.
    if (!ctx.anim_damage)
        clear_hit_geometry_();

    // Room-switch loading: the old room's rows were cleared on the click; show a
    // clean background until the new room's snapshot lands (set_messages cancels
    // this). A centered spinner appears only once the delay has elapsed, so fast
    // / warm switches show nothing transient. Mutually exclusive with the gate
    // below (begin_switch_loading clears the gate; set_messages re-arms it).
    if (switch_loading_)
    {
        ctx.canvas.fill_rect(bounds(), ctx.theme.palette.sidebar_bg);
        if (switch_spinner_due_)
            draw_switch_spinner_(ctx);
        return;
    }

    // Room-switch gate: hold the list invisible until the rows that will
    // be visible have their height-affecting content loaded + measured, so
    // the room appears once, already correct, instead of reflowing as
    // async media / preview cards arrive.
    if (room_switch_gate_.active())
    {
        // Heights must be measured to know the visible band; this also
        // re-snaps to the bottom as async content grows it.
        tk::ListView::ensure_measured(ctx);
        // First-paint dependency scan over the visible band. The keeper owns
        // the gate state + per-Kind dep check; we feed it the visible rows
        // (it can't see visible_range()/messages_).
        room_switch_gate_.evaluate(
            [this](const std::function<void(const MessageRowData&)>& visit)
            {
                auto [first, last] = visible_range();
                if (first < 0 || last < first)
                {
                    return; // nothing visible → reveal
                }
                for (int i = first;
                     i <= last && i < static_cast<int>(messages_.size()); ++i)
                {
                    visit(messages_[static_cast<std::size_t>(i)]);
                }
            });
        if (room_switch_gate_.blocking())
        {
            // Paint only the background tk::ListView::paint would draw,
            // then skip the rows + every overlay below.
            ctx.canvas.fill_rect(bounds(), ctx.theme.palette.sidebar_bg);
            return;
        }
        room_switch_gate_.try_reveal(); // deps resolved (or timed out)
    }

    tk::ListView::paint(ctx);

    // Back-pagination spinner: 8-dot rotating indicator at the top of the
    // viewport while a back-paginate request is in flight.
    if (paginating_)
    {
        ctx.canvas.push_clip_rect(bounds_);
        draw_pagination_spinner_(ctx);
        ctx.canvas.pop_clip();
    }

    // Focused-thread dim. Painted above rows / below scroll-pill + hover
    // tooltips. When highlighted_event_id_ is set we punch the matching row
    // out of the scrim so it reads at full brightness — the un-dimmed row
    // is what visually anchors the open thread panel to the main timeline.
    if (dimmed_)
    {
        const tk::Rect B = bounds();
        const tk::Color scrim = tk::Color::rgba(0, 0, 0, 60);

        tk::Rect cutout{};
        if (!highlighted_event_id_.empty())
        {
            for (std::size_t i = 0; i < messages_.size(); ++i)
            {
                if (messages_[i].event_id != highlighted_event_id_)
                    continue;
                const tk::Rect r =
                    tk::ListView::row_world_rect(static_cast<int>(i));
                // Clip the row rect into the list bounds so a partially
                // scrolled row punches only its visible slice.
                const float x0 = std::max(B.x, r.x);
                const float y0 = std::max(B.y, r.y);
                const float x1 = std::min(B.right(), r.right());
                const float y1 = std::min(B.bottom(), r.bottom());
                if (x1 > x0 && y1 > y0)
                    cutout = {x0, y0, x1 - x0, y1 - y0};
                break;
            }
        }

        if (cutout.empty())
        {
            ctx.canvas.fill_rect(B, scrim);
        }
        else
        {
            const float top_h   = cutout.y - B.y;
            const float bot_y   = cutout.bottom();
            const float bot_h   = B.bottom() - bot_y;
            const float left_w  = cutout.x - B.x;
            const float right_x = cutout.right();
            const float right_w = B.right() - right_x;
            if (top_h > 0)
                ctx.canvas.fill_rect({B.x, B.y, B.w, top_h}, scrim);
            if (bot_h > 0)
                ctx.canvas.fill_rect({B.x, bot_y, B.w, bot_h}, scrim);
            if (left_w > 0)
                ctx.canvas.fill_rect({B.x, cutout.y, left_w, cutout.h},
                                     scrim);
            if (right_w > 0)
                ctx.canvas.fill_rect({right_x, cutout.y, right_w, cutout.h},
                                     scrim);
        }
    }

    // Nav-loading overlay: dim + spinner while subscribe_room_at rebuilds the
    // focused timeline. Spinner deferred kNavSpinnerDelayMs; fast resolves show
    // nothing. Rows beneath remain visible through the scrim.
    if (nav_loading_ && nav_spinner_due_)
    {
        ctx.canvas.fill_rect(bounds(), tk::Color::rgba(0, 0, 0, 60));
        draw_nav_spinner_(ctx);
    }

    // Search match outline — 2px accent stroke around the focused match row.
    // Only drawn when there is an active search (search_match_ids_ non-empty)
    // and a focused match (highlighted_event_id_ non-empty).
    if (!highlighted_event_id_.empty() && !search_match_ids_.empty())
    {
        for (std::size_t i = 0; i < messages_.size(); ++i)
        {
            if (messages_[i].event_id != highlighted_event_id_)
                continue;
            const tk::Rect B = bounds();
            const tk::Rect r =
                tk::ListView::row_world_rect(static_cast<int>(i));
            const float x0 = std::max(B.x, r.x);
            const float y0 = std::max(B.y, r.y);
            const float x1 = std::min(B.right(), r.right());
            const float y1 = std::min(B.bottom(), r.bottom());
            if (x1 > x0 && y1 > y0)
                ctx.canvas.stroke_rect({x0, y0, x1 - x0, y1 - y0},
                                       ctx.theme.palette.accent, 2.0f);
            break;
        }
    }

    maybe_notify_receipt_();
    maybe_notify_visible_range_();

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
    scroll_pill_->set_visible(pill_visible_);
    if (pill_visible_)
    {
        constexpr float kSz = 36.0f, kInsetR = 12.0f, kInsetB = 16.0f;
        tk::Rect v = bounds();
        pill_rect_ = {v.x + v.w - kSz - kInsetR, v.y + v.h - kSz - kInsetB, kSz, kSz};
        scroll_pill_->set_bounds(pill_rect_);
        scroll_pill_->paint(ctx);
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
        // MSC4027 custom-image reactions: `r.key` IS the mxc:// URI (matrix-
        // sdk aggregates by raw key, not by per-event shortcode). Resolve
        // the shortcode from the local emoticon packs so we render
        // `:shortcode:` instead of the raw mxc string; fall back to plain
        // "Reacted by:" when the mxc isn't in any of the user's packs.
        std::string header;
        if (r.source && shortcode_provider_)
        {
            std::string sc = shortcode_provider_(r.source->mxc_url());
            if (!sc.empty())
            {
                // Shortcode already supplies its trailing ':' so we don't
                // append the extra punctuation colon the Unicode branch uses.
                header = tk::tr("Reacted with :");
                header += sc;
                header += ":";
            }
        }
        if (header.empty())
        {
            if (r.source)
            {
                header = tk::tr("Reacted by:");
            }
            else
            {
                header = tk::tr("Reacted with ");
                header += r.key;
                header += ":";
            }
        }
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
