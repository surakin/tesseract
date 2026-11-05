#include "app/ScreenshotFixture.h"

#include <tesseract/media_source.h>

#include <string_view>
#include <utility>

#ifndef TESSERACT_SCREENSHOT_ASSETS_DIR
#error "TESSERACT_SCREENSHOT_ASSETS_DIR must name the screenshot assets directory"
#endif

namespace tesseract::screenshot
{
namespace
{

constexpr std::uint64_t kBaseTime = 1785661200000ULL; // 2026-08-02 09:00 UTC

// Real files on disk, referenced as mxc://<absolute path> (see local_media_path).
#define SCREENSHOT_ASSET(name) "mxc://" TESSERACT_SCREENSHOT_ASSETS_DIR "/" name

constexpr char kAlexAvatar[] = SCREENSHOT_ASSET("alex.jpg");
constexpr char kMayaAvatar[] = SCREENSHOT_ASSET("maya.jpg");
constexpr char kSamAvatar[] = SCREENSHOT_ASSET("sam.jpg");
constexpr char kRobinAvatar[] = SCREENSHOT_ASSET("robin.jpg");
constexpr char kTesseractRoomAvatar[] = SCREENSHOT_ASSET("tesseract.png");
constexpr char kDesignRoomAvatar[] = SCREENSHOT_ASSET("design.jpg");
constexpr char kCommunityRoomAvatar[] = SCREENSHOT_ASSET("community.jpg");
constexpr char kCorgiPhoto[] = SCREENSHOT_ASSET("corgi.jpg");
constexpr char kStickerChip[] = SCREENSHOT_ASSET("sticker.jpg");
// Never fetched: voice playback and file download need a user click.
constexpr char kVoiceClip[] = "mxc://tesseract.test/voice-clip";
constexpr char kReleaseNotes[] = "mxc://tesseract.test/release-notes";

#undef SCREENSHOT_ASSET

constexpr char kAlex[] = "@alex:tesseract.test";
constexpr char kMaya[] = "@maya:tesseract.test";
constexpr char kSam[] = "@sam:tesseract.test";
constexpr char kRobin[] = "@robin:tesseract.test";

std::string avatar_for_sender(const std::string& sender)
{
    if (sender == kAlex)
    {
        return kAlexAvatar;
    }
    if (sender == kMaya)
    {
        return kMayaAvatar;
    }
    if (sender == kSam)
    {
        return kSamAvatar;
    }
    if (sender == kRobin)
    {
        return kRobinAvatar;
    }
    return {};
}

std::uint64_t at(std::uint64_t minutes)
{
    return kBaseTime + minutes * 60'000ULL;
}

views::MessageRowData row(views::MessageRowData::Kind kind, std::string id, std::string sender,
                          std::string name, std::uint64_t minutes, bool own)
{
    views::MessageRowData row;
    row.kind = kind;
    row.event_id = std::move(id);
    row.sender = std::move(sender);
    row.sender_avatar_url = avatar_for_sender(row.sender);
    row.sender_name = std::move(name);
    row.timestamp_ms = at(minutes);
    row.is_own = own;
    return row;
}

views::MessageRowData text(std::string id, std::string sender, std::string name, std::string body,
                           std::uint64_t minutes, bool own = false)
{
    auto out = row(views::MessageRowData::Kind::Text, std::move(id), std::move(sender),
                   std::move(name), minutes, own);
    out.body = std::move(body);
    return out;
}

tesseract::RoomInfo room(std::string id, std::string name, std::string avatar,
                         std::string last_sender, std::string last_body, std::uint64_t minutes)
{
    tesseract::RoomInfo r;
    r.id = std::move(id);
    r.name = std::move(name);
    r.avatar_url = std::move(avatar);
    r.last_message_sender_name = std::move(last_sender);
    r.last_message_body = std::move(last_body);
    r.last_message_kind = "text";
    r.last_activity_ts = at(minutes);
    return r;
}

std::vector<tesseract::RoomInfo> make_rooms(const std::string& selected)
{
    auto project = room(selected, "Tesseract", kTesseractRoomAvatar, "Maya",
                        "just tried dark mode on my laptop and wow, it looks so good", 42);
    project.topic = "Building a fast, native Matrix client";
    project.is_favorite = true;
    project.is_encrypted = true;

    auto launch = room("!launch:tesseract.test", "Launch Planning", {}, "Maya",
                       "@Alex could you give the release notes a once-over?", 44);
    launch.notification_count = 3;
    launch.highlight_count = 1;
    launch.unread_count = 3;

    auto design = room("!design:tesseract.test", "Design", kDesignRoomAvatar, "Sam",
                       "left a few comments on the mockup", 35);
    design.topic = "Mockups, icons and bikeshedding";
    design.notification_count = 2;
    design.unread_count = 2;

    auto sam = room("!sam:tesseract.test", "Sam Rivera", kSamAvatar, "Sam", {}, 30);
    sam.is_direct = true;
    sam.dm_counterpart_user_id = kSam;
    sam.last_message_kind = "sticker";
    sam.last_message_sticker_url = kStickerChip;
    sam.last_message_thumbnail_url = kStickerChip;

    auto standup = room("!standup:tesseract.test", "Daily Standup", {}, "Robin",
                        "running 2 min late, start without me", 28);
    standup.is_call_room = true;
    standup.has_active_call = true;
    standup.call_members = {kSam, kRobin};
    standup.call_intent = "video";

    auto maya = room("!maya:tesseract.test", "Maya Chen", kMayaAvatar, "Maya",
                     "haha yes, see you at the meetup tomorrow", 21);
    maya.is_direct = true;
    maya.dm_counterpart_user_id = kMaya;

    auto offtopic = room("!offtopic:tesseract.test", "Off-topic", {}, "Robin",
                         "sorry, that was my cat walking across the keyboard", 15);
    offtopic.muted = true;
    offtopic.unread_count = 5;

    auto community = room("!community:tesseract.test", "Matrix Community", kCommunityRoomAvatar,
                          "Robin", "does anyone have a good guide for self-hosting Synapse?", 10);
    community.unread_count = 1;

    auto space =
        room("!space:tesseract.test", "Tesseract Project", kTesseractRoomAvatar, {}, {}, 5);
    space.is_space = true;
    space.last_message_kind = {};

    auto archive = room("!archive:tesseract.test", "Old Announcements", {}, "Alex",
                        "0.9 is out \xF0\x9F\x8E\x89 thanks everyone", 1);
    archive.is_low_priority = true;

    std::vector<tesseract::RoomInfo> out;
    for (auto* r : {&project, &launch, &design, &sam, &standup, &maya, &offtopic, &community,
                    &space, &archive})
    {
        out.push_back(std::move(*r));
    }
    return out;
}

} // namespace

std::optional<std::filesystem::path> local_media_path(std::string_view url)
{
    constexpr std::string_view kScheme = "mxc://";
    if (!url.starts_with(kScheme))
    {
        return std::nullopt;
    }
    const std::filesystem::path path{std::string(url.substr(kScheme.size()))};
    if (!path.is_absolute())
    {
        return std::nullopt;
    }
    return path;
}

Fixture make_fixture()
{
    using Kind = views::MessageRowData::Kind;

    Fixture out;
    out.user_id = kAlex;
    out.display_name = "Alex";
    out.avatar_url = kAlexAvatar;
    out.selected_room_id = "!tesseract:tesseract.test";
    out.rooms = make_rooms(out.selected_room_id);
    out.thread_root_id = "$thread";
    out.typing_names = {"Maya"};

    views::MessageRowData day;
    day.kind = Kind::DaySeparator;
    day.event_id = "$day";
    day.timestamp_ms = kBaseTime;
    out.messages.push_back(std::move(day));

    auto join = row(Kind::Membership, "$join", kSam, "Sam", 1, false);
    join.membership_action = tesseract::MembershipAction::Joined;
    join.membership_target_user_id = kSam;
    join.membership_target_name = "Sam";
    join.membership_target_avatar_url = kSamAvatar;
    out.messages.push_back(std::move(join));

    auto plan = text("$plan", kMaya, "Maya",
                     "morning \xE2\x98\x95 what's left before we tag 0.10:\n"
                     "- thread panel polish\n"
                     "- bump matrix-sdk (again \xF0\x9F\x99\x83)\n"
                     "- new screenshots",
                     3);
    plan.formatted_body = "<p>morning \xE2\x98\x95 what's left before we tag <code>0.10</code>:</p>"
                          "<ul><li>thread panel polish</li>"
                          "<li>bump <code>matrix-sdk</code> (again \xF0\x9F\x99\x83)</li>"
                          "<li>new screenshots</li></ul>";
    out.messages.push_back(std::move(plan));

    const auto corgi = tesseract::MediaSource::plain(kCorgiPhoto);
    const std::string corgi_caption = "look who decided to join the standup \xF0\x9F\x90\xB6";
    auto image = row(Kind::Image, "$corgi", kMaya, "Maya", 6, false);
    image.source = corgi;
    image.media_w = 800;
    image.media_h = 600;
    // The body is shown as the caption, the file name stays secondary.
    image.has_filename_caption = true;
    image.file_name = "IMG_4127.jpg";
    image.body = corgi_caption;
    out.messages.push_back(std::move(image));

    auto face =
        text("$face", kAlex, "Alex", "omg that face \xF0\x9F\x98\x8D is that Biscuit?", 8, true);
    face.in_reply_to_id = "$corgi";
    face.in_reply_to_sender_name = "Maya";
    face.in_reply_to_body = corgi_caption;
    face.in_reply_to_image_source = corgi;
    face.reactions.push_back(
        {"\xE2\x9D\xA4\xEF\xB8\x8F", 3, true, nullptr, {"Alex", "Maya", "Robin"}});
    face.reactions.push_back({"\xF0\x9F\x98\x82", 2, false, nullptr, {"Sam", "Robin"}});
    out.messages.push_back(std::move(face));

    auto code = text("$code", kSam, "Sam",
                     "found the scroll jump!! we never re-anchored after images finished "
                     "loading:\n"
                     "if (anchor.visible())\n"
                     "    list.scroll_to(anchor.index(), Align::Bottom);",
                     14);
    code.formatted_body = "<p>found the scroll jump!! we never re-anchored after images finished "
                          "loading:</p>"
                          "<pre><code>if (anchor.visible())\n"
                          "    list.scroll_to(anchor.index(), Align::Bottom);</code></pre>";
    out.messages.push_back(std::move(code));

    auto voice = row(Kind::Voice, "$voice", kRobin, "Robin", 19, false);
    voice.audio_source = tesseract::MediaSource::plain(kVoiceClip);
    voice.audio_mime = "audio/ogg";
    voice.duration_ms = 23'000;
    voice.body = "Voice message";
    // Speech-like: phrases separated by short pauses.
    voice.waveform = {120, 340, 610, 820, 560, 300, 710, 880, 640, 410, 180, 90,
                      60,  250, 590, 760, 930, 700, 480, 650, 820, 540, 260, 110,
                      70,  80,  320, 680, 850, 610, 390, 720, 560, 330, 470, 600,
                      410, 230, 140, 300, 450, 380, 260, 170, 110, 70,  50,  40};
    out.messages.push_back(std::move(voice));

    auto thread = text("$thread", kRobin, "Robin",
                       "tl;dr for anyone who can't listen right now: threads on by default in "
                       "new rooms, yes or no?",
                       24);
    thread.is_thread_root = true;
    thread.thread_reply_count = 4;
    thread.thread_latest_sender_name = "Maya";
    thread.thread_latest_body = "fwiw the panel's been solid all week for me";
    thread.thread_latest_ts = at(40);
    out.thread_messages.push_back(thread);
    out.messages.push_back(std::move(thread));

    auto notes = row(Kind::File, "$notes", kAlex, "Alex", 31, true);
    notes.file_source = tesseract::MediaSource::plain(kReleaseNotes);
    notes.file_name = "release-notes-draft.pdf";
    notes.file_size = 248'000;
    notes.body = "release-notes-draft.pdf";
    out.messages.push_back(std::move(notes));

    auto final = text("$final", kMaya, "Maya",
                      "just tried dark mode on my laptop and wow, it looks so good", 42);
    final.is_edited = true;
    final.read_receipts.push_back({kSam, "Sam", kSamAvatar, at(43)});
    final.read_receipts.push_back({kRobin, "Robin", kRobinAvatar, at(43)});
    out.messages.push_back(std::move(final));

    auto reply = [&](std::string id, const char* sender, std::string name, std::string body,
                     std::uint64_t minutes)
    {
        auto r = text(std::move(id), sender, std::move(name), std::move(body), minutes,
                      std::string_view{sender} == kAlex);
        r.thread_root_id = out.thread_root_id;
        return r;
    };
    out.thread_messages.push_back(
        reply("$t1", kMaya, "Maya", "yes please, the main timeline gets so noisy otherwise", 26));
    out.thread_messages.push_back(reply("$t2", kSam, "Sam", "\xF0\x9F\x91\x8D", 30));
    out.thread_messages.push_back(
        reply("$t3", kAlex, "Alex", "ok I'll flip the default after lunch", 34));
    auto last = reply("$t4", kMaya, "Maya", "fwiw the panel's been solid all week for me", 40);
    last.reactions.push_back({"\xE2\x9D\xA4\xEF\xB8\x8F", 1, false, nullptr, {"Robin"}});
    out.thread_messages.push_back(std::move(last));

    out.members = {
        {kAlex, "Alex", kAlexAvatar, 100},
        {kMaya, "Maya", kMayaAvatar, 100},
        {kSam, "Sam", kSamAvatar, 50},
        {kRobin, "Robin", kRobinAvatar, 0},
        {"@casey:tesseract.test", "Casey", {}, 0},
        {"@drew:tesseract.test", "Drew", {}, 0},
    };
    out.presence = {
        {kAlex, tesseract::PresenceState::Online},
        {kMaya, tesseract::PresenceState::Online},
        {kSam, tesseract::PresenceState::Online},
        {kRobin, tesseract::PresenceState::Unavailable},
        {"@casey:tesseract.test", tesseract::PresenceState::Offline},
        {"@drew:tesseract.test", tesseract::PresenceState::Offline},
    };

    return out;
}

} // namespace tesseract::screenshot
