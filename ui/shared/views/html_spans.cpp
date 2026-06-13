#include "html_spans.h"

#include "tesseract/highlight.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace tesseract::views
{

namespace
{

// ── UTF-8 encoding ────────────────────────────────────────────────────────

// Map a numeric character reference to a safe Unicode scalar value, following
// the HTML spec's "numeric character reference end state" sanitisation: reject
// non-scalar values so we never emit invalid UTF-8 that the text backends
// (Pango / DirectWrite / CoreText) could mishandle.
//   - NUL, surrogates (U+D800–U+DFFF) and > U+10FFFF → U+FFFD.
//   - C0 controls are disallowed except TAB/LF/CR (the spec also permits FF,
//     but mapping it to U+FFFD is harmless here) → U+FFFD.
// Everything else passes through unchanged.
uint32_t sanitize_codepoint(uint32_t cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    {
        return 0xFFFD;
    }
    if (cp < 0x20 && cp != 0x09 && cp != 0x0A && cp != 0x0D)
    {
        return 0xFFFD;
    }
    return cp;
}

void append_codepoint(std::string& out, uint32_t cp)
{
    cp = sanitize_codepoint(cp);
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0x10FFFF)
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ── Entity decoding ───────────────────────────────────────────────────────

// Decode one HTML entity starting at `p` (pointing at '&').
// Appends decoded text to `out` and advances `p` past the entity.
void decode_entity(const char*& p, const char* end, std::string& out)
{
    const char* start = p; // points at '&'
    ++p;                   // skip '&'
    if (p >= end)
    {
        out += '&';
        return;
    }

    if (*p == '#')
    {
        ++p;
        if (p >= end)
        {
            out += '&';
            p = start + 1;
            return;
        }
        uint32_t cp = 0;
        if (*p == 'x' || *p == 'X')
        {
            ++p;
            const char* hex_start = p;
            while (p < end && std::isxdigit(static_cast<unsigned char>(*p)))
            {
                if (cp <= 0x10FFFF)
                {
                    cp = cp * 16u;
                    if (std::isdigit(static_cast<unsigned char>(*p)))
                    {
                        cp += static_cast<uint32_t>(*p - '0');
                    }
                    else
                    {
                        cp += static_cast<uint32_t>(
                            std::tolower(static_cast<unsigned char>(*p)) - 'a' +
                            10);
                    }
                }
                // else: keep consuming digits but stop accumulating so the
                // uint32_t can't silently wrap into a small valid codepoint.
                ++p;
            }
            if (p == hex_start)
            {
                out += '&';
                p = start + 1;
                return;
            }
        }
        else
        {
            const char* dec_start = p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
            {
                if (cp <= 0x10FFFF)
                {
                    cp = cp * 10u + static_cast<uint32_t>(*p - '0');
                }
                ++p;
            }
            if (p == dec_start)
            {
                out += '&';
                p = start + 1;
                return;
            }
        }
        if (p < end && *p == ';')
        {
            ++p;
        }
        append_codepoint(out, cp);
        return;
    }

    // Named entity — scan up to ';'
    const char* name_start = p;
    while (p < end && *p != ';' && *p != '<' && *p != '&' &&
           !std::isspace(static_cast<unsigned char>(*p)))
    {
        ++p;
    }
    std::string_view ent(name_start, static_cast<std::size_t>(p - name_start));
    const bool had_semicolon = (p < end && *p == ';');
    if (had_semicolon)
    {
        ++p;
    }

    if (ent == "amp")
    {
        out += '&';
    }
    else if (ent == "lt")
    {
        out += '<';
    }
    else if (ent == "gt")
    {
        out += '>';
    }
    else if (ent == "quot")
    {
        out += '"';
    }
    else if (ent == "apos")
    {
        out += '\'';
    }
    else if (ent == "nbsp")
    {
        out += '\xc2';
        out += '\xa0';
    } // U+00A0
    // Unknown entity: emit the literal source back, preserving the original
    // terminator state. A bare '&' or '&word' with no ';' must NOT gain one
    // (e.g. "AT&T" stays "AT&T", "Tom & Jerry" stays unchanged).
    else
    {
        out += '&';
        out += std::string(ent);
        if (had_semicolon)
        {
            out += ';';
        }
    }
}

// ── Tag scanning ──────────────────────────────────────────────────────────

struct Tag
{
    std::string name;           // lower-cased tag name
    std::string href;           // non-empty only for <a href="http(s)://...">
    std::string spoiler_reason; // value of data-mx-spoiler (may be empty)
    std::string code_lang; // language token from class="language-X" on pre/code
    bool has_spoiler = false;   // true when data-mx-spoiler attribute present
    bool closing = false;
    bool self_closing = false;
};

// Extract the language token from a class attribute value, e.g.
// "language-rust" or "lang-py hljs" → "rust"/"py". The class may carry several
// space-separated tokens. The result is lower-cased and restricted to a safe
// charset so it is never interpreted as markup downstream.
static std::string lang_from_class(std::string_view cls)
{
    std::size_t i = 0;
    while (i < cls.size())
    {
        while (i < cls.size() && std::isspace(static_cast<unsigned char>(cls[i])))
        {
            ++i;
        }
        std::size_t j = i;
        while (j < cls.size() &&
               !std::isspace(static_cast<unsigned char>(cls[j])))
        {
            ++j;
        }
        std::string_view tok = cls.substr(i, j - i);
        std::string_view prefix;
        if (tok.rfind("language-", 0) == 0)
        {
            prefix = "language-";
        }
        else if (tok.rfind("lang-", 0) == 0)
        {
            prefix = "lang-";
        }
        if (!prefix.empty())
        {
            std::string out;
            for (char c : tok.substr(prefix.size()))
            {
                unsigned char uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc) || c == '+' || c == '#' || c == '.' ||
                    c == '-' || c == '_')
                {
                    out += static_cast<char>(std::tolower(uc));
                }
            }
            return out;
        }
        i = j;
    }
    return {};
}

