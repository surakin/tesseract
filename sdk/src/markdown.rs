//! Markdown-to-HTML conversion backed by pulldown-cmark (enabled via the
//! matrix-sdk "markdown" feature which cascades to ruma/markdown).
//!
//! `markdown_to_html`        — full block + inline conversion; returns empty
//!                             `formatted_body` when no markdown is detected
//!                             (caller should use the plain-text body as-is).
//! `markdown_inline_to_html` — inline-only (no outer <p>), always returns
//!                             HTML-escaped output so callers can embed it
//!                             inside HTML elements (e.g. spoiler spans).
//!
//! Behavioural notes vs. the former C++ parser:
//!  • Inline/block HTML events are converted to escaped Text so raw HTML
//!    tags typed by the user (e.g. <script>) never reach the wire unescaped.
//!  • Code-fence info strings are lowercased and restricted to
//!    [a-zA-Z0-9+#._-] characters (same as the old sanitize_fence_lang).
//!  • U+2028 (QTextEdit Shift+Enter) is normalised to U+000A before parsing.
//!  • Link/image URLs are restricted to http(s); unsafe schemes are stripped.

use pulldown_cmark::{CodeBlockKind, CowStr, Event, Options, Parser, Tag, TagEnd};

const OPTIONS: Options = Options::ENABLE_TABLES.union(Options::ENABLE_STRIKETHROUGH);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn is_safe_url(url: &str) -> bool {
    url.is_empty() || url.starts_with("https://") || url.starts_with("http://")
}

fn is_block_tag(tag: &Tag<'_>) -> bool {
    matches!(
        tag,
        Tag::Paragraph
            | Tag::Heading { .. }
            | Tag::BlockQuote(_)
            | Tag::CodeBlock(_)
            | Tag::HtmlBlock
            | Tag::List(_)
            | Tag::FootnoteDefinition(_)
            | Tag::Table(_)
    )
}

/// Lowercase and restrict the fenced-code info string to the first
/// whitespace-separated token, keeping only `[a-zA-Z0-9+#._-]` characters.
/// Matches the old C++ `sanitize_fence_lang` behaviour.
fn sanitize_fence_lang(info: &str) -> String {
    let first_token = info.split_ascii_whitespace().next().unwrap_or("");
    first_token
        .chars()
        .take_while(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '#' | '.' | '-' | '_'))
        .map(|c| c.to_ascii_lowercase())
        .collect()
}

/// Normalise U+2028 LINE SEPARATOR → U+000A NEWLINE.  QTextEdit inserts
/// U+2028 for Shift+Enter; without this pulldown-cmark does not recognise
/// fenced code-block delimiters that span those positions.
fn preprocess(text: &str) -> std::borrow::Cow<'_, str> {
    if text.contains('\u{2028}') {
        std::borrow::Cow::Owned(text.replace('\u{2028}', "\n"))
    } else {
        std::borrow::Cow::Borrowed(text)
    }
}

/// Apply all event-level transformations before rendering to HTML:
///  1. Convert InlineHtml / Html events → Text (escapes raw HTML).
///  2. Sanitize fenced code-block info strings.
///  3. Drop links/images with non-http(s) destination URLs.
fn sanitize_events<'a>(events: Vec<Event<'a>>) -> Vec<Event<'a>> {
    let mut out = Vec::with_capacity(events.len());
    let mut skip_link = false;
    let mut skip_image = false;

    for event in events {
        let transformed = match event {
            // Raw HTML → escaped text (prevents XSS via user-typed tags).
            Event::InlineHtml(html) | Event::Html(html) => {
                Some(Event::Text(html))
            }

            // Sanitize fenced code-block language token.
            Event::Start(Tag::CodeBlock(CodeBlockKind::Fenced(ref info))) => {
                let sanitized = sanitize_fence_lang(info);
                Some(Event::Start(Tag::CodeBlock(CodeBlockKind::Fenced(
                    CowStr::Boxed(sanitized.into_boxed_str()),
                ))))
            }

            // Drop links/images with unsafe URL schemes.
            Event::Start(Tag::Link { ref dest_url, .. }) if !is_safe_url(dest_url) => {
                skip_link = true;
                None
            }
            Event::End(TagEnd::Link) if skip_link => {
                skip_link = false;
                None
            }
            Event::Start(Tag::Image { ref dest_url, .. }) if !is_safe_url(dest_url) => {
                skip_image = true;
                None
            }
            Event::End(TagEnd::Image) if skip_image => {
                skip_image = false;
                None
            }

            other => Some(other),
        };
        if let Some(e) = transformed {
            out.push(e);
        }
    }
    out
}

