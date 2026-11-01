#include "app/ScreenshotFixture.h"

#include "tk/pixmap_cache.h"
#include "tk/svg.h"

#include <tesseract/media_source.h>

#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace tesseract::screenshot
{
namespace
{

constexpr std::uint64_t kBaseTime = 1785661200000ULL; // 2026-08-02 09:00 UTC

constexpr char kAlexAvatar[] = "fixture://avatar/alex";
constexpr char kMayaAvatar[] = "fixture://avatar/maya";
constexpr char kSamAvatar[] = "fixture://avatar/sam";
constexpr char kRobinAvatar[] = "fixture://avatar/robin";
constexpr char kTesseractRoomAvatar[] = "fixture://room/tesseract";
constexpr char kDesignRoomAvatar[] = "fixture://room/design";
constexpr char kCommunityRoomAvatar[] = "fixture://room/matrix-community";
constexpr char kReleaseBanner[] = "fixture://media/release-banner";
constexpr char kStickerChip[] = "fixture://media/sticker-chip";
// Never fetched: voice playback and file download need a user click.
constexpr char kVoiceClip[] = "fixture://media/voice-clip";
constexpr char kReleaseNotes[] = "fixture://media/release-notes";

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
                        "Screenshots look great in dark mode too.", 42);
    project.topic = "Building a fast, native Matrix client";
    project.is_favorite = true;
    project.is_encrypted = true;

    auto launch = room("!launch:tesseract.test", "Launch Planning", {}, "Maya",
                       "Alex, can you check the release notes?", 44);
    launch.notification_count = 3;
    launch.highlight_count = 1;
    launch.unread_count = 3;

    auto design = room("!design:tesseract.test", "Design", kDesignRoomAvatar, "Sam",
                       "I added notes to the mockup", 35);
    design.topic = "UI reviews and visual polish";
    design.notification_count = 2;
    design.unread_count = 2;

    auto sam = room("!sam:tesseract.test", "Sam Rivera", kSamAvatar, "Sam", {}, 30);
    sam.is_direct = true;
    sam.dm_counterpart_user_id = kSam;
    sam.last_message_kind = "sticker";
    sam.last_message_sticker_url = kStickerChip;
    sam.last_message_thumbnail_url = kStickerChip;

    auto standup =
        room("!standup:tesseract.test", "Daily Standup", {}, "Robin", "Starting in 2 minutes", 28);
    standup.is_call_room = true;
    standup.has_active_call = true;
    standup.call_members = {kSam, kRobin};
    standup.call_intent = "video";

    auto maya =
        room("!maya:tesseract.test", "Maya Chen", kMayaAvatar, "Maya", "See you tomorrow!", 21);
    maya.is_direct = true;
    maya.dm_counterpart_user_id = kMaya;

    auto offtopic = room("!offtopic:tesseract.test", "Off-topic", {}, "Robin",
                         "Anyone tried the new keyboard?", 15);
    offtopic.muted = true;
    offtopic.unread_count = 5;

    auto community = room("!community:tesseract.test", "Matrix Community", kCommunityRoomAvatar,
                          "Robin", "Welcome to the room", 10);
    community.unread_count = 1;

    auto space =
        room("!space:tesseract.test", "Tesseract Project", kTesseractRoomAvatar, {}, {}, 5);
    space.is_space = true;
    space.last_message_kind = {};

    auto archive =
        room("!archive:tesseract.test", "Old Announcements", {}, "Alex", "0.9 is out!", 1);
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
                     "Release checklist for 0.10:\n"
                     "- Final pass on the thread panel\n"
                     "- Bump matrix-sdk\n"
                     "- Refresh screenshots",
                     3);
    plan.formatted_body = "<p>Release checklist for <code>0.10</code>:</p>"
                          "<ul><li>Final pass on the thread panel</li>"
                          "<li>Bump <code>matrix-sdk</code></li>"
                          "<li>Refresh screenshots</li></ul>";
    out.messages.push_back(std::move(plan));

    const auto banner = tesseract::MediaSource::plain(kReleaseBanner);
    auto image = row(Kind::Image, "$banner", kMaya, "Maya", 6, false);
    image.source = banner;
    image.media_w = 600;
    image.media_h = 340;
    image.has_filename_caption = true;
    image.file_name = "release-banner.png";
    image.body = "New banner for the release post";
    out.messages.push_back(std::move(image));

    auto love = text("$love", kAlex, "Alex", "Love it \xE2\x80\x94 ship it!", 8, true);
    love.in_reply_to_id = "$banner";
    love.in_reply_to_sender_name = "Maya";
    love.in_reply_to_body = "New banner for the release post";
    love.in_reply_to_image_source = banner;
    love.reactions.push_back({"\xF0\x9F\x8E\x89", 3, true, nullptr, {"Alex", "Maya", "Robin"}});
    love.reactions.push_back({"\xF0\x9F\x91\x8D", 2, false, nullptr, {"Sam", "Robin"}});
    out.messages.push_back(std::move(love));

    auto code = text("$code", kSam, "Sam",
                     "Fixed the scroll anchor:\n"
                     "if (anchor.visible())\n"
                     "    list.scroll_to(anchor.index(), Align::Bottom);",
                     14);
    code.formatted_body = "<p>Fixed the scroll anchor:</p>"
                          "<pre><code>if (anchor.visible())\n"
                          "    list.scroll_to(anchor.index(), Align::Bottom);</code></pre>";
    out.messages.push_back(std::move(code));

    auto voice = row(Kind::Voice, "$voice", kRobin, "Robin", 19, false);
    voice.audio_source = tesseract::MediaSource::plain(kVoiceClip);
    voice.audio_mime = "audio/ogg";
    voice.duration_ms = 14'000;
    voice.body = "Voice message";
    for (std::uint16_t i = 0; i < 48; ++i)
    {
        voice.waveform.push_back(static_cast<std::uint16_t>(200 + (i * 37) % 700));
    }
    out.messages.push_back(std::move(voice));

    auto thread =
        text("$thread", kRobin, "Robin", "Should we turn on threads by default for new rooms?", 24);
    thread.is_thread_root = true;
    thread.thread_reply_count = 4;
    thread.thread_latest_sender_name = "Maya";
    thread.thread_latest_body = "Yes \xE2\x80\x94 the panel is solid now.";
    thread.thread_latest_ts = at(40);
    out.thread_messages.push_back(thread);
    out.messages.push_back(std::move(thread));

    auto notes = row(Kind::File, "$notes", kAlex, "Alex", 31, true);
    notes.file_source = tesseract::MediaSource::plain(kReleaseNotes);
    notes.file_name = "release-notes.pdf";
    notes.file_size = 248'000;
    notes.body = "release-notes.pdf";
    out.messages.push_back(std::move(notes));

    auto final = text("$final", kMaya, "Maya", "Screenshots look great in dark mode too.", 42);
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
        reply("$t1", kMaya, "Maya",
              "I think so \xE2\x80\x94 fewer side conversations in the main "
              "timeline.",
              26));
    out.thread_messages.push_back(reply("$t2", kSam, "Sam", "\xF0\x9F\x91\x8D", 30));
    out.thread_messages.push_back(
        reply("$t3", kAlex, "Alex", "Agreed. I'll update the default.", 34));
    auto last = reply("$t4", kMaya, "Maya", "Yes \xE2\x80\x94 the panel is solid now.", 40);
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

