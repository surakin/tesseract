#include "html_spans.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace tesseract::views {

namespace {

// ── UTF-8 encoding ────────────────────────────────────────────────────────

void append_codepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ── Entity decoding ───────────────────────────────────────────────────────

// Decode one HTML entity starting at `p` (pointing at '&').
// Appends decoded text to `out` and advances `p` past the entity.
void decode_entity(const char*& p, const char* end, std::string& out) {
    const char* start = p; // points at '&'
    ++p;                   // skip '&'
    if (p >= end) { out += '&'; return; }

    if (*p == '#') {
        ++p;
        if (p >= end) { out += '&'; p = start + 1; return; }
        uint32_t cp = 0;
        if (*p == 'x' || *p == 'X') {
            ++p;
            const char* hex_start = p;
            while (p < end && std::isxdigit(static_cast<unsigned char>(*p))) {
                if (cp <= 0x10FFFF) {
                    cp = cp * 16u;
                    if (std::isdigit(static_cast<unsigned char>(*p)))
                        cp += static_cast<uint32_t>(*p - '0');
                    else
                        cp += static_cast<uint32_t>(
                            std::tolower(static_cast<unsigned char>(*p)) - 'a' + 10);
                }
                // else: keep consuming digits but stop accumulating so the
                // uint32_t can't silently wrap into a small valid codepoint.
                ++p;
            }
            if (p == hex_start) { out += '&'; p = start + 1; return; }
        } else {
            const char* dec_start = p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                if (cp <= 0x10FFFF)
                    cp = cp * 10u + static_cast<uint32_t>(*p - '0');
                ++p;
            }
            if (p == dec_start) { out += '&'; p = start + 1; return; }
        }
        if (p < end && *p == ';') ++p;
        append_codepoint(out, cp);
        return;
    }

    // Named entity — scan up to ';'
    const char* name_start = p;
    while (p < end && *p != ';' && *p != '<' && *p != '&'
           && !std::isspace(static_cast<unsigned char>(*p)))
        ++p;
    std::string_view ent(name_start, static_cast<std::size_t>(p - name_start));
    if (p < end && *p == ';') ++p;

    if      (ent == "amp")  { out += '&'; }
    else if (ent == "lt")   { out += '<'; }
    else if (ent == "gt")   { out += '>'; }
    else if (ent == "quot") { out += '"'; }
    else if (ent == "apos") { out += '\''; }
    else if (ent == "nbsp") { out += '\xc2'; out += '\xa0'; } // U+00A0
    // Unknown entity: emit literal '&name;' fallback
    else { out += '&'; out += std::string(ent); out += ';'; }
}

// ── Tag scanning ──────────────────────────────────────────────────────────

struct Tag {
    std::string name;          // lower-cased tag name
    std::string href;          // non-empty only for <a href="http(s)://...">
    std::string spoiler_reason; // value of data-mx-spoiler (may be empty)
    bool has_spoiler  = false; // true when data-mx-spoiler attribute present
    bool closing      = false;
    bool self_closing = false;
};

// Scan [p, end) for the attribute named `target` (case-insensitive).
// Returns the attribute value when found (empty string for boolean attributes
// with no value), or std::nullopt when the attribute is absent entirely.
static std::optional<std::string> extract_attr(const char* p, const char* end,
                                               const char* target) {
    const std::size_t tlen = std::strlen(target);
    while (p < end) {
        // Skip whitespace between attributes.
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end || *p == '>' || *p == '/') break;

        // Read attribute name.
        const char* name_start = p;
        while (p < end && *p != '=' && *p != '>' && *p != '/'
               && !std::isspace(static_cast<unsigned char>(*p)))
            ++p;
        const std::size_t name_len = static_cast<std::size_t>(p - name_start);

        // Skip optional whitespace + '='.
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        bool has_value = (p < end && *p == '=');
        if (has_value) {
            ++p; // skip '='
            while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        }

        // Read (quoted) value.
        std::string value;
        if (has_value && p < end && (*p == '"' || *p == '\'')) {
            char qch = *p++;
            while (p < end && *p != qch) value += *p++;
            if (p < end) ++p; // closing quote
        } else if (has_value) {
            // Unquoted value.
            while (p < end && !std::isspace(static_cast<unsigned char>(*p))
                   && *p != '>' && *p != '/')
                value += *p++;
        }

        // Case-insensitive name comparison.
        if (name_len == tlen) {
            bool match = true;
            for (std::size_t i = 0; i < tlen && match; ++i)
                match = (std::tolower(static_cast<unsigned char>(name_start[i]))
                         == static_cast<unsigned char>(target[i]));
            if (match) return value;
        }
    }
    return std::nullopt;
}