// Scan [p, end) for the attribute named `target` (case-insensitive).
// Returns the attribute value when found (empty string for boolean attributes
// with no value), or std::nullopt when the attribute is absent entirely.
static std::optional<std::string> extract_attr(const char* p, const char* end,
                                               const char* target)
{
    const std::size_t tlen = std::strlen(target);
    while (p < end)
    {
        // Skip whitespace between attributes.
        while (p < end && std::isspace(static_cast<unsigned char>(*p)))
        {
            ++p;
        }
        if (p >= end || *p == '>' || *p == '/')
        {
            break;
        }

        // Read attribute name.
        const char* name_start = p;
        while (p < end && *p != '=' && *p != '>' && *p != '/' &&
               !std::isspace(static_cast<unsigned char>(*p)))
        {
            ++p;
        }
        const std::size_t name_len = static_cast<std::size_t>(p - name_start);

        // Skip optional whitespace + '='.
        while (p < end && std::isspace(static_cast<unsigned char>(*p)))
        {
            ++p;
        }
        bool has_value = (p < end && *p == '=');
        if (has_value)
        {
            ++p; // skip '='
            while (p < end && std::isspace(static_cast<unsigned char>(*p)))
            {
                ++p;
            }
        }

        // Read (quoted) value.
        std::string value;
        if (has_value && p < end && (*p == '"' || *p == '\''))
        {
            char qch = *p++;
            while (p < end && *p != qch)
            {
                value += *p++;
            }
            if (p < end)
            {
                ++p; // closing quote
            }
        }
        else if (has_value)
        {
            // Unquoted value.
            while (p < end && !std::isspace(static_cast<unsigned char>(*p)) &&
                   *p != '>' && *p != '/')
            {
                value += *p++;
            }
        }

        // Case-insensitive name comparison.
        if (name_len == tlen)
        {
            bool match = true;
            for (std::size_t i = 0; i < tlen && match; ++i)
            {
                match =
                    (std::tolower(static_cast<unsigned char>(name_start[i])) ==
                     static_cast<unsigned char>(target[i]));
            }
            if (match)
            {
                return value;
            }
        }
    }
    return std::nullopt;
}

// Parse one tag starting at `p` (pointing at '<'). Advances `p` past '>'.
Tag parse_tag(const char*& p, const char* end)
{
    Tag t{};
    ++p; // skip '<'
    if (p < end && *p == '/')
    {
        t.closing = true;
        ++p;
    }
    if (p < end && *p == '!')
    {
        // An HTML comment ends at "-->", not the first '>': a '>' inside the
        // comment (e.g. within a quoted attribute) must not terminate it early
        // and leak the tail as visible text. DOCTYPE / other "<!…>"
        // declarations have no such body and end at the first '>'.
        if (end - p >= 3 && p[1] == '-' && p[2] == '-')
        {
            p += 3; // skip "!--"
            while (p < end)
            {
                if (end - p >= 3 && p[0] == '-' && p[1] == '-' && p[2] == '>')
                {
                    p += 3;
                    break;
                }
                ++p;
            }
        }
        else
        {
            while (p < end && *p != '>')
            {
                ++p;
            }
            if (p < end)
            {
                ++p;
            }
        }
        return t;
    }

    while (p < end &&
           (std::isalnum(static_cast<unsigned char>(*p)) || *p == '-'))
    {
        t.name +=
            static_cast<char>(std::tolower(static_cast<unsigned char>(*p++)));
    }

    // Record attribute start for href extraction on <a> tags.
    const char* attr_start = p;

    // Skip attributes, detect '/>' self-close.
    bool in_quote = false;
    char quote_ch = 0;
    while (p < end)
    {
        char c = *p++;
        if (in_quote)
        {
            if (c == quote_ch)
            {
                in_quote = false;
            }
        }
        else if (c == '"' || c == '\'')
        {
            in_quote = true;
            quote_ch = c;
        }
        else if (c == '/')
        {
            t.self_closing = true;
        }
        else if (c == '>')
        {
            break;
        }
    }

    const char* attr_end = p - 1; // points just before '>'

    // For opening <a> tags extract the href attribute.
    if (t.name == "a" && !t.closing)
    {
        auto href = extract_attr(attr_start, attr_end, "href");
        if (href &&
            (href->rfind("http://", 0) == 0 || href->rfind("https://", 0) == 0))
        {
            t.href = std::move(*href);
        }
    }

    // For opening <span> tags detect data-mx-spoiler (MSC2010).
    if (t.name == "span" && !t.closing)
    {
        auto v = extract_attr(attr_start, attr_end, "data-mx-spoiler");
        if (v.has_value())
        {
            t.has_spoiler = true;
            t.spoiler_reason = std::move(*v);
        }
    }

    // For opening <pre>/<code> tags read the highlight language from
    // class="language-X" (the Matrix convention is on the inner <code>).
    if ((t.name == "pre" || t.name == "code") && !t.closing)
    {
        auto cls = extract_attr(attr_start, attr_end, "class");
        if (cls.has_value())
        {
            t.code_lang = lang_from_class(*cls);
        }
    }

    return t;
}