namespace
{

constexpr char kAlexPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAACMXRFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDI1NAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwZjExYzAyMDUwMDA3NGM2ZjcyNjU2YzY1NjkxYzAyNTAwMDBmNGM2OTczNjEyMDU3NjkKNzM2MzY4NmY2NjczNmI3OTFjMDI2ZTAwMGY0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzkxYzAyNzQwMGIxNTI2NTZkCjY5NzgyMDZmNjYyMDkzNGM2ZjcyNjU2YzY1Njk5NDIwMjg2ODc0NzQ3MDczM2EyZjJmNzc3Nzc3MmU2NjY5Njc2ZDYxMmU2Mwo2ZjZkMmY2MzZmNmQ2ZDc1NmU2OTc0NzkyZjY2Njk2YzY1MmYzMTMxMzkzODM3MzQzOTM2MzkzMzMyMzgzMDM0MzYzOTM2MzMKMzkyOTIwNjI3OTIwOTM0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzk5NDJjMjA2YzY5NjM2NTZlNzM2NTY0MjA3NTZlCjY0NjU3MjIwOTM0MzQzMzAyMDMxMmUzMDk0MjAyODY4NzQ3NDcwNzMzYTJmMmY2MzcyNjU2MTc0Njk3NjY1NjM2ZjZkNmQ2Zgo2ZTczMmU2ZjcyNjcyZjcwNzU2MjZjNjk2MzY0NmY2ZDYxNjk2ZTJmN2E2NTcyNmYyZjMxMmUzMDJmMjkxYzAyMDAwMDAyMDAKMDQwMAqqw15NAAAF/GlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSfvu78nIGlkPSdXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQnPz4KPHg6eG1wbWV0YSB4bWxuczp4PSdhZG9iZTpuczptZXRhLycgeDp4bXB0az0nSW1hZ2U6OkV4aWZUb29sIDEzLjU5Jz4KPHJkZjpSREYgeG1sbnM6cmRmPSdodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjJz4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOmRjPSdodHRwOi8vcHVybC5vcmcvZGMvZWxlbWVudHMvMS4xLyc+CiAgPGRjOmNyZWF0b3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaT5MaXNhIFdpc2Nob2Zza3k8L3JkZjpsaT4KICAgPC9yZGY6U2VxPgogIDwvZGM6Y3JlYXRvcj4KICA8ZGM6cmlnaHRzPgogICA8cmRmOkFsdD4KICAgIDxyZGY6bGkgeG1sOmxhbmc9J3gtZGVmYXVsdCc+UmVtaXggb2Yg4oCcTG9yZWxlaeKAnSAoaHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0Njk2MzkpIGJ5IOKAnExpc2EgV2lzY2hvZnNreeKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5Mb3JlbGVpPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnRpdGxlPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczpwaG90b3Nob3A9J2h0dHA6Ly9ucy5hZG9iZS5jb20vcGhvdG9zaG9wLzEuMC8nPgogIDxwaG90b3Nob3A6Q3JlZGl0Pkxpc2EgV2lzY2hvZnNreTwvcGhvdG9zaG9wOkNyZWRpdD4KIDwvcmRmOkRlc2NyaXB0aW9uPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6cGx1cz0naHR0cDovL25zLnVzZXBsdXMub3JnL2xkZi94bXAvMS4wLyc+CiAgPHBsdXM6TGljZW5zb3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaSByZGY6cGFyc2VUeXBlPSdSZXNvdXJjZSc+CiAgICAgPHBsdXM6TGljZW5zb3JVUkw+aHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0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)b64";

