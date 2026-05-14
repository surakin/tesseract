#include "html_spans.h"

#include <cctype>
#include <cstdint>
#include <cstring>
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
                cp = cp * 16u;
                if (std::isdigit(static_cast<unsigned char>(*p)))
                    cp += static_cast<uint32_t>(*p - '0');
                else
                    cp += static_cast<uint32_t>(
                        std::tolower(static_cast<unsigned char>(*p)) - 'a' + 10);
                ++p;
            }
            if (p == hex_start) { out += '&'; p = start + 1; return; }
        } else {
            const char* dec_start = p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
                cp = cp * 10u + static_cast<uint32_t>(*p++ - '0');
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
    std::string name;  // lower-cased tag name
    bool closing     = false;
    bool self_closing = false;
};

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

    // Skip attributes, detect '/>' self-close
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
    return t;
}

// ── Formatting state ──────────────────────────────────────────────────────

struct FmtState {
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

    // Formatting stack: bottom entry = no formatting.
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
            if (prev.bold == s.bold && prev.italic == s.italic &&
                prev.code == s.code && prev.strikethrough == s.strikethrough) {
                prev.text += cur_text;
                cur_text.clear();
                return;
            }
        }
        spans.push_back(tk::TextSpan{cur_text, s.bold, s.italic, s.code,
                                      s.strikethrough});
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
                else if (tag.name == "p") {
                    // Paragraph separator — blank line between paras.
                    if (!first_block) {
                        flush();
                        cur_text += '\n';
                    }
                    first_block = false;
                    stack.push_back(ns);
                    continue;
                }
                // All other opening tags (a, u, span, h1-h6, li, …): preserve
                // text content without changing formatting.
                flush();
                stack.push_back(ns);
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

} // namespace tesseract::views