// ── Formatting state ──────────────────────────────────────────────────────

struct FmtState
{
    std::string url;
    std::string spoiler_reason;
    bool spoiler = false;
    bool bold = false;
    bool semibold = false;
    bool italic = false;
    bool code = false;
    bool strikethrough = false;
    bool is_mention = false;
};

// A matrix.to *user* permalink — `https://matrix.to/#/@user:server` (or the
// `@room` sentinel). Room (`!`/`#`) and event (`$`) permalinks are excluded:
// only `@`-prefixed entities render as mention pills.
static bool is_matrix_to_user_link(const std::string& url)
{
    static const std::string kHttps = "https://matrix.to/#/";
    static const std::string kHttp = "http://matrix.to/#/";
    const std::string* pfx = nullptr;
    if (url.rfind(kHttps, 0) == 0)
    {
        pfx = &kHttps;
    }
    else if (url.rfind(kHttp, 0) == 0)
    {
        pfx = &kHttp;
    }
    if (pfx == nullptr)
    {
        return false;
    }
    const std::size_t n = pfx->size();
    if (url.size() > n && url[n] == '@')
    {
        return true;
    }
    // Tolerate a percent-encoded leading '@' (%40user:server).
    return url.size() >= n + 3 && url.compare(n, 3, "%40") == 0;
}

// Split plain (non-link, non-code) spans on a word-bounded literal "@room"
// token, flagging that token as a mention pill. `@room` mentions arrive as
// plain text (per spec — the SDK rewrites the sentinel anchor to text and sets
// m.mentions.room), so there is no link to key off; this is a heuristic.
static std::vector<tk::TextSpan>
split_room_mentions(std::vector<tk::TextSpan> spans, bool dark)
{
    auto is_word = [](unsigned char ch)
    { return std::isalnum(ch) != 0 || ch == '_'; };
    std::vector<tk::TextSpan> out;
    out.reserve(spans.size());
    for (auto& sp : spans)
    {
        if (sp.is_mention || sp.code || !sp.url.empty() ||
            sp.text.find("@room") == std::string::npos)
        {
            out.push_back(std::move(sp));
            continue;
        }
        const std::string t = sp.text;
        std::size_t i = 0;
        std::size_t emitted = 0;
        while (true)
        {
            std::size_t pos = t.find("@room", i);
            if (pos == std::string::npos)
            {
                break;
            }
            std::size_t after = pos + 5;
            bool left_ok = (pos == 0) || !is_word((unsigned char)t[pos - 1]);
            bool right_ok =
                (after >= t.size()) || !is_word((unsigned char)t[after]);
            if (left_ok && right_ok)
            {
                if (pos > emitted)
                {
                    tk::TextSpan pre = sp;
                    pre.text = t.substr(emitted, pos - emitted);
                    out.push_back(std::move(pre));
                }
                tk::TextSpan m = sp;
                m.text = "@room";
                m.url.clear();
                m.is_mention = true;
                m.has_color = true;
                m.color = dark ? tk::Color{0xA8, 0xC5, 0xFF, 0xFF}
                               : tk::Color{0x1B, 0x4A, 0xC2, 0xFF};
                m.has_background = true;
                m.background = dark ? tk::Color{0x2E, 0x3B, 0x5E, 0xFF}
                                    : tk::Color{0xDB, 0xE5, 0xFF, 0xFF};
                out.push_back(std::move(m));
                emitted = after;
                i = after;
            }
            else
            {
                i = pos + 1;
            }
        }
        if (emitted < t.size())
        {
            tk::TextSpan rest = sp;
            rest.text = t.substr(emitted);
            out.push_back(std::move(rest));
        }
    }
    return out;
}

} // namespace

// ── Main parser ───────────────────────────────────────────────────────────