constexpr char kMayaPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAACMXRFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDI1NAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwZjExYzAyMDUwMDA3NGM2ZjcyNjU2YzY1NjkxYzAyNTAwMDBmNGM2OTczNjEyMDU3NjkKNzM2MzY4NmY2NjczNmI3OTFjMDI2ZTAwMGY0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzkxYzAyNzQwMGIxNTI2NTZkCjY5NzgyMDZmNjYyMDkzNGM2ZjcyNjU2YzY1Njk5NDIwMjg2ODc0NzQ3MDczM2EyZjJmNzc3Nzc3MmU2NjY5Njc2ZDYxMmU2Mwo2ZjZkMmY2MzZmNmQ2ZDc1NmU2OTc0NzkyZjY2Njk2YzY1MmYzMTMxMzkzODM3MzQzOTM2MzkzMzMyMzgzMDM0MzYzOTM2MzMKMzkyOTIwNjI3OTIwOTM0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzk5NDJjMjA2YzY5NjM2NTZlNzM2NTY0MjA3NTZlCjY0NjU3MjIwOTM0MzQzMzAyMDMxMmUzMDk0MjAyODY4NzQ3NDcwNzMzYTJmMmY2MzcyNjU2MTc0Njk3NjY1NjM2ZjZkNmQ2Zgo2ZTczMmU2ZjcyNjcyZjcwNzU2MjZjNjk2MzY0NmY2ZDYxNjk2ZTJmN2E2NTcyNmYyZjMxMmUzMDJmMjkxYzAyMDAwMDAyMDAKMDQwMAqqw15NAAAF/GlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSfvu78nIGlkPSdXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQnPz4KPHg6eG1wbWV0YSB4bWxuczp4PSdhZG9iZTpuczptZXRhLycgeDp4bXB0az0nSW1hZ2U6OkV4aWZUb29sIDEzLjU5Jz4KPHJkZjpSREYgeG1sbnM6cmRmPSdodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjJz4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOmRjPSdodHRwOi8vcHVybC5vcmcvZGMvZWxlbWVudHMvMS4xLyc+CiAgPGRjOmNyZWF0b3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaT5MaXNhIFdpc2Nob2Zza3k8L3JkZjpsaT4KICAgPC9yZGY6U2VxPgogIDwvZGM6Y3JlYXRvcj4KICA8ZGM6cmlnaHRzPgogICA8cmRmOkFsdD4KICAgIDxyZGY6bGkgeG1sOmxhbmc9J3gtZGVmYXVsdCc+UmVtaXggb2Yg4oCcTG9yZWxlaeKAnSAoaHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0Njk2MzkpIGJ5IOKAnExpc2EgV2lzY2hvZnNreeKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5Mb3JlbGVpPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnRpdGxlPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczpwaG90b3Nob3A9J2h0dHA6Ly9ucy5hZG9iZS5jb20vcGhvdG9zaG9wLzEuMC8nPgogIDxwaG90b3Nob3A6Q3JlZGl0Pkxpc2EgV2lzY2hvZnNreTwvcGhvdG9zaG9wOkNyZWRpdD4KIDwvcmRmOkRlc2NyaXB0aW9uPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6cGx1cz0naHR0cDovL25zLnVzZXBsdXMub3JnL2xkZi94bXAvMS4wLyc+CiAgPHBsdXM6TGljZW5zb3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaSByZGY6cGFyc2VUeXBlPSdSZXNvdXJjZSc+CiAgICAgPHBsdXM6TGljZW5zb3JVUkw+aHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0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)b64";

