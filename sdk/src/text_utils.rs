use linkify::{LinkFinder, LinkKind};

pub use crate::ffi::UrlSpan;

/// Remove the bidirectional override and isolate controls (U+202A–U+202E,
/// U+2066–U+2069) from text shown as a name, so a sender cannot reorder the
/// text around it. Right-to-left marks and ordinary bidi text are kept.
pub fn strip_bidi_overrides(s: &str) -> String {
    s.chars()
        .filter(|c| !matches!(*c, '\u{202A}'..='\u{202E}' | '\u{2066}'..='\u{2069}'))
        .collect()
}

/// Escape `&`, `<`, `>` and `"` so `s` can sit in HTML text or a quoted attribute.
pub(crate) fn html_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(c),
        }
    }
    out
}

/// Find all linkable URIs in `text` and return their byte-offset spans.
///
/// Covers two classes:
///   - http(s):// URLs — detected by `linkify`, which handles unicode domains,
///     IDNs, and complex punctuation edge cases correctly.
///   - `matrix:` URIs (MSC2312) — detected with a custom scanner since no
///     published crate covers this scheme.
///
/// Spans are sorted by `start` and guaranteed non-overlapping.  The `url`
/// field of each span is the verbatim URI text (identical to
/// `&text[start..end]`).
pub fn find_url_spans(text: &str) -> Vec<UrlSpan> {
    let mut spans: Vec<UrlSpan> = Vec::new();

    // ── http(s) via linkify ───────────────────────────────────────────────────
    let finder = LinkFinder::new();
    for link in finder.links(text) {
        let s = link.as_str();
        let has_http_scheme = |scheme: &str| {
            s.get(..scheme.len())
                .is_some_and(|p| p.eq_ignore_ascii_case(scheme))
        };
        if link.kind() == &LinkKind::Url
            && (has_http_scheme("https://") || has_http_scheme("http://"))
        {
            spans.push(UrlSpan {
                start: link.start(),
                end: link.end(),
                url: s.to_string(),
            });
        }
    }

    // ── matrix: URIs ─────────────────────────────────────────────────────────
    matrix_uri_spans(text, &mut spans);

    // Sort and deduplicate (linkify and matrix scanner shouldn't overlap, but
    // be defensive in case of any edge case).
    spans.sort_by_key(|s| s.start);
    spans.dedup_by(|b, a| {
        // If b starts before a ends, drop b (it overlaps).
        b.start < a.end
    });

    spans
}

/// Like [`find_url_spans`], but drops any span that falls inside a Markdown
/// code region — a fenced code block (` ``` `/`~~~`, ≥3 backticks/tildes) or
/// an inline code span (matching backtick runs). Used for URL-preview
/// candidate selection, where a URL shown as code (a log line, an example, a
/// config snippet) was not meant as a link to preview.
///
/// Plain autolinking of message bodies into clickable text
/// (`find_url_spans`'s other caller) is unaffected — a bare backtick run in a
/// plain-text (non-Markdown) message isn't a code span there, so narrowing
/// that path too would be a separate, broader behavior change.
pub fn find_preview_url_spans(text: &str) -> Vec<UrlSpan> {
    let code_regions = code_regions(text);
    find_url_spans(text)
        .into_iter()
        .filter(|s| !code_regions.iter().any(|r| s.start >= r.0 && s.end <= r.1))
        .collect()
}