// Parse one tag starting at `p` (pointing at '<'). Advances `p` past '>'.
Tag parse_tag(const char*& p, const char* end) {
    Tag t{};
    ++p; // skip '<'
    if (p < end && *p == '/')  { t.closing = true; ++p; }
    if (p < end && *p == '!')  {
        // Comment or DOCTYPE — skip to '>'
        while (p < end && *p != '>') ++p;
        if (p < end) ++p;
        return t;
    }

    while (p < end && (std::isalnum(static_cast<unsigned char>(*p)) || *p == '-'))
        t.name += static_cast<char>(std::tolower(static_cast<unsigned char>(*p++)));

    // Record attribute start for href extraction on <a> tags.
    const char* attr_start = p;

    // Skip attributes, detect '/>' self-close.
    bool in_quote = false;
    char quote_ch = 0;
    while (p < end) {
        char c = *p++;
        if (in_quote) {
            if (c == quote_ch) in_quote = false;
        } else if (c == '"' || c == '\'') {
            in_quote = true; quote_ch = c;
        } else if (c == '/') {
            t.self_closing = true;
        } else if (c == '>') {
            break;
        }
    }

    const char* attr_end = p - 1; // points just before '>'

    // For opening <a> tags extract the href attribute.
    if (t.name == "a" && !t.closing) {
        auto href = extract_attr(attr_start, attr_end, "href");
        if (href && (href->rfind("http://", 0) == 0 || href->rfind("https://", 0) == 0))
            t.href = std::move(*href);
    }

    // For opening <span> tags detect data-mx-spoiler (MSC2010).
    if (t.name == "span" && !t.closing) {
        auto v = extract_attr(attr_start, attr_end, "data-mx-spoiler");
        if (v.has_value()) {
            t.has_spoiler    = true;
            t.spoiler_reason = std::move(*v);
        }
    }

    return t;
}

// ── Formatting state ──────────────────────────────────────────────────────

struct FmtState {
    std::string url;
    std::string spoiler_reason;
    bool spoiler       = false;
    bool bold          = false;
    bool italic        = false;
    bool code          = false;
    bool strikethrough = false;
};

} // namespace

// ── Main parser ───────────────────────────────────────────────────────────

std::vector<tk::TextSpan> html_to_spans(std::string_view html) {
    std::vector<tk::TextSpan> spans;
    if (html.empty()) return spans;

    const char* p   = html.data();
    const char* end = p + html.size();

    // Formatting stack: bottom entry = no formatting. Capped so a hostile
    // body of deeply nested tags (<b><b><b>… ×100k) can't grow it without
    // bound and exhaust the heap; past the cap, opening tags are flattened
    // (no formatting change) rather than pushed.
    constexpr std::size_t kMaxTagDepth = 64;
    std::vector<FmtState> stack;
    stack.push_back(FmtState{});

    std::string cur_text;
    bool first_block = true;

    // Flush accumulated text as a span under the current formatting state.
    auto flush = [&]() {
        if (cur_text.empty()) return;
        const FmtState& s = stack.back();
        if (!spans.empty()) {
            tk::TextSpan& prev = spans.back();
            if (prev.url == s.url &&
                prev.spoiler == s.spoiler &&
                prev.spoiler_reason == s.spoiler_reason &&
                prev.bold == s.bold && prev.italic == s.italic &&
                prev.code == s.code && prev.strikethrough == s.strikethrough) {
                prev.text += cur_text;
                cur_text.clear();
                return;
            }
        }
        tk::TextSpan sp;
        sp.text           = cur_text;
        sp.url            = s.url;
        sp.spoiler        = s.spoiler;
        sp.spoiler_reason = s.spoiler_reason;
        sp.bold           = s.bold;
        sp.italic         = s.italic;
        sp.code           = s.code;
        sp.strikethrough  = s.strikethrough;
        spans.push_back(std::move(sp));
        cur_text.clear();
    };

    while (p < end) {
        if (*p == '<') {
            Tag tag = parse_tag(p, end);
            if (tag.name.empty()) continue;

            if (!tag.closing && !tag.self_closing) {
                // Opening tag — compute new formatting state.
                FmtState ns = stack.back();
                if      (tag.name == "b" || tag.name == "strong") ns.bold = true;
                else if (tag.name == "i" || tag.name == "em")     ns.italic = true;
                else if (tag.name == "code")                       ns.code = true;
                else if (tag.name == "pre")                        ns.code = true;
                else if (tag.name == "del" || tag.name == "s"
                         || tag.name == "strike")                  ns.strikethrough = true;
                else if (tag.name == "a" && !tag.href.empty())     ns.url = tag.href;
                else if (tag.name == "span" && tag.has_spoiler) {
                    ns.spoiler        = true;
                    ns.spoiler_reason = tag.spoiler_reason;
                }
                else if (tag.name == "p") {
                    // Paragraph separator — blank line between paras.
                    if (!first_block) {
                        flush();
                        cur_text += '\n';
                    }
                    first_block = false;
                    if (stack.size() < kMaxTagDepth) stack.push_back(ns);
                    continue;
                }
                // All other opening tags (u, span, h1-h6, li, …): preserve
                // text content without changing formatting.
                flush();
                if (stack.size() < kMaxTagDepth) stack.push_back(ns);
            } else if (tag.self_closing || tag.name == "br") {
                flush();
                cur_text += '\n';
            } else {
                // Closing tag.
                if (tag.name == "p") {
                    flush();
                    cur_text += '\n';
                    flush();
                } else {
                    flush();
                }
                if (stack.size() > 1) stack.pop_back();
            }
        } else if (*p == '&') {
            decode_entity(p, end, cur_text);
        } else {
            cur_text += *p++;
        }
    }

    flush();

    // Trim trailing newlines from the last span.
    if (!spans.empty()) {
        std::string& t = spans.back().text;
        while (!t.empty() && t.back() == '\n')
            t.pop_back();
        if (t.empty()) spans.pop_back();
    }

    return spans;
}