constexpr char kSamPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAACMXRFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDI1NAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwZjExYzAyMDUwMDA3NGM2ZjcyNjU2YzY1NjkxYzAyNTAwMDBmNGM2OTczNjEyMDU3NjkKNzM2MzY4NmY2NjczNmI3OTFjMDI2ZTAwMGY0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzkxYzAyNzQwMGIxNTI2NTZkCjY5NzgyMDZmNjYyMDkzNGM2ZjcyNjU2YzY1Njk5NDIwMjg2ODc0NzQ3MDczM2EyZjJmNzc3Nzc3MmU2NjY5Njc2ZDYxMmU2Mwo2ZjZkMmY2MzZmNmQ2ZDc1NmU2OTc0NzkyZjY2Njk2YzY1MmYzMTMxMzkzODM3MzQzOTM2MzkzMzMyMzgzMDM0MzYzOTM2MzMKMzkyOTIwNjI3OTIwOTM0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzk5NDJjMjA2YzY5NjM2NTZlNzM2NTY0MjA3NTZlCjY0NjU3MjIwOTM0MzQzMzAyMDMxMmUzMDk0MjAyODY4NzQ3NDcwNzMzYTJmMmY2MzcyNjU2MTc0Njk3NjY1NjM2ZjZkNmQ2Zgo2ZTczMmU2ZjcyNjcyZjcwNzU2MjZjNjk2MzY0NmY2ZDYxNjk2ZTJmN2E2NTcyNmYyZjMxMmUzMDJmMjkxYzAyMDAwMDAyMDAKMDQwMAqqw15NAAAF/GlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSfvu78nIGlkPSdXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQnPz4KPHg6eG1wbWV0YSB4bWxuczp4PSdhZG9iZTpuczptZXRhLycgeDp4bXB0az0nSW1hZ2U6OkV4aWZUb29sIDEzLjU5Jz4KPHJkZjpSREYgeG1sbnM6cmRmPSdodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjJz4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOmRjPSdodHRwOi8vcHVybC5vcmcvZGMvZWxlbWVudHMvMS4xLyc+CiAgPGRjOmNyZWF0b3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaT5MaXNhIFdpc2Nob2Zza3k8L3JkZjpsaT4KICAgPC9yZGY6U2VxPgogIDwvZGM6Y3JlYXRvcj4KICA8ZGM6cmlnaHRzPgogICA8cmRmOkFsdD4KICAgIDxyZGY6bGkgeG1sOmxhbmc9J3gtZGVmYXVsdCc+UmVtaXggb2Yg4oCcTG9yZWxlaeKAnSAoaHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0Njk2MzkpIGJ5IOKAnExpc2EgV2lzY2hvZnNreeKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5Mb3JlbGVpPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnRpdGxlPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczpwaG90b3Nob3A9J2h0dHA6Ly9ucy5hZG9iZS5jb20vcGhvdG9zaG9wLzEuMC8nPgogIDxwaG90b3Nob3A6Q3JlZGl0Pkxpc2EgV2lzY2hvZnNreTwvcGhvdG9zaG9wOkNyZWRpdD4KIDwvcmRmOkRlc2NyaXB0aW9uPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6cGx1cz0naHR0cDovL25zLnVzZXBsdXMub3JnL2xkZi94bXAvMS4wLyc+CiAgPHBsdXM6TGljZW5zb3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaSByZGY6cGFyc2VUeXBlPSdSZXNvdXJjZSc+CiAgICAgPHBsdXM6TGljZW5zb3JVUkw+aHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0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)b64";

constexpr char kRobinPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAACMXRFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDI1NAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwZjExYzAyMDUwMDA3NGM2ZjcyNjU2YzY1NjkxYzAyNTAwMDBmNGM2OTczNjEyMDU3NjkKNzM2MzY4NmY2NjczNmI3OTFjMDI2ZTAwMGY0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzkxYzAyNzQwMGIxNTI2NTZkCjY5NzgyMDZmNjYyMDkzNGM2ZjcyNjU2YzY1Njk5NDIwMjg2ODc0NzQ3MDczM2EyZjJmNzc3Nzc3MmU2NjY5Njc2ZDYxMmU2Mwo2ZjZkMmY2MzZmNmQ2ZDc1NmU2OTc0NzkyZjY2Njk2YzY1MmYzMTMxMzkzODM3MzQzOTM2MzkzMzMyMzgzMDM0MzYzOTM2MzMKMzkyOTIwNjI3OTIwOTM0YzY5NzM2MTIwNTc2OTczNjM2ODZmNjY3MzZiNzk5NDJjMjA2YzY5NjM2NTZlNzM2NTY0MjA3NTZlCjY0NjU3MjIwOTM0MzQzMzAyMDMxMmUzMDk0MjAyODY4NzQ3NDcwNzMzYTJmMmY2MzcyNjU2MTc0Njk3NjY1NjM2ZjZkNmQ2Zgo2ZTczMmU2ZjcyNjcyZjcwNzU2MjZjNjk2MzY0NmY2ZDYxNjk2ZTJmN2E2NTcyNmYyZjMxMmUzMDJmMjkxYzAyMDAwMDAyMDAKMDQwMAqqw15NAAAF/GlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSfvu78nIGlkPSdXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQnPz4KPHg6eG1wbWV0YSB4bWxuczp4PSdhZG9iZTpuczptZXRhLycgeDp4bXB0az0nSW1hZ2U6OkV4aWZUb29sIDEzLjU5Jz4KPHJkZjpSREYgeG1sbnM6cmRmPSdodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjJz4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOmRjPSdodHRwOi8vcHVybC5vcmcvZGMvZWxlbWVudHMvMS4xLyc+CiAgPGRjOmNyZWF0b3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaT5MaXNhIFdpc2Nob2Zza3k8L3JkZjpsaT4KICAgPC9yZGY6U2VxPgogIDwvZGM6Y3JlYXRvcj4KICA8ZGM6cmlnaHRzPgogICA8cmRmOkFsdD4KICAgIDxyZGY6bGkgeG1sOmxhbmc9J3gtZGVmYXVsdCc+UmVtaXggb2Yg4oCcTG9yZWxlaeKAnSAoaHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0Njk2MzkpIGJ5IOKAnExpc2EgV2lzY2hvZnNreeKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5Mb3JlbGVpPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnRpdGxlPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczpwaG90b3Nob3A9J2h0dHA6Ly9ucy5hZG9iZS5jb20vcGhvdG9zaG9wLzEuMC8nPgogIDxwaG90b3Nob3A6Q3JlZGl0Pkxpc2EgV2lzY2hvZnNreTwvcGhvdG9zaG9wOkNyZWRpdD4KIDwvcmRmOkRlc2NyaXB0aW9uPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6cGx1cz0naHR0cDovL25zLnVzZXBsdXMub3JnL2xkZi94bXAvMS4wLyc+CiAgPHBsdXM6TGljZW5zb3I+CiAgIDxyZGY6U2VxPgogICAgPHJkZjpsaSByZGY6cGFyc2VUeXBlPSdSZXNvdXJjZSc+CiAgICAgPHBsdXM6TGljZW5zb3JVUkw+aHR0cHM6Ly93d3cuZmlnbWEuY29tL2NvbW11bml0eS9maWxlLzExOTg3NDk2OTMyODA0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)b64";