std::vector<tk::TextSpan> html_to_spans(std::string_view html, bool dark)
{
    std::vector<tk::TextSpan> spans;
    if (html.empty())
    {
        return spans;
    }

    const char* p = html.data();
    const char* end = p + html.size();

    // Formatting stack: bottom entry = no formatting. Capped so a hostile
    // body of deeply nested tags (<b><b><b>… ×100k) can't grow it without
    // bound and exhaust the heap; past the cap, opening tags are flattened
    // (no formatting change) rather than pushed.
    constexpr std::size_t kMaxTagDepth = 64;
    std::vector<FmtState> stack;
    stack.push_back(FmtState{});

    // Number of opening tags that were *flattened* (not pushed) because the
    // depth cap was already reached. Their matching close tags must be
    // absorbed here rather than popping a legitimate outer frame — otherwise
    // text after an over-deep section would render with the wrong styling.
    std::size_t dropped_opens = 0;

    std::string cur_text;
    bool first_block = true;

    // Flush accumulated text as a span under the current formatting state.
    auto flush = [&]()
    {
        if (cur_text.empty())
        {
            return;
        }
        const FmtState& s = stack.back();
        if (!spans.empty())
        {
            tk::TextSpan& prev = spans.back();
            if (prev.url == s.url && prev.spoiler == s.spoiler &&
                prev.spoiler_reason == s.spoiler_reason &&
                prev.bold == s.bold && prev.semibold == s.semibold &&
                prev.italic == s.italic &&
                prev.code == s.code && prev.strikethrough == s.strikethrough &&
                prev.is_mention == s.is_mention)
            {
                prev.text += cur_text;
                cur_text.clear();
                return;
            }
        }
        tk::TextSpan sp;
        sp.text = cur_text;
        sp.url = s.url;
        sp.spoiler = s.spoiler;
        sp.spoiler_reason = s.spoiler_reason;
        sp.bold = s.bold;
        sp.semibold = s.semibold;
        sp.italic = s.italic;
        sp.code = s.code;
        sp.strikethrough = s.strikethrough;
        if (s.is_mention)
        {
            // A mention pill: themed accent text on a rounded background. The
            // run keeps its `url` for hit-testing; backends skip the underline.
            sp.is_mention = true;
            sp.has_color = true;
            sp.color = dark ? tk::Color{0xA8, 0xC5, 0xFF, 0xFF}
                            : tk::Color{0x1B, 0x4A, 0xC2, 0xFF};
            sp.has_background = true;
            sp.background = dark ? tk::Color{0x2E, 0x3B, 0x5E, 0xFF}
                                 : tk::Color{0xDB, 0xE5, 0xFF, 0xFF};
        }
        spans.push_back(std::move(sp));
        cur_text.clear();
    };

    // Code-block capture: between <pre> and </pre> we collect the raw (entity-
    // decoded) source text and, if a language is known, run it through the
    // syntax highlighter to emit per-token colored spans. Unknown/empty
    // language → a single uncolored monospace span (the legacy behaviour).
    bool        in_code_block = false;
    std::string code_buf;
    std::string code_lang;

    auto emit_code_block = [&]()
    {
        std::vector<tesseract::HighlightSpan> hl;
        if (!code_lang.empty())
        {
            hl = tesseract::highlight_code(code_buf, code_lang, dark);
        }
        if (hl.empty())
        {
            tk::TextSpan sp;
            sp.text = code_buf;
            sp.code = true;
            sp.code_block = true;
            spans.push_back(std::move(sp));
        }
        else
        {
            for (auto& h : hl)
            {
                tk::TextSpan sp;
                sp.text       = std::move(h.text);
                sp.code       = true;
                sp.code_block = true;
                sp.has_color  = true;
                sp.color      = tk::Color{h.r, h.g, h.b, 255};
                spans.push_back(std::move(sp));
            }
        }
        code_buf.clear();
        code_lang.clear();
    };

    while (p < end)
    {
        if (in_code_block)
        {
            if (*p == '<')
            {
                Tag tag = parse_tag(p, end);
                if (tag.closing && tag.name == "pre")
                {
                    emit_code_block();
                    in_code_block = false;
                }
                else if (!tag.closing && tag.name == "code" &&
                         code_lang.empty())
                {
                    // Language carried on the inner <code class="language-X">.
                    code_lang = tag.code_lang;
                }
                // Any other tag inside a code block is ignored.
                continue;
            }
            if (*p == '&')
            {
                decode_entity(p, end, code_buf);
                continue;
            }
            code_buf += *p++;
            continue;
        }

        if (*p == '<')
        {
            Tag tag = parse_tag(p, end);
            if (tag.name.empty())
            {
                continue;
            }

            if (!tag.closing && !tag.self_closing)
            {
                // Opening tag — compute new formatting state.
                FmtState ns = stack.back();
                if (tag.name == "b" || tag.name == "strong")
                {
                    ns.bold = true;
                }
                else if (tag.name == "i" || tag.name == "em")
                {
                    ns.italic = true;
                }
                else if (tag.name == "code")
                {
                    ns.code = true;
                }
                else if (tag.name == "pre")
                {
                    // Enter code-block capture instead of pushing a formatting
                    // frame; the matching </pre> emits the (highlighted) span(s).
                    flush();
                    in_code_block = true;
                    code_buf.clear();
                    code_lang = tag.code_lang; // inner <code> may set it later
                    continue;
                }
                else if (tag.name == "del" || tag.name == "s" ||
                         tag.name == "strike")
                {
                    ns.strikethrough = true;
                }
                else if (tag.name == "a" && !tag.href.empty())
                {
                    ns.url = tag.href;
                    if (is_matrix_to_user_link(tag.href))
                    {
                        ns.is_mention = true;
                    }
                }
                else if (tag.name == "span" && tag.has_spoiler)
                {
                    ns.spoiler = true;
                    ns.spoiler_reason = tag.spoiler_reason;
                }
                else if (tag.name == "p")
                {
                    // Paragraph separator — blank line between paras.
                    if (!first_block)
                    {
                        flush();
                        cur_text += '\n';
                    }
                    first_block = false;
                    if (stack.size() < kMaxTagDepth)
                    {
                        stack.push_back(ns);
                    }
                    else
                    {
                        ++dropped_opens;
                    }
                    continue;
                }
                // All other opening tags (u, span, h1-h6, li, …): preserve
                // text content without changing formatting.
                flush();
                if (stack.size() < kMaxTagDepth)
                {
                    stack.push_back(ns);
                }
                else
                {
                    ++dropped_opens;
                }
            }
            else if (tag.self_closing || tag.name == "br")
            {
                flush();
                cur_text += '\n';
            }
            else
            {
                // Closing tag.
                if (tag.name == "p")
                {
                    flush();
                    cur_text += '\n';
                    flush();
                }
                else
                {
                    flush();
                }
                // Absorb a flattened (dropped) open before touching the real
                // stack, so the close of an over-deep tag can't pop a
                // legitimate outer formatting frame.
                if (dropped_opens > 0)
                {
                    --dropped_opens;
                }
                else if (stack.size() > 1)
                {
                    stack.pop_back();
                }
            }
        }
        else if (*p == '&')
        {
            decode_entity(p, end, cur_text);
        }
        else
        {
            const char c = *p++;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                // Collapse HTML whitespace (CSS white-space:normal): any run of
                // whitespace in a text node becomes at most one space.
                if (cur_text.empty() || cur_text.back() != ' ')
                    cur_text += ' ';
            }
            else
            {
                cur_text += c;
            }
        }
    }

    // Emit a code block left open by malformed HTML (missing </pre>).
    if (in_code_block)
    {
        emit_code_block();
    }

    flush();

    // Trim trailing newlines from the last span.
    if (!spans.empty())
    {
        std::string& t = spans.back().text;
        while (!t.empty() && t.back() == '\n')
        {
            t.pop_back();
        }
        if (t.empty())
        {
            spans.pop_back();
        }
    }

    return split_room_mentions(std::move(spans), dark);
}