fn events_to_html(events: Vec<Event<'_>>) -> String {
    let events = sanitize_events(events);
    let mut html = String::new();
    pulldown_cmark::html::push_html(&mut html, events.into_iter());
    html
}

// ---------------------------------------------------------------------------
// Core parsers
// ---------------------------------------------------------------------------

/// Full block + inline conversion.  Returns `None` when no markdown syntax is
/// detected (mirrors ruma's `parse_markdown` detection logic).
fn parse_block(text: &str) -> Option<String> {
    let text = preprocess(text);
    let text: &str = &text;

    let parser_events: Vec<_> = Parser::new_ext(text, OPTIONS)
        .map(|e| if e == Event::SoftBreak { Event::HardBreak } else { e })
        .collect();

    // Detect whether the content is a single inline paragraph or multi-block.
    let first_is_para =
        parser_events.first().is_some_and(|e| matches!(e, Event::Start(Tag::Paragraph)));
    let last_is_para =
        parser_events.last().is_some_and(|e| matches!(e, Event::End(TagEnd::Paragraph)));
    let mut is_inline = first_is_para && last_is_para;
    let mut has_markdown = !is_inline;

    if !has_markdown {
        let mut pos = 0;
        for event in parser_events.iter().skip(1) {
            match event {
                Event::Text(s) if text[pos..].starts_with(s.as_ref()) => {
                    pos += s.len();
                    continue;
                }
                Event::HardBreak => {
                    if text[pos..].starts_with("\r\n") {
                        pos += 2;
                        continue;
                    } else if text[pos..].starts_with(['\r', '\n']) {
                        pos += 1;
                        continue;
                    }
                }
                Event::End(TagEnd::Paragraph) => continue,
                Event::Start(tag) => {
                    is_inline &= !is_block_tag(tag);
                }
                _ => {}
            }
            has_markdown = true;
            if !is_inline {
                break;
            }
        }
        has_markdown |= pos != text.len();
    }

    if !has_markdown {
        return None;
    }

    // Strip the outer <p>…</p> for single-paragraph (inline) content as the
    // Matrix spec requires.
    let events = if is_inline {
        let mut evs = parser_events;
        if matches!(evs.first(), Some(Event::Start(Tag::Paragraph))) {
            evs.remove(0);
        }
        if matches!(evs.last(), Some(Event::End(TagEnd::Paragraph))) {
            evs.pop();
        }
        evs
    } else {
        parser_events
    };

    Some(events_to_html(events))
}

/// Inline-only conversion — always produces HTML output (even plain text is
/// HTML-escaped), outer `<p>` is stripped.
fn render_inline(text: &str) -> String {
    let text = preprocess(text);
    let text: &str = &text;

    let mut events: Vec<_> = Parser::new_ext(text, OPTIONS)
        .map(|e| if e == Event::SoftBreak { Event::HardBreak } else { e })
        .collect();

    // Strip the wrapping paragraph.
    if matches!(events.first(), Some(Event::Start(Tag::Paragraph))) {
        events.remove(0);
    }
    if matches!(events.last(), Some(Event::End(TagEnd::Paragraph))) {
        events.pop();
    }

    events_to_html(events)
}

// ---------------------------------------------------------------------------
// Public FFI entry points
// ---------------------------------------------------------------------------

pub fn markdown_to_html(text: &str) -> crate::ffi::MarkdownFfiResult {
    let formatted_body = parse_block(text).unwrap_or_default();
    crate::ffi::MarkdownFfiResult { formatted_body }
}