constexpr char kDesignPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAABr3RFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDE5MAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwYjExYzAyMDUwMDA2NTM2ODYxNzA2NTczMWMwMjUwMDAwODQ0Njk2MzY1NDI2NTYxNzIKMWMwMjZlMDAwODQ0Njk2MzY1NDI2NTYxNzIxYzAyNzQwMDgwOTM1MzY4NjE3MDY1NzM5NDIwMjg2ODc0NzQ3MDczM2EyZjJmCjc3Nzc3NzJlNjQ2OTYzNjU2MjY1NjE3MjJlNjM2ZjZkMjkyMDYyNzkyMDkzNDQ2OTYzNjU0MjY1NjE3Mjk0MmMyMDZjNjk2Mwo2NTZlNzM2NTY0MjA3NTZlNjQ2NTcyMjA5MzQzNDMzMDIwMzEyZTMwOTQyMDI4Njg3NDc0NzA3MzNhMmYyZjYzNzI2NTYxNzQKNjk3NjY1NjM2ZjZkNmQ2ZjZlNzMyZTZmNzI2NzJmNzA3NTYyNmM2OTYzNjQ2ZjZkNjE2OTZlMmY3YTY1NzI2ZjJmMzEyZTMwCjJmMjkxYzAyMDAwMDAyMDAwNDAwCipgNyAAAAWcaVRYdFhNTDpjb20uYWRvYmUueG1wAAAAAAA8P3hwYWNrZXQgYmVnaW49J++7vycgaWQ9J1c1TTBNcENlaGlIenJlU3pOVGN6a2M5ZCc/Pgo8eDp4bXBtZXRhIHhtbG5zOng9J2Fkb2JlOm5zOm1ldGEvJyB4OnhtcHRrPSdJbWFnZTo6RXhpZlRvb2wgMTMuNTknPgo8cmRmOlJERiB4bWxuczpyZGY9J2h0dHA6Ly93d3cudzMub3JnLzE5OTkvMDIvMjItcmRmLXN5bnRheC1ucyMnPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6ZGM9J2h0dHA6Ly9wdXJsLm9yZy9kYy9lbGVtZW50cy8xLjEvJz4KICA8ZGM6Y3JlYXRvcj4KICAgPHJkZjpTZXE+CiAgICA8cmRmOmxpPkRpY2VCZWFyPC9yZGY6bGk+CiAgIDwvcmRmOlNlcT4KICA8L2RjOmNyZWF0b3I+CiAgPGRjOnJpZ2h0cz4KICAgPHJkZjpBbHQ+CiAgICA8cmRmOmxpIHhtbDpsYW5nPSd4LWRlZmF1bHQnPuKAnFNoYXBlc+KAnSAoaHR0cHM6Ly93d3cuZGljZWJlYXIuY29tKSBieSDigJxEaWNlQmVhcuKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5TaGFwZXM8L3JkZjpsaT4KICAgPC9yZGY6QWx0PgogIDwvZGM6dGl0bGU+CiA8L3JkZjpEZXNjcmlwdGlvbj4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOnBob3Rvc2hvcD0naHR0cDovL25zLmFkb2JlLmNvbS9waG90b3Nob3AvMS4wLyc+CiAgPHBob3Rvc2hvcDpDcmVkaXQ+RGljZUJlYXI8L3Bob3Rvc2hvcDpDcmVkaXQ+CiA8L3JkZjpEZXNjcmlwdGlvbj4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOnBsdXM9J2h0dHA6Ly9ucy51c2VwbHVzLm9yZy9sZGYveG1wLzEuMC8nPgogIDxwbHVzOkxpY2Vuc29yPgogICA8cmRmOlNlcT4KICAgIDxyZGY6bGkgcmRmOnBhcnNlVHlwZT0nUmVzb3VyY2UnPgogICAgIDxwbHVzOkxpY2Vuc29yVVJMPmh0dHBzOi8vd3d3LmRpY2ViZWFyLmNvbTwvcGx1czpMaWNlbnNvclVSTD4KICAgIDwvcmRmOmxpPgogICA8L3JkZjpTZXE+CiAgPC9wbHVzOkxpY2Vuc29yPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczp4bXBSaWdodHM9J2h0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9yaWdodHMvJz4KICA8eG1wUmlnaHRzOldlYlN0YXRlbWVudD5odHRwczovL2NyZWF0aXZlY29t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)b64";