// ── URL extraction helpers ────────────────────────────────────────────────

// matrix.to permalinks (https://matrix.to/#/@user:s, #!room:s, $event, etc.)
// are Matrix entity references, not web pages — skip them for URL preview.
static bool is_matrix_to_url(const std::string& url)
{
    return url.rfind("https://matrix.to", 0) == 0 ||
           url.rfind("http://matrix.to", 0) == 0;
}

std::string first_url_from_html(std::string_view html)
{
    // Walk the HTML looking for the first <a href="http(s)://..."> tag that
    // is not a matrix.to permalink.
    const char* p = html.data();
    const char* end = p + html.size();
    while (p < end)
    {
        if (*p == '<')
        {
            Tag tag = parse_tag(p, end);
            if (!tag.href.empty() && !is_matrix_to_url(tag.href))
            {
                return tag.href;
            }
        }
        else
        {
            ++p;
        }
    }
    return {};
}

std::string first_url_from_plain(std::string_view text)
{
    // Scan for the first "https://" or "http://" prefix that is not a
    // matrix.to permalink.
    static constexpr std::string_view kHttps = "https://";
    static constexpr std::string_view kHttp = "http://";
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end)
    {
        // Try https:// first (longer prefix wins).
        auto try_prefix = [&](std::string_view prefix) -> std::string
        {
            if (static_cast<std::size_t>(end - p) < prefix.size())
            {
                return {};
            }
            if (std::string_view(p, prefix.size()) != prefix)
            {
                return {};
            }
            // Collect URL characters until whitespace or end.
            const char* url_start = p;
            const char* q = p + prefix.size();
            while (q < end && !std::isspace(static_cast<unsigned char>(*q)))
            {
                ++q;
            }
            // Strip common trailing punctuation that follows a URL in prose.
            while (q > url_start + prefix.size())
            {
                char last = *(q - 1);
                if (last == '.' || last == ',' || last == ':' || last == ';' ||
                    last == '!' || last == '?' || last == ')' || last == ']')
                {
                    --q;
                }
                else
                {
                    break;
                }
            }
            return std::string(url_start,
                               static_cast<std::size_t>(q - url_start));
        };
        std::string url = try_prefix(kHttps);
        if (url.empty())
        {
            url = try_prefix(kHttp);
        }
        if (!url.empty() && !is_matrix_to_url(url))
        {
            return url;
        }
        ++p;
    }
    return {};
}

namespace
{

// Length of the http(s) URL starting exactly at text[pos], or 0 if none
// starts there. Mirrors first_url_from_plain's extent and trailing-prose-
// punctuation rules, plus a word-boundary guard so we never link mid-token
// (e.g. the "https://x" inside "ahttps://x").
std::size_t plain_url_len_at(std::string_view text, std::size_t pos)
{
    static constexpr std::string_view kHttps = "https://";
    static constexpr std::string_view kHttp = "http://";

    std::string_view rest = text.substr(pos);
    std::string_view prefix;
    if (rest.rfind(kHttps, 0) == 0)
    {
        prefix = kHttps;
    }
    else if (rest.rfind(kHttp, 0) == 0)
    {
        prefix = kHttp;
    }
    else
    {
        return 0;
    }

    // Word-boundary guard: the char before the scheme must not be
    // alphanumeric (start-of-text is fine).
    if (pos > 0 && std::isalnum(static_cast<unsigned char>(text[pos - 1])))
    {
        return 0;
    }

    std::size_t i = prefix.size();
    while (i < rest.size() &&
           !std::isspace(static_cast<unsigned char>(rest[i])))
    {
        ++i;
    }
    while (i > prefix.size())
    {
        char last = rest[i - 1];
        if (last == '.' || last == ',' || last == ':' || last == ';' ||
            last == '!' || last == '?' || last == ')' || last == ']')
        {
            --i;
        }
        else
        {
            break;
        }
    }
    // Need at least one host character after the scheme.
    return i > prefix.size() ? i : 0;
}

} // namespace

