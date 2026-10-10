#include "media_viewer_items.h"

#include "tk/i18n.h"

#include <algorithm>
#include <cctype>

namespace tesseract::views
{

namespace
{
// File extension (with the dot) for a mime type, for naming a save when the
// item has no file name: ".ogg" for "audio/ogg", ".mp3" for "audio/mpeg",
// ".mov" for "video/quicktime", ".svg" for "image/svg+xml", ".mp4" for
// "video/mp4; codecs=avc1". Parameters, "+suffix", and "x-" / "vnd." prefixes
// are stripped; empty when the mime type has no usable subtype.
std::string mime_extension(const std::string& mime)
{
    const auto slash = mime.find('/');
    if (slash == std::string::npos)
    {
        return {};
    }
    std::string type = mime.substr(0, slash);
    std::string sub = mime.substr(slash + 1);
    const auto semi = sub.find(';');
    if (semi != std::string::npos)
    {
        sub.resize(semi);
    }
    const auto plus = sub.find('+');
    if (plus != std::string::npos)
    {
        sub.resize(plus);
    }
    const auto lower = [](std::string& v)
    {
        for (char& c : v)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    };
    const auto trim = [](std::string& v)
    {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
        {
            v.pop_back();
        }
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
        {
            v.erase(v.begin());
        }
    };
    lower(type);
    lower(sub);
    trim(type);
    trim(sub);
    for (const char* prefix : {"x-", "vnd."})
    {
        if (sub.rfind(prefix, 0) == 0)
        {
            sub.erase(0, std::string(prefix).size());
        }
    }
    struct Mapping
    {
        const char* sub;
        const char* ext;
    };
    // Subtypes whose extension differs from the subtype itself.
    static constexpr Mapping kMap[] = {
        {"jpeg", "jpg"},       {"pjpeg", "jpg"},    {"quicktime", "mov"},
        {"matroska", "mkv"},   {"msvideo", "avi"},  {"wave", "wav"},
        {"wav", "wav"},        {"3gpp", "3gp"},     {"3gpp2", "3g2"},
        {"microsoft.icon", "ico"}, {"icon", "ico"}, {"tiff", "tif"},
        {"mp4-latm", "m4a"},   {"m4a", "m4a"},      {"aac", "aac"},
        {"mpeg4-generic", "m4a"}, {"ms-wmv", "wmv"}, {"plain", "txt"},
    };
    for (const auto& m : kMap)
    {
        if (sub == m.sub)
        {
            return std::string(".") + m.ext;
        }
    }
    if (sub == "mpeg")
    {
        // MP3 for audio, MPEG-1/2 program streams otherwise.
        return type == "audio" ? ".mp3" : ".mpg";
    }
    if (sub == "mp4" && type == "audio")
    {
        return ".m4a";
    }
    // Only a plain alphanumeric token is safe to put in a suggested file name.
    const bool plain =
        !sub.empty() && sub.size() <= 8 &&
        std::all_of(sub.begin(), sub.end(), [](char c)
                    { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    return plain ? "." + sub : std::string{};
}
} // namespace

MediaViewerItem item_from_image_hit(const MessageListView::ImageHit& hit)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Image;
    item.event_id = hit.event_id;
    item.source = hit.source ? hit.source->fetch_token() : std::string{};
    item.thumbnail = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
    item.caption = hit.body;
    item.filename = hit.body;
    item.width = hit.natural_w;
    item.height = hit.natural_h;
    return item;
}

MediaViewerItem item_from_video_hit(const MessageListView::VideoHit& hit)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Video;
    item.event_id = hit.event_id;
    item.source = hit.source ? hit.source->fetch_token() : std::string{};
    item.thumbnail = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
    item.mime_type = hit.mime_type;
    item.width = hit.natural_w;
    item.height = hit.natural_h;
    item.duration_ms = hit.duration_ms;
    item.loop = hit.loop;
    item.no_audio = hit.no_audio;
    item.hide_controls = hit.hide_controls;
    return item;
}

MediaViewerItem item_from_avatar(const std::string& url, const std::string& name)
{
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Image;
    item.source = url;
    item.thumbnail = url;
    item.caption = name;
    item.filename = name;
    return item;
}

std::vector<MediaViewerItem> items_from_gallery(const MessageListView::GalleryHit& hit)
{
    using Row = MessageRowData::GalleryItemRow;
    std::vector<MediaViewerItem> out;
    out.reserve(hit.items.size());
    for (const Row& row : hit.items)
    {
        MediaViewerItem item;
        switch (row.kind)
        {
            case Row::Kind::Image:
            {
                item.kind = MediaViewerItem::Kind::Image;
                break;
            }
            case Row::Kind::Video:
            {
                item.kind = MediaViewerItem::Kind::Video;
                break;
            }
            case Row::Kind::Audio:
            {
                item.kind = MediaViewerItem::Kind::Audio;
                break;
            }
            case Row::Kind::File:
            {
                item.kind = MediaViewerItem::Kind::File;
                break;
            }
        }
        item.event_id = hit.event_id;
        item.source = row.source ? row.source->fetch_token() : std::string{};
        item.thumbnail = row.thumbnail ? row.thumbnail->fetch_token() : std::string{};
        item.caption =
            (row.body.empty() || row.body == row.filename) ? hit.caption : row.body;
        item.filename = row.filename;
        item.mime_type = row.mime_type;
        item.width = row.media_w;
        item.height = row.media_h;
        item.duration_ms = row.duration_ms;
        item.file_size = row.file_size;
        item.waveform = row.waveform;
        out.push_back(std::move(item));
    }
    return out;
}

MediaSaveSpec media_save_spec(const MediaViewerItem& item)
{
    MediaSaveSpec spec;
    switch (item.kind)
    {
        case MediaViewerItem::Kind::Image:
        {
            spec.title = tk::tr("Save image");
            spec.suggested_name =
                item.filename.empty() ? "image" + mime_extension(item.mime_type) : item.filename;
            spec.filter_name = tk::tr("Images");
            spec.patterns = {"*.jpg", "*.jpeg", "*.png", "*.gif", "*.webp"};
            break;
        }
        case MediaViewerItem::Kind::Video:
        {
            spec.title = tk::tr("Save video");
            if (!item.filename.empty())
            {
                spec.suggested_name = item.filename;
            }
            else
            {
                std::string ext = mime_extension(item.mime_type);
                spec.suggested_name = "video" + (ext.empty() ? ".mp4" : ext);
            }
            spec.filter_name = tk::tr("Videos");
            spec.patterns = {"*.mp4", "*.webm", "*.mkv"};
            break;
        }
        case MediaViewerItem::Kind::Audio:
        {
            spec.title = tk::tr("Save audio");
            if (!item.filename.empty())
            {
                spec.suggested_name = item.filename;
            }
            else if (!item.caption.empty())
            {
                spec.suggested_name = item.caption;
            }
            else
            {
                spec.suggested_name = "audio" + mime_extension(item.mime_type);
            }
            break;
        }
        case MediaViewerItem::Kind::File:
        {
            spec.title = tk::tr("Save file");
            if (!item.filename.empty())
            {
                spec.suggested_name = item.filename;
            }
            else if (!item.caption.empty())
            {
                spec.suggested_name = item.caption;
            }
            else
            {
                spec.suggested_name = "download";
            }
            break;
        }
    }
    return spec;
}

} // namespace tesseract::views