constexpr char kCommunityPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAABr3RFWHRSYXcgcHJvZmlsZSB0eXBlIGlwdGMACklQVEMgcHJvZmlsZQogICAgIDE5MAozODQyNDk0ZDA0MDQwMDAwMDAwMDAwYjExYzAyMDUwMDA2NTM2ODYxNzA2NTczMWMwMjUwMDAwODQ0Njk2MzY1NDI2NTYxNzIKMWMwMjZlMDAwODQ0Njk2MzY1NDI2NTYxNzIxYzAyNzQwMDgwOTM1MzY4NjE3MDY1NzM5NDIwMjg2ODc0NzQ3MDczM2EyZjJmCjc3Nzc3NzJlNjQ2OTYzNjU2MjY1NjE3MjJlNjM2ZjZkMjkyMDYyNzkyMDkzNDQ2OTYzNjU0MjY1NjE3Mjk0MmMyMDZjNjk2Mwo2NTZlNzM2NTY0MjA3NTZlNjQ2NTcyMjA5MzQzNDMzMDIwMzEyZTMwOTQyMDI4Njg3NDc0NzA3MzNhMmYyZjYzNzI2NTYxNzQKNjk3NjY1NjM2ZjZkNmQ2ZjZlNzMyZTZmNzI2NzJmNzA3NTYyNmM2OTYzNjQ2ZjZkNjE2OTZlMmY3YTY1NzI2ZjJmMzEyZTMwCjJmMjkxYzAyMDAwMDAyMDAwNDAwCipgNyAAAAWcaVRYdFhNTDpjb20uYWRvYmUueG1wAAAAAAA8P3hwYWNrZXQgYmVnaW49J++7vycgaWQ9J1c1TTBNcENlaGlIenJlU3pOVGN6a2M5ZCc/Pgo8eDp4bXBtZXRhIHhtbG5zOng9J2Fkb2JlOm5zOm1ldGEvJyB4OnhtcHRrPSdJbWFnZTo6RXhpZlRvb2wgMTMuNTknPgo8cmRmOlJERiB4bWxuczpyZGY9J2h0dHA6Ly93d3cudzMub3JnLzE5OTkvMDIvMjItcmRmLXN5bnRheC1ucyMnPgoKIDxyZGY6RGVzY3JpcHRpb24gcmRmOmFib3V0PScnCiAgeG1sbnM6ZGM9J2h0dHA6Ly9wdXJsLm9yZy9kYy9lbGVtZW50cy8xLjEvJz4KICA8ZGM6Y3JlYXRvcj4KICAgPHJkZjpTZXE+CiAgICA8cmRmOmxpPkRpY2VCZWFyPC9yZGY6bGk+CiAgIDwvcmRmOlNlcT4KICA8L2RjOmNyZWF0b3I+CiAgPGRjOnJpZ2h0cz4KICAgPHJkZjpBbHQ+CiAgICA8cmRmOmxpIHhtbDpsYW5nPSd4LWRlZmF1bHQnPuKAnFNoYXBlc+KAnSAoaHR0cHM6Ly93d3cuZGljZWJlYXIuY29tKSBieSDigJxEaWNlQmVhcuKAnSwgbGljZW5zZWQgdW5kZXIg4oCcQ0MwIDEuMOKAnSAoaHR0cHM6Ly9jcmVhdGl2ZWNvbW1vbnMub3JnL3B1YmxpY2RvbWFpbi96ZXJvLzEuMC8pPC9yZGY6bGk+CiAgIDwvcmRmOkFsdD4KICA8L2RjOnJpZ2h0cz4KICA8ZGM6dGl0bGU+CiAgIDxyZGY6QWx0PgogICAgPHJkZjpsaSB4bWw6bGFuZz0neC1kZWZhdWx0Jz5TaGFwZXM8L3JkZjpsaT4KICAgPC9yZGY6QWx0PgogIDwvZGM6dGl0bGU+CiA8L3JkZjpEZXNjcmlwdGlvbj4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOnBob3Rvc2hvcD0naHR0cDovL25zLmFkb2JlLmNvbS9waG90b3Nob3AvMS4wLyc+CiAgPHBob3Rvc2hvcDpDcmVkaXQ+RGljZUJlYXI8L3Bob3Rvc2hvcDpDcmVkaXQ+CiA8L3JkZjpEZXNjcmlwdGlvbj4KCiA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0nJwogIHhtbG5zOnBsdXM9J2h0dHA6Ly9ucy51c2VwbHVzLm9yZy9sZGYveG1wLzEuMC8nPgogIDxwbHVzOkxpY2Vuc29yPgogICA8cmRmOlNlcT4KICAgIDxyZGY6bGkgcmRmOnBhcnNlVHlwZT0nUmVzb3VyY2UnPgogICAgIDxwbHVzOkxpY2Vuc29yVVJMPmh0dHBzOi8vd3d3LmRpY2ViZWFyLmNvbTwvcGx1czpMaWNlbnNvclVSTD4KICAgIDwvcmRmOmxpPgogICA8L3JkZjpTZXE+CiAgPC9wbHVzOkxpY2Vuc29yPgogPC9yZGY6RGVzY3JpcHRpb24+CgogPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9JycKICB4bWxuczp4bXBSaWdodHM9J2h0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9yaWdodHMvJz4KICA8eG1wUmlnaHRzOldlYlN0YXRlbWVudD5odHRwczovL2NyZWF0aXZlY29t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)b64";