std::vector<tk::TextSpan> autolink_plain_to_spans(std::string_view text)
{
    std::vector<tk::TextSpan> spans;
    std::size_t plain_start = 0;
    std::size_t i = 0;
    bool found = false;

    while (i < text.size())
    {
        std::size_t len = plain_url_len_at(text, i);
        if (len == 0)
        {
            ++i;
            continue;
        }

        found = true;
        if (i > plain_start)
        {
            tk::TextSpan s;
            s.text = std::string(text.substr(plain_start, i - plain_start));
            spans.push_back(std::move(s));
        }
        tk::TextSpan link;
        link.text = std::string(text.substr(i, len));
        link.url = link.text;
        spans.push_back(std::move(link));

        i += len;
        plain_start = i;
    }

    if (!found)
    {
        return {};
    }
    if (plain_start < text.size())
    {
        tk::TextSpan s;
        s.text = std::string(text.substr(plain_start));
        spans.push_back(std::move(s));
    }
    return spans;
}

// ── Block-level parser ───────────────────────────────────────────────────────
//
// Parses a Matrix HTML formatted_body into a vector of BodyBlocks.  Each
// block-level element (heading, list item, blockquote, table row, paragraph)
// becomes its own BodyBlock; inline formatting within each block works
// identically to html_to_spans().

