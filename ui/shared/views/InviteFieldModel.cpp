#include "InviteFieldModel.h"

namespace tesseract::views
{

namespace
{

constexpr std::string_view kPlaceholder = "\xEF\xBF\xBC"; // U+FFFC

bool is_separator(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
           c == ';';
}

} // namespace

bool is_complete_mxid(std::string_view s)
{
    if (s.size() < 4 || s.front() != '@')
        return false;
    for (char c : s)
        if (is_separator(c))
            return false;
    const auto colon = s.find(':');
    return colon != std::string_view::npos && colon > 1 && colon + 1 < s.size();
}

InviteFieldParse parse_invite_field(std::string_view text,
                                    const std::vector<tesseract::MentionSeg>& segs,
                                    int cursor)
{
    InviteFieldParse out;
    const int size = static_cast<int>(text.size());
    if (cursor < 0 || cursor > size)
        cursor = size;

    // Mention segments in order — matched 1:1 to placeholders.
    std::vector<const tesseract::MentionSeg*> mentions;
    for (const auto& s : segs)
        if (s.kind != tesseract::MentionSeg::Kind::Text)
            mentions.push_back(&s);
    std::size_t next_mention = 0;

    std::vector<InviteFieldToken> tokens;
    int tok_start = -1;
    auto flush = [&](int end)
    {
        if (tok_start >= 0 && end > tok_start)
            tokens.push_back({tok_start, end,
                              std::string(text.substr(tok_start, end - tok_start))});
        tok_start = -1;
    };

    int i = 0;
    while (i < size)
    {
        if (text.substr(i, kPlaceholder.size()) == kPlaceholder)
        {
            flush(i);
            if (next_mention < mentions.size())
            {
                const auto* m = mentions[next_mention++];
                if (m->kind == tesseract::MentionSeg::Kind::Mention && !m->is_room)
                    out.pills.push_back({i, m->user_id, m->display_name});
            }
            i += static_cast<int>(kPlaceholder.size());
            continue;
        }
        if (is_separator(text[i]))
            flush(i);
        else if (tok_start < 0)
            tok_start = i;
        ++i;
    }
    flush(size);

    for (auto& t : tokens)
    {
        // The token the caret sits at the end of (or inside) is the query.
        if (cursor >= t.start && cursor <= t.end)
        {
            out.query = std::move(t);
            continue;
        }
        if (is_complete_mxid(t.text))
            out.committed.push_back(std::move(t));
    }
    if (out.query.text.empty())
        out.query.start = out.query.end = cursor;
    return out;
}

} // namespace tesseract::views