constexpr char kTesseractSvg[] =
    R"b64(PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA1MTIgNTEyIj4NCiAgPGRlZnM+DQogICAgPGxpbmVhckdyYWRpZW50IGlkPSJiZyIgeDE9IjAiIHkxPSIwIiB4Mj0iNTEyIiB5Mj0iNTEyIiBncmFkaWVudFVuaXRzPSJ1c2VyU3BhY2VPblVzZSI+DQogICAgICA8c3RvcCBvZmZzZXQ9IjAlIiBzdG9wLWNvbG9yPSIjMWUxYjRiIi8+DQogICAgICA8c3RvcCBvZmZzZXQ9IjEwMCUiIHN0b3AtY29sb3I9IiMzMTJlODEiLz4NCiAgICA8L2xpbmVhckdyYWRpZW50Pg0KICAgIDxsaW5lYXJHcmFkaWVudCBpZD0id2lyZSIgeDE9IjAiIHkxPSIwIiB4Mj0iNTEyIiB5Mj0iNTEyIiBncmFkaWVudFVuaXRzPSJ1c2VyU3BhY2VPblVzZSI+DQogICAgICA8c3RvcCBvZmZzZXQ9IjAlIiBzdG9wLWNvbG9yPSIjODE4Y2Y4Ii8+DQogICAgICA8c3RvcCBvZmZzZXQ9IjEwMCUiIHN0b3AtY29sb3I9IiNhNzhiZmEiLz4NCiAgICA8L2xpbmVhckdyYWRpZW50Pg0KICA8L2RlZnM+DQoNCiAgPCEtLSBSb3VuZGVkIHNxdWFyZSBiYWNrZ3JvdW5kIC0tPg0KICA8cmVjdCB3aWR0aD0iNTEyIiBoZWlnaHQ9IjUxMiIgcng9Ijk2IiBmaWxsPSJ1cmwoI2JnKSIvPg0KDQogIDwhLS0gT3V0ZXIgY3ViZSB3aXJlZnJhbWUgLS0+DQogIDxyZWN0IHg9IjY0IiB5PSI2NCIgd2lkdGg9IjM4NCIgaGVpZ2h0PSIzODQiDQogICAgICAgIGZpbGw9Im5vbmUiIHN0cm9rZT0idXJsKCN3aXJlKSIgc3Ryb2tlLXdpZHRoPSIyOCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIvPg0KDQogIDwhLS0gSW5uZXIgY3ViZSB3aXJlZnJhbWUgLS0+DQogIDxyZWN0IHg9IjE2MyIgeT0iMTYzIiB3aWR0aD0iMTg2IiBoZWlnaHQ9IjE4NiINCiAgICAgICAgZmlsbD0ibm9uZSIgc3Ryb2tlPSJ1cmwoI3dpcmUpIiBzdHJva2Utd2lkdGg9IjI4IiBzdHJva2UtbGluZWpvaW49InJvdW5kIi8+DQoNCiAgPCEtLSBDb3JuZXIgY29ubmVjdG9ycyAob3V0ZXIg4oaSIGlubmVyKSAtLT4NCiAgPGxpbmUgeDE9IjY0IiAgeTE9IjY0IiAgeDI9IjE2MyIgeTI9IjE2MyIgc3Ryb2tlPSIjYzRiNWZkIiBzdHJva2Utd2lkdGg9IjIwIiBzdHJva2UtbGluZWNhcD0icm91bmQiLz4NCiAgPGxpbmUgeDE9IjQ0OCIgeTE9IjY0IiAgeDI9IjM0OSIgeTI9IjE2MyIgc3Ryb2tlPSIjYzRiNWZkIiBzdHJva2Utd2lkdGg9IjIwIiBzdHJva2UtbGluZWNhcD0icm91bmQiLz4NCiAgPGxpbmUgeDE9IjQ0OCIgeTE9IjQ0OCIgeDI9IjM0OSIgeTI9IjM0OSIgc3Ryb2tlPSIjYzRiNWZkIiBzdHJva2Utd2lkdGg9IjIwIiBzdHJva2UtbGluZWNhcD0icm91bmQiLz4NCiAgPGxpbmUgeDE9IjY0IiAgeTE9IjQ0OCIgeDI9IjE2MyIgeTI9IjM0OSIgc3Ryb2tlPSIjYzRiNWZkIiBzdHJva2Utd2lkdGg9IjIwIiBzdHJva2UtbGluZWNhcD0icm91bmQiLz4NCjwvc3ZnPg0K)b64";