/// Byte ranges of `text` that are Markdown code regions (fenced blocks and
/// inline code spans), as `(start, end)` pairs.
fn code_regions(text: &str) -> Vec<(usize, usize)> {
    let mut regions = Vec::new();

    // ── Fenced code blocks ──────────────────────────────────────────────────
    // Scan line by line; a fence line is one whose first non-space run is
    // ≥3 backticks or ≥3 tildes. A block runs from the fence line's start to
    // the matching closing fence (same char, length ≥ opening), or to the end
    // of `text` if unterminated.
    let mut fence: Option<(u8, usize)> = None; // (fence_char, fence_len)
    let mut fence_start = 0usize;
    let mut pos = 0usize;
    for line in text.split_inclusive('\n') {
        let trimmed = line.trim_start();
        let indent = line.len() - trimmed.len();
        let fence_char = trimmed.as_bytes().first().copied();
        let run_len = match fence_char {
            Some(c @ (b'`' | b'~')) => trimmed.bytes().take_while(|&b| b == c).count(),
            _ => 0,
        };

        match fence {
            None => {
                if run_len >= 3 && indent < 4 {
                    fence = Some((fence_char.unwrap(), run_len));
                    fence_start = pos;
                }
            }
            Some((c, len)) => {
                if fence_char == Some(c) && run_len >= len && indent < 4 {
                    regions.push((fence_start, pos + line.len()));
                    fence = None;
                }
            }
        }

        pos += line.len();
    }
    if fence.is_some() {
        // Unterminated fence: excludes the rest of the text.
        regions.push((fence_start, text.len()));
    }

    // ── Inline code spans ────────────────────────────────────────────────────
    // Skip scanning inside fenced blocks already found above.
    let bytes = text.as_bytes();
    let n = bytes.len();
    let mut i = 0;
    while i < n {
        if regions.iter().any(|r| i >= r.0 && i < r.1) {
            i += 1;
            continue;
        }
        if bytes[i] == b'`' {
            let open_start = i;
            let open_len = {
                let mut j = i;
                while j < n && bytes[j] == b'`' {
                    j += 1;
                }
                j - i
            };
            let mut j = open_start + open_len;
            let mut found_close = None;
            while j < n {
                if bytes[j] == b'`' {
                    let close_start = j;
                    let mut k = j;
                    while k < n && bytes[k] == b'`' {
                        k += 1;
                    }
                    let close_len = k - close_start;
                    if close_len == open_len {
                        found_close = Some(k);
                        break;
                    }
                    j = k;
                } else {
                    j += 1;
                }
            }
            if let Some(end) = found_close {
                regions.push((open_start, end));
                i = end;
                continue;
            }
            // No matching close: not a code span, just literal backticks.
            i = open_start + open_len;
            continue;
        }
        i += 1;
    }

    regions.sort_by_key(|r| r.0);
    regions
}

// ── matrix: URI scanner ───────────────────────────────────────────────────────

const MATRIX_SCHEME: &str = "matrix:";

/// Scan `text` for `matrix:` URIs and push found spans into `out`.
fn matrix_uri_spans(text: &str, out: &mut Vec<UrlSpan>) {
    let bytes = text.as_bytes();
    let scheme = MATRIX_SCHEME.as_bytes();
    let n = bytes.len();
    let slen = scheme.len();

    let mut i = 0;
    while i + slen <= n {
        // Fast check: look for the 'm' of "matrix:".
        if bytes[i] != b'm' || bytes[i..].len() < slen || &bytes[i..i + slen] != scheme {
            i += 1;
            continue;
        }

        // Word-boundary guard: char before 'm' must not be alphanumeric.
        if i > 0 && bytes[i - 1].is_ascii_alphanumeric() {
            i += 1;
            continue;
        }

        // Measure the URI extent: run until whitespace or end-of-string.
        let start = i;
        let mut end = i + slen;
        while end < n && !bytes[end].is_ascii_whitespace() {
            end += 1;
        }

        // Require at least one non-scheme character.
        if end == i + slen {
            i += 1;
            continue;
        }

        // Strip trailing prose punctuation (mirrors the http(s) rules).
        while end > i + slen {
            match bytes[end - 1] {
                b'.' | b',' | b':' | b';' | b'!' | b'?' | b')' | b']' => end -= 1,
                _ => break,
            }
        }

        let uri = &text[start..end];

        // Basic sanity: must have a known segment after "matrix:".
        let rest = &uri[slen..];
        let segment = rest
            .split('/')
            .next()
            .unwrap_or("")
            .split('?')
            .next()
            .unwrap_or("");
        let valid = matches!(segment, "u" | "r" | "roomid" | "e");

        if valid {
            out.push(UrlSpan {
                start,
                end,
                url: uri.to_string(),
            });
        }

        i = end;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn spans(text: &str) -> Vec<(usize, usize, &str)> {
        find_url_spans(text)
            .iter()
            .map(|s| (s.start, s.end, text[s.start..s.end].into()))
            .collect()
    }

    #[test]
    fn no_urls_returns_empty() {
        assert!(find_url_spans("just some text, no link").is_empty());
        assert!(find_url_spans("").is_empty());
        assert!(find_url_spans("ftp://example.org not http").is_empty());
    }

    #[test]
    fn https_url_detected() {
        let s = find_url_spans("https://example.org/path");
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].url, "https://example.org/path");
    }

    #[test]
    fn http_url_in_sentence() {
        let s = spans("see http://example.org now");
        assert_eq!(s, vec![(4, 22, "http://example.org")]);
    }

    #[test]
    fn matrix_uri_user() {
        let s = find_url_spans("join matrix:u/alice:example.org today");
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].url, "matrix:u/alice:example.org");
    }

    #[test]
    fn matrix_uri_room_alias() {
        let s = find_url_spans("try matrix:r/general:example.org");
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].url, "matrix:r/general:example.org");
    }

    #[test]
    fn matrix_uri_room_id() {
        let s = find_url_spans("matrix:roomid/abc123:example.com");
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].url, "matrix:roomid/abc123:example.com");
    }

    #[test]
    fn matrix_uri_word_boundary_guard() {
        // Preceded by alphanumeric → not a link.
        assert!(find_url_spans("xmatrix:r/general:example.org").is_empty());
        assert!(find_url_spans("1matrix:r/general:example.org").is_empty());
    }

    #[test]
    fn matrix_uri_trailing_punctuation_stripped() {
        let s = find_url_spans("matrix:r/general:example.org.");
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].url, "matrix:r/general:example.org");
    }

    #[test]
    fn matrix_uri_unknown_segment_ignored() {
        assert!(find_url_spans("matrix:bogus/whatever").is_empty());
    }

    #[test]
    fn mixed_http_and_matrix() {
        let text = "see https://example.org and matrix:u/alice:example.org";
        let s = find_url_spans(text);
        assert_eq!(s.len(), 2);
        assert_eq!(s[0].url, "https://example.org");
        assert_eq!(s[1].url, "matrix:u/alice:example.org");
        assert!(s[0].start < s[1].start);
    }
}