// ── URL extraction helpers ────────────────────────────────────────────────

std::string first_url_from_html(std::string_view html) {
    // Walk the HTML looking for the first <a href="http(s)://..."> tag.
    const char* p   = html.data();
    const char* end = p + html.size();
    while (p < end) {
        if (*p == '<') {
            Tag tag = parse_tag(p, end);
            if (!tag.href.empty()) return tag.href;
        } else {
            ++p;
        }
    }
    return {};
}

std::string first_url_from_plain(std::string_view text) {
    // Scan for the first "https://" or "http://" prefix.
    static constexpr std::string_view kHttps = "https://";
    static constexpr std::string_view kHttp  = "http://";
    const char* p   = text.data();
    const char* end = p + text.size();
    while (p < end) {
        // Try https:// first (longer prefix wins).
        auto try_prefix = [&](std::string_view prefix) -> std::string {
            if (static_cast<std::size_t>(end - p) < prefix.size()) return {};
            if (std::string_view(p, prefix.size()) != prefix) return {};
            // Collect URL characters until whitespace or end.
            const char* url_start = p;
            const char* q = p + prefix.size();
            while (q < end && !std::isspace(static_cast<unsigned char>(*q))) ++q;
            // Strip common trailing punctuation that follows a URL in prose.
            while (q > url_start + prefix.size()) {
                char last = *(q - 1);
                if (last == '.' || last == ',' || last == ':' || last == ';'
                    || last == '!' || last == '?' || last == ')' || last == ']')
                    --q;
                else
                    break;
            }
            return std::string(url_start, static_cast<std::size_t>(q - url_start));
        };
        std::string url = try_prefix(kHttps);
        if (url.empty()) url = try_prefix(kHttp);
        if (!url.empty()) return url;
        ++p;
    }
    return {};
}

namespace {

// Length of the http(s) URL starting exactly at text[pos], or 0 if none
// starts there. Mirrors first_url_from_plain's extent and trailing-prose-
// punctuation rules, plus a word-boundary guard so we never link mid-token
// (e.g. the "https://x" inside "ahttps://x").
std::size_t plain_url_len_at(std::string_view text, std::size_t pos) {
    static constexpr std::string_view kHttps = "https://";
    static constexpr std::string_view kHttp  = "http://";

    std::string_view rest = text.substr(pos);
    std::string_view prefix;
    if      (rest.rfind(kHttps, 0) == 0) prefix = kHttps;
    else if (rest.rfind(kHttp,  0) == 0) prefix = kHttp;
    else return 0;

    // Word-boundary guard: the char before the scheme must not be
    // alphanumeric (start-of-text is fine).
    if (pos > 0
        && std::isalnum(static_cast<unsigned char>(text[pos - 1])))
        return 0;

    std::size_t i = prefix.size();
    while (i < rest.size()
           && !std::isspace(static_cast<unsigned char>(rest[i])))
        ++i;
    while (i > prefix.size()) {
        char last = rest[i - 1];
        if (last == '.' || last == ',' || last == ':' || last == ';'
            || last == '!' || last == '?' || last == ')' || last == ']')
            --i;
        else
            break;
    }
    // Need at least one host character after the scheme.
    return i > prefix.size() ? i : 0;
}

} // namespace

std::vector<tk::TextSpan> autolink_plain_to_spans(std::string_view text) {
    std::vector<tk::TextSpan> spans;
    std::size_t plain_start = 0;
    std::size_t i           = 0;
    bool        found       = false;

    while (i < text.size()) {
        std::size_t len = plain_url_len_at(text, i);
        if (len == 0) { ++i; continue; }

        found = true;
        if (i > plain_start) {
            tk::TextSpan s;
            s.text = std::string(text.substr(plain_start, i - plain_start));
            spans.push_back(std::move(s));
        }
        tk::TextSpan link;
        link.text = std::string(text.substr(i, len));
        link.url  = link.text;
        spans.push_back(std::move(link));

        i += len;
        plain_start = i;
    }

    if (!found) return {};
    if (plain_start < text.size()) {
        tk::TextSpan s;
        s.text = std::string(text.substr(plain_start));
        spans.push_back(std::move(s));
    }
    return spans;
}

} // namespace tesseract::views