constexpr char kReleaseBannerPng[] =
    R"b64(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)b64";

constexpr char kStickerChipPng[] =
    R"b64(iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAIAAABt+uBvAAACHklEQVR42u2czVHDMBBG4x3fKYAGKCZ3ZiiQClICRdAABaQCOORAxsbGklafVsrbWxLL+vS0u9ZPrOn74/WEbZuBAEAAAhCAAAQgAAEIAxCAAAQgAAEIQADCfm1uruB6+dq/4On8/HCA/oWydbEe1hwZzVZxJaa5Cy5bdxOQsu7oaO4s8iBBA2oHnXVNR1CXDUCnao02Bp169dowdCrVbiPRqaHBBqPjrsTGo+Orx4ak46iK5Y76gGK6j5c2G5iOi0JCrOlkdW3n98/7j5e3l3ql2uegVO9dtPPPb7xKeUWZtfKdg63NKxXCg5K6pQaFJEbZTkSSBhCAIgKKPz700izyoP2Ry9aveaV6DTFfCrKxojQHrVt1pJ15pbxsynsdqsccdMraPuMpBiAAAQhAAHo4QG3/N6jUjAdFAuS1DChbTjzJFu3dm3S7oWDOYYKQvqdTTirvbtlJUxFii34uYaTf/ykCdLxbXBhl0yl55jZ7FSEpiSiz8sKmwtNfXDZ/djBlFPH1oKn8eJzy/dWSjFZ7TOuQpFMVlGRWMZ1mOejWziRXUi6zOodYXqAdDLoSLi4TxsnxiK5QC9Ve02kLqCmUEgurLIgGC66vee3WhcqG9VpHWpvUaN0pFtc1C3RXffzX7gbrunsFTjqLQ8DFmwZ8b94r6MY/eWHdTs7u6GaC0ixJd20AAhCAAAQgAAEIQADCAAQgAAEIQAAazH4ATo7yF6Fh814AAAAASUVORK5CYII=)b64";

std::vector<std::uint8_t> decode_base64(std::string_view encoded)
{
    if (encoded.size() % 4 != 0)
    {
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = -8;
    bool saw_padding = false;
    for (const unsigned char c : encoded)
    {
        int value = -1;
        if (c >= 'A' && c <= 'Z')
        {
            value = c - 'A';
        }
        else if (c >= 'a' && c <= 'z')
        {
            value = c - 'a' + 26;
        }
        else if (c >= '0' && c <= '9')
        {
            value = c - '0' + 52;
        }
        else if (c == '+')
        {
            value = 62;
        }
        else if (c == '/')
        {
            value = 63;
        }
        else if (c == '=')
        {
            saw_padding = true;
            continue;
        }
        if (value < 0 || saw_padding)
        {
            return {};
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffu));
            bits -= 8;
        }
    }
    return out;
}

struct Asset
{
    const char* key;
    std::string_view base64;
    std::size_t byte_count;
    bool svg = false;
};

constexpr std::array kAssets{
    Asset{kAlexAvatar, kAlexPng, 5289},
    Asset{kMayaAvatar, kMayaPng, 5395},
    Asset{kSamAvatar, kSamPng, 5694},
    Asset{kRobinAvatar, kRobinPng, 5748},
    Asset{kDesignRoomAvatar, kDesignPng, 4048},
    Asset{kCommunityRoomAvatar, kCommunityPng, 3424},
    Asset{kTesseractRoomAvatar, kTesseractSvg, 1431, true},
    Asset{kReleaseBanner, kReleaseBannerPng, 3825},
    Asset{kStickerChip, kStickerChipPng, 599},
};

} // namespace

bool install_assets(tk::CanvasFactory& factory, tk::PixmapCache& cache)
{
    for (const auto& asset : kAssets)
    {
        auto bytes = decode_base64(asset.base64);
        if (bytes.size() != asset.byte_count)
        {
            return false;
        }
        auto image = asset.svg
                         ? tk::rasterize_svg(factory, std::span<const std::uint8_t>{bytes}, 192)
                         : factory.decode_image(std::span<const std::uint8_t>{bytes});
        if (!image)
        {
            return false;
        }
        cache.store(tk::CacheKey::media(asset.key), std::move(image));
    }
    return true;
}

std::vector<std::string> installed_asset_keys()
{
    std::vector<std::string> out;
    for (const auto& asset : kAssets)
    {
        out.emplace_back(asset.key);
    }
    return out;
}

} // namespace tesseract::screenshot