#[cfg(test)]
mod preview_span_tests {
    use super::*;

    fn preview_urls(text: &str) -> Vec<&str> {
        find_preview_url_spans(text)
            .iter()
            .map(|s| &text[s.start..s.end])
            .collect()
    }

    #[test]
    fn url_in_fenced_block_excluded() {
        let text = "see\n```\nhttp://example.com\n```\nthanks";
        assert!(preview_urls(text).is_empty());
    }

    #[test]
    fn url_in_tilde_fenced_block_excluded() {
        let text = "see\n~~~\nhttp://example.com\n~~~\nthanks";
        assert!(preview_urls(text).is_empty());
    }

    #[test]
    fn url_in_inline_code_excluded() {
        let text = "try `http://example.com` sometime";
        assert!(preview_urls(text).is_empty());
    }

    #[test]
    fn url_outside_code_region_kept() {
        let text = "see http://example.org now";
        assert_eq!(preview_urls(text), vec!["http://example.org"]);
    }

    #[test]
    fn unterminated_fence_excludes_rest_of_text() {
        let text = "intro\n```\nhttp://example.com\nmore http://example.org";
        assert!(preview_urls(text).is_empty());
    }

    #[test]
    fn mixed_code_and_prose_keeps_only_prose_url() {
        let text = "see https://example.org but not ```\nhttp://example.com\n```";
        assert_eq!(preview_urls(text), vec!["https://example.org"]);
    }

    #[test]
    fn matrix_uri_in_fenced_block_excluded() {
        let text = "```\nmatrix:u/alice:example.org\n```";
        assert!(preview_urls(text).is_empty());
    }
}

#[cfg(test)]
mod bidi_tests {
    use super::strip_bidi_overrides;

    #[test]
    fn removes_overrides_and_isolates_but_keeps_text() {
        // "invoice" followed by an RLO that would draw "fdp.exe" reversed.
        assert_eq!(
            strip_bidi_overrides("invoice\u{202E}fdp.exe"),
            "invoicefdp.exe"
        );
        assert_eq!(strip_bidi_overrides("a\u{2066}b\u{2069}c"), "abc");
        // Right-to-left marks and plain Arabic/Hebrew text are unchanged.
        assert_eq!(
            strip_bidi_overrides("\u{200F}שלום مرحبا"),
            "\u{200F}שלום مرحبا"
        );
    }
}
