#include "markdown.h"
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views {

// ---------------------------------------------------------------------------
// HTML escaping
// ---------------------------------------------------------------------------

static void html_escape_to(std::string_view s, std::string& out) {
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;";  break;
        case '>': out += "&gt;";  break;
        case '"': out += "&quot;"; break;
        default:  out += c;       break;
        }
    }
}

// ---------------------------------------------------------------------------
// Fast-path: check whether text has any markdown markers worth parsing
// ---------------------------------------------------------------------------

static bool has_markdown_markers(std::string_view text) {
    bool at_bol = true;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '*' || c == '`' || c == '~' || c == '[' || c == '_')
            return true;
        if (at_bol) {
            if (c == '>' || c == '-' || c == '+')
                return true;
            if (std::isdigit(static_cast<unsigned char>(c))
                && i + 1 < text.size() && text[i + 1] == '.')
                return true;
        }
        at_bol = (c == '\n');
    }
    return false;
}

// ---------------------------------------------------------------------------
// Check whether any formatting tags (beyond <p>/<br>) were emitted
// ---------------------------------------------------------------------------

static bool has_formatting_tags(const std::string& html) {
    static const char* const kTags[] = {
        "<strong>", "<em>", "<code>", "<del>", "<a ",
        "<blockquote>", "<ul>", "<ol>", "<pre>", nullptr
    };
    for (int i = 0; kTags[i]; ++i)
        if (html.find(kTags[i]) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Inline parser — appends HTML to `out`, recurses for nested spans
// ---------------------------------------------------------------------------

static void parse_inline(std::string_view text, std::string& out,
                         int depth = 0) {
    // Bound recursion: a hostile/garbled formatted_body of deeply nested
    // emphasis markers (***…, __…) would otherwise recurse per nesting
    // level on the UI thread and overflow the stack. Past the cap, emit the
    // remainder as escaped plain text.
    constexpr int kMaxInlineDepth = 16;
    if (depth > kMaxInlineDepth) {
        html_escape_to(text, out);
        return;
    }
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        char c = text[i];

        // ---- backtick code span (`…`) ----
        if (c == '`') {
            size_t j = text.find('`', i + 1);
            if (j != std::string_view::npos) {
                out += "<code>";
                html_escape_to(text.substr(i + 1, j - i - 1), out);
                out += "</code>";
                i = j + 1;
                continue;
            }
        }

        // ---- strikethrough (~~…~~) ----
        if (c == '~' && i + 1 < n && text[i + 1] == '~') {
            size_t j = text.find("~~", i + 2);
            if (j != std::string_view::npos) {
                out += "<del>";
                parse_inline(text.substr(i + 2, j - (i + 2)), out, depth + 1);
                out += "</del>";
                i = j + 2;
                continue;
            }
        }

        // ---- bold + italic (***…***) — must test before ** and * ----
        if (c == '*' && i + 2 < n && text[i + 1] == '*' && text[i + 2] == '*') {
            size_t j = text.find("***", i + 3);
            if (j != std::string_view::npos) {
                out += "<strong><em>";
                parse_inline(text.substr(i + 3, j - (i + 3)), out, depth + 1);
                out += "</em></strong>";
                i = j + 3;
                continue;
            }
        }

        // ---- bold (**…**) ----
        if (c == '*' && i + 1 < n && text[i + 1] == '*') {
            size_t j = text.find("**", i + 2);
            if (j != std::string_view::npos) {
                out += "<strong>";
                parse_inline(text.substr(i + 2, j - (i + 2)), out, depth + 1);
                out += "</strong>";
                i = j + 2;
                continue;
            }
        }

        // ---- italic (*…*) ----
        if (c == '*') {
            size_t j = text.find('*', i + 1);
            if (j != std::string_view::npos) {
                out += "<em>";
                parse_inline(text.substr(i + 1, j - (i + 1)), out, depth + 1);
                out += "</em>";
                i = j + 1;
                continue;
            }
        }

        // ---- bold (__…__) — only at word boundaries ----
        if (c == '_' && i + 1 < n && text[i + 1] == '_') {
            bool left_ok = (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])));
            if (left_ok) {
                size_t j = text.find("__", i + 2);
                if (j != std::string_view::npos) {
                    bool right_ok = (j + 2 >= n
                                     || !std::isalnum(static_cast<unsigned char>(text[j + 2])));
                    if (right_ok) {
                        out += "<strong>";
                        parse_inline(text.substr(i + 2, j - (i + 2)), out, depth + 1);
                        out += "</strong>";
                        i = j + 2;
                        continue;
                    }
                }
            }
        }

        // ---- italic (_…_) — only at word boundaries ----
        if (c == '_') {
            bool left_ok = (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])));
            if (left_ok) {
                size_t j = text.find('_', i + 1);
                if (j != std::string_view::npos) {
                    bool right_ok = (j + 1 >= n
                                     || !std::isalnum(static_cast<unsigned char>(text[j + 1])));
                    if (right_ok) {
                        out += "<em>";
                        parse_inline(text.substr(i + 1, j - (i + 1)), out, depth + 1);
                        out += "</em>";
                        i = j + 1;
                        continue;
                    }
                }
            }
        }

        // ---- link ([label](url)) — http(s) only ----
        if (c == '[') {
            size_t close_br = text.find(']', i + 1);
            if (close_br != std::string_view::npos
                && close_br + 1 < n && text[close_br + 1] == '(') {
                size_t url_start = close_br + 2;
                size_t url_end   = text.find(')', url_start);
                if (url_end != std::string_view::npos) {
                    std::string_view url = text.substr(url_start, url_end - url_start);
                    if (url.size() >= 7
                        && (url.substr(0, 7) == "http://" || url.substr(0, 8) == "https://")) {
                        out += "<a href=\"";
                        html_escape_to(url, out);
                        out += "\">";
                        parse_inline(text.substr(i + 1, close_br - (i + 1)), out, depth + 1);
                        out += "</a>";
                        i = url_end + 1;
                        continue;
                    }
                }
            }
        }

        // ---- plain character (with HTML escaping) ----
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;";  break;
        case '>': out += "&gt;";  break;
        default:  out += c;       break;
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------
// Block-level parser
// ---------------------------------------------------------------------------

