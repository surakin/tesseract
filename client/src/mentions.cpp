#include "tesseract/mentions.h"
#include "html_util.h"

#include <cstddef>
#include <string_view>

namespace tesseract
{
namespace
{

// Private-use delimiters (U+E000 … U+E001) survive both markdown conversion
// and HTML escaping unchanged, so a mention's placeholder can be located and
// swapped for its anchor after `markdown_to_html` runs.
std::string sentinel(std::size_t idx)
{
    std::string s = "\xEE\x80\x80"; // U+E000
    s += std::to_string(idx);
    s += "\xEE\x80\x81"; // U+E001
    return s;
}

// Minimal HTML for the fallback path (a message with mentions but no markdown,
// where `markdown_to_html` returns an empty formatted_body).
std::string plain_to_html(std::string_view src)
{
    std::string out;
    out.reserve(src.size() + 8);
    for (char c : src)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\n':
            out += "<br/>";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string anchor_for(const MentionSeg& m)
{
    std::string out = "<a href=\"https://matrix.to/#/";
    if (m.is_room)
    {
        out += "@room\">@room</a>";
        return out;
    }
    html_escape_to(m.user_id, out);
    out += "\">";
    html_escape_to(m.display_name, out);
    out += "</a>";
    return out;
}

// MSC2545 inline custom emoticon. `alt`/`title` both carry the plain-text
// fallback (":shortcode:") per convention; `height` matches Element's
// emitted markup.
std::string img_for(const MentionSeg& m)
{
    std::string out = "<img data-mx-emoticon src=\"";
    html_escape_to(m.mxc_url, out);
    out += "\" alt=\":";
    html_escape_to(m.shortcode, out);
    out += ":\" title=\":";
    html_escape_to(m.shortcode, out);
    out += ":\" height=\"32\"/>";
    return out;
}

} // namespace

MarkdownResult build_mention_message(const std::vector<MentionSeg>& segments)
{
    bool has_sentinel_seg = false;
    for (const auto& s : segments)
    {
        if (s.kind != MentionSeg::Kind::Text)
        {
            has_sentinel_seg = true;
            break;
        }
    }

    std::string body;
    for (const auto& s : segments)
    {
        if (s.kind == MentionSeg::Kind::Text)
        {
            body += s.text;
        }
        else if (s.kind == MentionSeg::Kind::Mention)
        {
            body += s.is_room ? "@room" : s.display_name;
        }
        else
        {
            body += ":" + s.shortcode + ":";
        }
    }

    if (!has_sentinel_seg)
    {
        return markdown_to_html(body);
    }

    std::string md;
    std::vector<std::string> anchors;
    for (const auto& s : segments)
    {
        if (s.kind == MentionSeg::Kind::Text)
        {
            md += s.text;
        }
        else
        {
            md += sentinel(anchors.size());
            anchors.push_back(s.kind == MentionSeg::Kind::Mention
                                  ? anchor_for(s)
                                  : img_for(s));
        }
    }

    MarkdownResult res = markdown_to_html(md);
    std::string html =
        res.formatted_body.empty() ? plain_to_html(md) : res.formatted_body;

    for (std::size_t i = 0; i < anchors.size(); ++i)
    {
        std::string tok = sentinel(i);
        std::size_t pos = html.find(tok);
        if (pos != std::string::npos)
        {
            html.replace(pos, tok.size(), anchors[i]);
        }
    }

    return {body, html};
}

} // namespace tesseract
