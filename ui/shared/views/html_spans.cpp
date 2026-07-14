#include "html_spans.h"

#include "tesseract/autolink.h"
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
    std::string img_src;        // <img src="..."> (MSC2545 inline emoticon)
    std::string img_alt;        // <img alt="..."> falling back to title="..."
    bool has_spoiler = false;   // true when data-mx-spoiler attribute present
    bool is_mx_emoticon = false; // <img data-mx-emoticon> attribute present
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

    // <img data-mx-emoticon src="mxc://..." alt=":shortcode:" title="..."> —
    // MSC2545 inline custom emoticon. A void element: never has a closing
    // tag, whether written self-closed ("<img .../>") or bare ("<img ...>").
    if (t.name == "img" && !t.closing)
    {
        t.is_mx_emoticon =
            extract_attr(attr_start, attr_end, "data-mx-emoticon").has_value();
        if (auto src = extract_attr(attr_start, attr_end, "src"))
        {
            t.img_src = std::move(*src);
        }
        auto alt = extract_attr(attr_start, attr_end, "alt");
        if (!alt || alt->empty())
        {
            alt = extract_attr(attr_start, attr_end, "title");
        }
        if (alt)
        {
            t.img_alt = std::move(*alt);
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
    // Set right after a forced line break (a <br> or a <p> boundary) is
    // appended to cur_text; cleared on the next real character. Lets the
    // whitespace-collapse branch below drop collapsible whitespace at the
    // start of a new line instead of turning it into a stray leading space.
    bool just_broke_line = false;

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
            // Never append text into an image span — it's a leaf run with
            // no text content of its own, not a compatible formatting run
            // to merge with, even when the surrounding formatting matches.
            if (!prev.is_image && prev.url == s.url && prev.spoiler == s.spoiler &&
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

            // <img> is a void element — never has a closing tag, whether
            // self-closed or bare — so it must be handled before the
            // opening/self-closing/closing dispatch below: a bare (non-self-
            // closed) <img> would otherwise fall into the generic "opening
            // tag" branch and get pushed onto the formatting stack expecting
            // a </img> that never arrives, silently corrupting later tag
            // nesting. Only a recognised MSC2545 emoticon with a valid mxc:
            // source becomes an image span; anything else (a plain <img>, or
            // one with an unsafe/missing src) falls back to its alt text so
            // no untrusted image source is ever rendered.
            if (!tag.closing && tag.name == "img")
            {
                if (tag.is_mx_emoticon && tag.img_src.rfind("mxc://", 0) == 0)
                {
                    flush();
                    tk::TextSpan sp;
                    sp.is_image = true;
                    sp.image_mxc = tag.img_src;
                    sp.image_alt = tag.img_alt;
                    spans.push_back(std::move(sp));
                }
                else if (!tag.img_alt.empty())
                {
                    cur_text += tag.img_alt;
                }
                continue;
            }

            // <br> is also a void element — same reasoning as <img> above.
            // Real-world formatted_body has usually been round-tripped
            // through an HTML5 sanitizer (matrix-sdk-ui's Timeline sanitizer,
            // or sdk/src/html_sanitize.rs's re-sanitization pass) by the time
            // it reaches here, which re-serializes self-closed `<br />` as
            // bare `<br>` — without this check, a bare <br> falls into the
            // generic "opening tag" branch below (since !closing && !self_
            // closing is true for it) and never emits the line break at all;
            // it just pushes a redundant formatting frame that no </br> ever
            // pops. Handling both forms here, before that dispatch, is what
            // makes <br>-separated lines actually render as multiple lines.
            if (!tag.closing && tag.name == "br")
            {
                flush();
                cur_text += '\n';
                just_broke_line = true;
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
                        just_broke_line = true;
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
                just_broke_line = true;
            }
            else
            {
                // Closing tag.
                if (tag.name == "p")
                {
                    flush();
                    cur_text += '\n';
                    just_broke_line = true;
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
            just_broke_line = false;
            decode_entity(p, end, cur_text);
        }
        else
        {
            const char c = *p++;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                // Collapse HTML whitespace (CSS white-space:normal): any run of
                // whitespace in a text node becomes at most one space — unless
                // we just emitted a forced line break (just_broke_line), in
                // which case collapsible whitespace at the start of the new
                // line vanishes entirely instead of becoming a stray leading
                // space. Browsers do this too — it's why the same
                // pretty-printed "<br />\n" markdown->HTML output renders
                // cleanly in Element but was gaining a leading space per line
                // here.
                if (!just_broke_line && (cur_text.empty() || cur_text.back() != ' '))
                    cur_text += ' ';
            }
            else
            {
                just_broke_line = false;
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

    // Trim trailing newlines from the last span. An image span legitimately
    // has empty text (it carries no text content of its own) — never drop
    // it here, only text spans left empty by trailing-newline trimming.
    if (!spans.empty() && !spans.back().is_image)
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
    for (const auto& s : tesseract::find_url_spans(text))
    {
        if (!is_matrix_to_url(s.url))
            return s.url;
    }
    return {};
}

std::vector<tk::TextSpan> autolink_plain_to_spans(std::string_view text)
{
    auto url_spans = tesseract::find_url_spans(text);
    if (url_spans.empty())
        return {};

    std::vector<tk::TextSpan> spans;
    std::size_t pos = 0;
    for (const auto& us : url_spans)
    {
        if (us.start > pos)
        {
            tk::TextSpan s;
            s.text = std::string(text.substr(pos, us.start - pos));
            spans.push_back(std::move(s));
        }
        tk::TextSpan link;
        link.text = std::string(text.substr(us.start, us.end - us.start));
        link.url  = us.url;
        spans.push_back(std::move(link));
        pos = us.end;
    }
    if (pos < text.size())
    {
        tk::TextSpan s;
        s.text = std::string(text.substr(pos));
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
    // See the identical flag in html_to_spans() above — suppresses a stray
    // leading space when collapsible whitespace immediately follows a
    // forced line break.
    bool just_broke_line = false;

    // flush() — emit accumulated text as a span into cur.spans.
    auto flush = [&]()
    {
        if (cur_text.empty())
            return;
        const FmtState& s = stack.back();
        if (!cur.spans.empty())
        {
            tk::TextSpan& prev = cur.spans.back();
            // Never append text into an image span — see html_to_spans()'s
            // identical guard.
            if (!prev.is_image && prev.url == s.url && prev.spoiler == s.spoiler &&
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
        if (!cur.spans.empty() && !cur.spans.back().is_image)
        {
            // Trim trailing newlines from the last span. An image span
            // legitimately has empty text (it carries no text content of its
            // own) — never drop it here, only text spans left empty by
            // trailing-newline trimming.
            std::string& t = cur.spans.back().text;
            while (!t.empty() && t.back() == '\n')
                t.pop_back();
            if (t.empty())
                cur.spans.pop_back();
        }
        // Trim CSS-collapsible leading whitespace from the first span so that
        // all text-layout backends share identical byte offsets in
        // paint_span_backgrounds/selection_rects.  Qt's HTML parser strips
        // block-start spaces; Pango/CoreText/D2D do not — this normalises them.
        while (!cur.spans.empty())
        {
            // An image span legitimately has empty text (it carries no text
            // content of its own) — never erase/trim it here. An empty
            // string vacuously satisfies "all whitespace"
            // (find_first_not_of returns npos), so without this guard the
            // very first span in a block being an image (no leading text
            // before it, e.g. a message that opens with a custom emoji)
            // gets silently erased entirely.
            if (cur.spans.front().is_image)
                break;
            std::string& t   = cur.spans.front().text;
            const auto   nsp = t.find_first_not_of(' ');
            if (nsp == std::string::npos)
            {
                cur.spans.erase(cur.spans.begin());
                continue;
            }
            if (nsp > 0)
                t.erase(0, nsp);
            break;
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
                just_broke_line = false;
                decode_entity(p, end, cur_text);
            }
            else
            {
                const char c = *p++;
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    // See html_to_spans()'s identical branch for why
                    // just_broke_line also gates this collapse.
                    if (!just_broke_line && (cur_text.empty() || cur_text.back() != ' '))
                        cur_text += ' ';
                }
                else
                {
                    just_broke_line = false;
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

        // See html_to_spans()'s identical check for why this must come
        // before the opening/self-closing/closing dispatch below.
        if (!tag.closing && tag.name == "img")
        {
            if (tag.is_mx_emoticon && tag.img_src.rfind("mxc://", 0) == 0)
            {
                flush();
                tk::TextSpan sp;
                sp.is_image = true;
                sp.image_mxc = tag.img_src;
                sp.image_alt = tag.img_alt;
                cur.spans.push_back(std::move(sp));
            }
            else if (!tag.img_alt.empty())
            {
                cur_text += tag.img_alt;
            }
            continue;
        }

        // <br> is also a void element — same reasoning as <img> above, and
        // as html_to_spans()'s identical check. Real-world formatted_body
        // has usually been round-tripped through an HTML5 sanitizer
        // (matrix-sdk-ui's Timeline sanitizer, or sdk/src/html_sanitize.rs's
        // re-sanitization pass) by the time it reaches here, which
        // re-serializes self-closed `<br />` as bare `<br>` — without this
        // check a bare <br> falls into the generic "opening tag" branch
        // below and never emits the line break at all.
        if (!tag.closing && tag.name == "br")
        {
            flush();
            cur_text += '\n';
            just_broke_line = true;
            continue;
        }

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
                        just_broke_line = true;
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
            just_broke_line = true;
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
                just_broke_line = true;
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

// Returns true when every non-whitespace character in `utf8` is a Unicode
// emoji codepoint (including ZWJ sequences, skin-tone modifiers, variation
// selectors, regional indicators, and keycap sequences). Used to pick the
// 2× BigEmoji font for emoji-only message bodies.
bool is_emoji_only(const std::string& utf8)
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
std::vector<tk::TextSpan> segment_emoji_runs(const tk::TextSpan& src)
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

// Expand a span vector in-place, splitting each span at emoji/text
// boundaries so backends can render emoji runs at InlineEmoji size.
void apply_emoji_segmentation(std::vector<tk::TextSpan>& spans)
{
    std::vector<tk::TextSpan> out;
    out.reserve(spans.size());
    for (const auto& sp : spans)
        for (auto& sub : segment_emoji_runs(sp))
            out.push_back(std::move(sub));
    spans = std::move(out);
}

// Byte-range variant of segment_emoji_runs for plain-string callers (e.g. a
// native composer's current text). Reuses segment_emoji_runs by wrapping the
// string in a single span, so the emoji classification logic is never
// duplicated.
std::vector<EmojiByteRange> find_emoji_byte_ranges(const std::string& utf8)
{
    std::vector<EmojiByteRange> ranges;
    if (utf8.empty())
        return ranges;

    tk::TextSpan whole;
    whole.text = utf8;
    std::size_t byte_pos = 0;
    for (const auto& sub : segment_emoji_runs(whole))
    {
        const std::size_t end = byte_pos + sub.text.size();
        if (sub.is_emoji_run)
            ranges.push_back({byte_pos, end});
        byte_pos = end;
    }
    return ranges;
}

} // namespace tesseract::views