pub fn markdown_inline_to_html(text: &str) -> String {
    if text.is_empty() {
        return String::new();
    }
    render_inline(text)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn to_html(text: &str) -> String {
        markdown_to_html(text).formatted_body
    }
    fn inline(text: &str) -> String {
        markdown_inline_to_html(text)
    }

    #[test]
    fn plain_text_produces_no_html() {
        assert_eq!(to_html("hello world"), "");
    }

    #[test]
    fn bold_detected() {
        assert!(to_html("**bold**").contains("<strong>"));
    }

    #[test]
    fn italic_detected() {
        assert!(to_html("*italic*").contains("<em>"));
    }

    #[test]
    fn strikethrough_detected() {
        assert!(to_html("~~strike~~").contains("<del>"));
    }

    #[test]
    fn code_span_detected() {
        assert!(to_html("`code`").contains("<code>"));
    }

    #[test]
    fn fenced_code_block() {
        let html = to_html("```rust\nfn main() {}\n```");
        assert!(html.contains("<code"));
        assert!(html.contains("fn main"));
    }

    #[test]
    fn safe_link_passes_through() {
        let html = to_html("[click](https://example.com)");
        assert!(html.contains("href=\"https://example.com\""));
    }

    #[test]
    fn javascript_link_stripped() {
        let html = to_html("[evil](javascript:alert(1))");
        assert!(!html.contains("href="));
        assert!(!html.contains("javascript:"));
        assert!(html.contains("evil"));
    }

    #[test]
    fn data_uri_link_stripped() {
        let html = to_html("[img](data:image/png;base64,abc)");
        assert!(!html.contains("href="));
        assert!(!html.contains("data:"));
    }

    #[test]
    fn inline_always_returns_html_for_plain_text() {
        assert_eq!(inline("hello"), "hello");
    }

    #[test]
    fn inline_no_outer_paragraph() {
        let html = inline("**bold**");
        assert!(!html.contains("<p>"));
        assert!(html.contains("<strong>"));
    }

    #[test]
    fn inline_empty_returns_empty() {
        assert_eq!(inline(""), "");
    }

    #[test]
    fn raw_html_tags_are_escaped() {
        // InlineHtml events are converted to Text so they get HTML-escaped,
        // matching the old C++ parser behaviour.
        let html = to_html("**<script>alert(1)</script>**");
        assert!(html.contains("&lt;script&gt;"));
        assert!(!html.contains("<script>"));
    }

    #[test]
    fn blockquote_detected() {
        assert!(to_html("> quote").contains("<blockquote>"));
    }

    #[test]
    fn unordered_list_detected() {
        assert!(to_html("- item").contains("<ul>"));
    }

    #[test]
    fn ordered_list_detected() {
        assert!(to_html("1. item").contains("<ol>"));
    }

    #[test]
    fn table_detected() {
        let md = "| a | b |\n|---|---|\n| 1 | 2 |";
        assert!(to_html(md).contains("<table>"));
    }

    #[test]
    fn fence_lang_lowercased() {
        let html = to_html("```Python\nx = 1\n```");
        assert!(html.contains("language-python"));
        assert!(!html.contains("language-Python"));
    }

    #[test]
    fn fence_lang_first_token_only() {
        let html = to_html("```Python title=foo\nx = 1\n```");
        assert!(html.contains("language-python"));
        assert!(!html.contains("title"));
    }

    #[test]
    fn fence_lang_injection_sanitized() {
        let html = to_html("```js\"><img src=x>\nx\n```");
        assert!(html.contains("language-js\""));
        assert!(!html.contains("<img"));
    }

    #[test]
    fn u2028_treated_as_newline() {
        let input = "```rust\u{2028}fn main() {}\u{2028}```";
        let html = to_html(input);
        assert!(html.contains("language-rust"));
        assert!(html.contains("fn main"));
    }

    #[test]
    fn sanitize_fence_lang_lowercases() {
        assert_eq!(sanitize_fence_lang("Python"), "python");
        assert_eq!(sanitize_fence_lang("RUST"), "rust");
        assert_eq!(sanitize_fence_lang("c++"), "c++");
    }

    #[test]
    fn sanitize_fence_lang_first_token() {
        assert_eq!(sanitize_fence_lang("python title=foo"), "python");
    }

    #[test]
    fn sanitize_fence_lang_stops_at_unsafe_chars() {
        assert_eq!(sanitize_fence_lang("js\"><img"), "js");
    }
}