std::vector<BodyBlock> html_to_blocks(std::string_view html, bool dark)
{
    if (html.empty())
        return {};

    std::vector<BodyBlock> blocks;

    // ── Block-context stack ──────────────────────────────────────────────────
    // Tracks open <ul>, <ol>, <blockquote> so nesting depth and list counters
    // are maintained as we descend into nested structures.
    struct BlockCtx
    {
        enum Kind { BQ, UL, OL };
        Kind kind;
        int  level;    // 1-based nesting depth
        int  counter;  // next item number for OL; unused for BQ/UL
    };
    std::vector<BlockCtx> bctx;

    bool in_thead = false; // inside <thead>

    // Current block being assembled.
    BodyBlock cur;
    cur.kind  = BodyBlock::Kind::Paragraph;
    cur.level = 0;
    cur.index = 0;

    // ── Inline formatting stack ──────────────────────────────────────────────
    // Mirrors html_to_spans() exactly so inline formatting works per-block.
    constexpr std::size_t kMaxTagDepth = 64;
    std::vector<FmtState> stack;
    stack.push_back(FmtState{});
    std::size_t dropped_opens = 0;

    std::string cur_text;

    // flush() — emit accumulated text as a span into cur.spans.
    auto flush = [&]()
    {
        if (cur_text.empty())
            return;
        const FmtState& s = stack.back();
        if (!cur.spans.empty())
        {
            tk::TextSpan& prev = cur.spans.back();
            if (prev.url == s.url && prev.spoiler == s.spoiler &&
                prev.spoiler_reason == s.spoiler_reason &&
                prev.bold == s.bold && prev.semibold == s.semibold &&
                prev.italic == s.italic &&
                prev.code == s.code &&
                prev.strikethrough == s.strikethrough &&
                prev.is_mention == s.is_mention)
            {
                prev.text += cur_text;
                cur_text.clear();
                return;
            }
        }
        tk::TextSpan sp;
        sp.text          = cur_text;
        sp.url           = s.url;
        sp.spoiler       = s.spoiler;
        sp.spoiler_reason = s.spoiler_reason;
        sp.bold          = s.bold;
        sp.semibold      = s.semibold;
        sp.italic        = s.italic;
        sp.code          = s.code;
        sp.strikethrough = s.strikethrough;
        if (s.is_mention)
        {
            sp.is_mention    = true;
            sp.has_color     = true;
            sp.color         = dark ? tk::Color{0xA8, 0xC5, 0xFF, 0xFF}
                                    : tk::Color{0x1B, 0x4A, 0xC2, 0xFF};
            sp.has_background = true;
            sp.background    = dark ? tk::Color{0x2E, 0x3B, 0x5E, 0xFF}
                                    : tk::Color{0xDB, 0xE5, 0xFF, 0xFF};
        }
        cur.spans.push_back(std::move(sp));
        cur_text.clear();
    };

    // commit_block() — trim, @room-split, and push cur to blocks; reset cur.
    auto commit_block = [&]()
    {
        flush();
        if (!cur.spans.empty())
        {
            // Trim trailing newlines from the last span.
            std::string& t = cur.spans.back().text;
            while (!t.empty() && t.back() == '\n')
                t.pop_back();
            if (t.empty())
                cur.spans.pop_back();
        }
        if (!cur.spans.empty())
        {
            cur.spans = split_room_mentions(std::move(cur.spans), dark);
            blocks.push_back(std::move(cur));
        }
        cur        = BodyBlock{};
        cur.kind   = BodyBlock::Kind::Paragraph;
        cur.level  = 0;
        cur.index  = 0;
    };

    // Code-block capture — same as html_to_spans().
    bool        in_code_block = false;
    std::string code_buf;
    std::string code_lang;

    auto emit_code_block = [&]()
    {
        // Flush any preceding inline content as its own block first.
        commit_block();
        // Build the code-block block.
        std::vector<tesseract::HighlightSpan> hl;
        if (!code_lang.empty())
            hl = tesseract::highlight_code(code_buf, code_lang, dark);
        if (hl.empty())
        {
            tk::TextSpan sp;
            sp.text       = code_buf;
            sp.code       = true;
            sp.code_block = true;
            cur.spans.push_back(std::move(sp));
        }
        else
        {
            for (auto& h : hl)
            {
                tk::TextSpan sp;
                sp.text      = std::move(h.text);
                sp.code      = true;
                sp.code_block = true;
                sp.has_color  = true;
                sp.color      = tk::Color{h.r, h.g, h.b, 255};
                cur.spans.push_back(std::move(sp));
            }
        }
        commit_block(); // push the code block
        code_buf.clear();
        code_lang.clear();
    };

    // ── Helpers ──────────────────────────────────────────────────────────────

    // Return the innermost list context or nullptr.
    auto inner_list = [&]() -> BlockCtx*
    {
        for (int i = static_cast<int>(bctx.size()) - 1; i >= 0; --i)
            if (bctx[i].kind == BlockCtx::UL || bctx[i].kind == BlockCtx::OL)
                return &bctx[i];
        return nullptr;
    };

    // Return current blockquote nesting depth (0 if not in any).
    auto bq_depth = [&]() -> int
    {
        int d = 0;
        for (const auto& c : bctx)
            if (c.kind == BlockCtx::BQ) ++d;
        return d;
    };

    // Return current list nesting depth (0 if not in any).
    auto list_depth = [&]() -> int
    {
        int d = 0;
        for (const auto& c : bctx)
            if (c.kind == BlockCtx::UL || c.kind == BlockCtx::OL) ++d;
        return d;
    };

    // True when inside a list item or blockquote (block container).
    auto in_block_container = [&]() -> bool
    {
        return !bctx.empty();
    };

    // ── Main loop ─────────────────────────────────────────────────────────────

    const char* p   = html.data();
    const char* end = p + html.size();

    // Suppress leading paragraph separator inside each block.
    bool first_in_block = true;

    while (p < end)
    {
        // ── Code-block capture ───────────────────────────────────────────────
        if (in_code_block)
        {
            if (*p == '<')
            {
                Tag tag = parse_tag(p, end);
                if (tag.closing && tag.name == "pre")
                {
                    emit_code_block();
                    in_code_block  = false;
                    first_in_block = true;
                }
                else if (!tag.closing && tag.name == "code" &&
                         code_lang.empty())
                {
                    code_lang = tag.code_lang;
                }
                continue;
            }
            if (*p == '&')
            {
                decode_entity(p, end, code_buf);
                continue;
            }
            code_buf += *p++;
            continue;
        }

        if (*p != '<')
        {
            if (*p == '&')
            {
                decode_entity(p, end, cur_text);
            }
            else
            {
                const char c = *p++;
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    if (cur_text.empty() || cur_text.back() != ' ')
                        cur_text += ' ';
                }
                else
                {
                    cur_text += c;
                }
            }
            continue;
        }

        // ── Tag dispatch ─────────────────────────────────────────────────────
        Tag tag = parse_tag(p, end);
        if (tag.name.empty())
            continue;

        // ── Block-level tags ─────────────────────────────────────────────────

        // Headings.
        if (tag.name.size() == 2 && tag.name[0] == 'h' &&
            tag.name[1] >= '1' && tag.name[1] <= '6')
        {
            if (!tag.closing)
            {
                commit_block();
                cur.kind  = BodyBlock::Kind::Heading;
                cur.level = tag.name[1] - '0';
                // Push semibold so all heading text is semibold.
                FmtState ns = stack.back();
                ns.semibold = true;
                if (stack.size() < kMaxTagDepth)
                    stack.push_back(ns);
                else
                    ++dropped_opens;
                first_in_block = true;
            }
            else
            {
                commit_block();
                if (dropped_opens > 0)
                    --dropped_opens;
                else if (stack.size() > 1)
                    stack.pop_back();
                cur.kind  = BodyBlock::Kind::Paragraph;
                cur.level = 0;
                first_in_block = true;
            }
            continue;
        }

        // Blockquote.
        if (tag.name == "blockquote")
        {
            if (!tag.closing)
            {
                commit_block();
                int depth = bq_depth() + 1;
                bctx.push_back({BlockCtx::BQ, depth, 0});
                cur.kind  = BodyBlock::Kind::Blockquote;
                cur.level = depth;
                first_in_block = true;
            }
            else
            {
                commit_block();
                for (int i = static_cast<int>(bctx.size()) - 1; i >= 0; --i)
                {
                    if (bctx[i].kind == BlockCtx::BQ)
                    {
                        bctx.erase(bctx.begin() + i);
                        break;
                    }
                }
                // Restore block kind from remaining context.
                if (bq_depth() > 0)
                {
                    cur.kind  = BodyBlock::Kind::Blockquote;
                    cur.level = bq_depth();
                }
                else
                {
                    cur.kind  = BodyBlock::Kind::Paragraph;
                    cur.level = 0;
                }
                first_in_block = true;
            }
            continue;
        }

        // Unordered list.
        if (tag.name == "ul")
        {
            if (!tag.closing)
            {
                int depth = list_depth() + 1;
                bctx.push_back({BlockCtx::UL, depth, 0});
            }
            else
            {
                for (int i = static_cast<int>(bctx.size()) - 1; i >= 0; --i)
                {
                    if (bctx[i].kind == BlockCtx::UL)
                    {
                        bctx.erase(bctx.begin() + i);
                        break;
                    }
                }
            }
            continue;
        }

        // Ordered list.
        if (tag.name == "ol")
        {
            if (!tag.closing)
            {
                int depth = list_depth() + 1;
                bctx.push_back({BlockCtx::OL, depth, 0});
            }
            else
            {
                for (int i = static_cast<int>(bctx.size()) - 1; i >= 0; --i)
                {
                    if (bctx[i].kind == BlockCtx::OL)
                    {
                        bctx.erase(bctx.begin() + i);
                        break;
                    }
                }
            }
            continue;
        }

        // List item.
        if (tag.name == "li")
        {
            if (!tag.closing)
            {
                commit_block();
                BlockCtx* lc = inner_list();
                if (lc && lc->kind == BlockCtx::OL)
                {
                    cur.kind  = BodyBlock::Kind::OrderedItem;
                    cur.index = ++lc->counter;
                    cur.level = lc->level;
                }
                else
                {
                    cur.kind  = BodyBlock::Kind::UnorderedItem;
                    cur.index = 0;
                    cur.level = lc ? lc->level : 1;
                }
                first_in_block = true;
            }
            else
            {
                commit_block();
                cur.kind  = BodyBlock::Kind::Paragraph;
                cur.level = 0;
                first_in_block = true;
            }
            continue;
        }

        // Table structure.
        if (tag.name == "table" || tag.name == "tbody")
            continue; // no action needed; tr/td drive the output

        if (tag.name == "thead")
        {
            in_thead = !tag.closing;
            continue;
        }

        if (tag.name == "tr")
        {
            if (!tag.closing)
            {
                commit_block();
                cur.kind  = BodyBlock::Kind::TableRow;
                cur.level = 0;
                cur.index = in_thead ? 1 : 0;
                first_in_block = true;
            }
            else
            {
                commit_block();
                cur.kind  = BodyBlock::Kind::Paragraph;
                cur.level = 0;
                first_in_block = true;
            }
            continue;
        }

        if (tag.name == "td" || tag.name == "th")
        {
            if (!tag.closing)
            {
                // Insert a cell-separator span before all but the first cell.
                flush();
                if (!cur.spans.empty() ||
                    !cur_text.empty())
                {
                    flush();
                    if (!cur.spans.empty())
                    {
                        tk::TextSpan sep;
                        sep.text = " \xe2\x94\x82 "; // " │ " (U+2502)
                        cur.spans.push_back(std::move(sep));
                    }
                }
                // Header cells are bold.
                if (tag.name == "th")
                {
                    FmtState ns = stack.back();
                    ns.bold = true;
                    if (stack.size() < kMaxTagDepth)
                        stack.push_back(ns);
                    else
                        ++dropped_opens;
                }
            }
            else
            {
                flush();
                if (tag.name == "th")
                {
                    if (dropped_opens > 0)
                        --dropped_opens;
                    else if (stack.size() > 1)
                        stack.pop_back();
                }
            }
            continue;
        }

        // ── Inline-level tags (same logic as html_to_spans()) ────────────────

        if (!tag.closing && !tag.self_closing)
        {
            FmtState ns = stack.back();
            if (tag.name == "b" || tag.name == "strong")
                ns.bold = true;
            else if (tag.name == "i" || tag.name == "em")
                ns.italic = true;
            else if (tag.name == "code")
                ns.code = true;
            else if (tag.name == "pre")
            {
                flush();
                in_code_block = true;
                code_buf.clear();
                code_lang = tag.code_lang;
                continue;
            }
            else if (tag.name == "del" || tag.name == "s" ||
                     tag.name == "strike")
                ns.strikethrough = true;
            else if (tag.name == "a" && !tag.href.empty())
            {
                ns.url = tag.href;
                if (is_matrix_to_user_link(tag.href))
                    ns.is_mention = true;
            }
            else if (tag.name == "span" && tag.has_spoiler)
            {
                ns.spoiler        = true;
                ns.spoiler_reason = tag.spoiler_reason;
            }
            else if (tag.name == "p")
            {
                if (in_block_container())
                {
                    // Inside a list item / blockquote: treat <p> as a line
                    // separator within the block, not a new block.
                    if (!first_in_block)
                    {
                        flush();
                        cur_text += '\n';
                    }
                    first_in_block = false;
                }
                else
                {
                    // Top-level <p>: start a new paragraph block.
                    commit_block();
                    cur.kind = BodyBlock::Kind::Paragraph;
                    first_in_block = true;
                }
                if (stack.size() < kMaxTagDepth)
                    stack.push_back(ns);
                else
                    ++dropped_opens;
                continue;
            }
            flush();
            first_in_block = false;
            if (stack.size() < kMaxTagDepth)
                stack.push_back(ns);
            else
                ++dropped_opens;
        }
        else if (tag.self_closing || tag.name == "br")
        {
            flush();
            cur_text += '\n';
        }
        else
        {
            // Closing tag.
            if (tag.name == "p")
            {
                flush();
                if (in_block_container())
                    cur_text += '\n';
                else
                    cur_text += '\n'; // paragraph end newline
                flush();
            }
            else
            {
                flush();
            }
            if (dropped_opens > 0)
                --dropped_opens;
            else if (stack.size() > 1)
                stack.pop_back();
        }
    }

    // Flush any open code block (missing </pre>).
    if (in_code_block)
        emit_code_block();

    commit_block();
    return blocks;
}

} // namespace tesseract::views