enum class BlockMode { None, Paragraph, Blockquote, UL, OL, Code };

MarkdownResult markdown_to_html(std::string_view text) {
    std::string body(text);
    if (!has_markdown_markers(text)) return {body, ""};

    // Split into lines (preserving empty lines)
    std::vector<std::string_view> lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
    }

    std::string html;
    html.reserve(text.size() * 2);

    BlockMode mode    = BlockMode::None;
    bool      in_fence = false;

    auto close_block = [&]() {
        switch (mode) {
        case BlockMode::Paragraph:  html += "</p>";              break;
        case BlockMode::Blockquote: html += "</blockquote>";     break;
        case BlockMode::UL:         html += "</li></ul>";        break;
        case BlockMode::OL:         html += "</li></ol>";        break;
        case BlockMode::Code:       html += "</code></pre>";
                                    in_fence = false;            break;
        case BlockMode::None:                                    break;
        }
        mode = BlockMode::None;
    };

    for (std::string_view line : lines) {
        // ---- fenced code block open/close ----
        if (line.size() >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
            if (in_fence) {
                close_block();
            } else {
                close_block();
                html += "<pre><code>";
                mode     = BlockMode::Code;
                in_fence = true;
            }
            continue;
        }
        if (in_fence) {
            html_escape_to(line, html);
            html += '\n';
            continue;
        }

        // ---- blank line — end current block ----
        bool blank = true;
        for (char ch : line)
            if (!std::isspace(static_cast<unsigned char>(ch))) { blank = false; break; }
        if (blank) {
            close_block();
            continue;
        }

        // ---- blockquote (> …) ----
        if (!line.empty() && line[0] == '>') {
            std::string_view content =
                (line.size() > 1) ? line.substr(line[1] == ' ' ? 2 : 1) : "";
            if (mode == BlockMode::Blockquote) {
                html += "<br>";
            } else {
                close_block();
                html += "<blockquote>";
                mode = BlockMode::Blockquote;
            }
            parse_inline(content, html);
            continue;
        }

        // ---- unordered list (-, +, or * at col 0 followed by space) ----
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '+')
            && line[1] == ' ') {
            std::string_view content = line.substr(2);
            if (mode == BlockMode::UL) {
                html += "</li><li>";
            } else {
                close_block();
                html += "<ul><li>";
                mode = BlockMode::UL;
            }
            parse_inline(content, html);
            continue;
        }

        // ---- ordered list (1. at col 0) ----
        {
            size_t k = 0;
            while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
            if (k > 0 && k < line.size() && line[k] == '.'
                && k + 1 < line.size() && line[k + 1] == ' ') {
                std::string_view content = line.substr(k + 2);
                if (mode == BlockMode::OL) {
                    html += "</li><li>";
                } else {
                    close_block();
                    html += "<ol><li>";
                    mode = BlockMode::OL;
                }
                parse_inline(content, html);
                continue;
            }
        }

        // ---- regular paragraph line ----
        if (mode == BlockMode::Paragraph) {
            html += "<br>";
        } else {
            close_block();
            html += "<p>";
            mode = BlockMode::Paragraph;
        }
        parse_inline(line, html);
    }
    close_block();

    if (!has_formatting_tags(html)) return {body, ""};
    return {body, html};
}

} // namespace tesseract::views
