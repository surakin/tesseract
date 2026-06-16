use linkify::{LinkFinder, LinkKind};

pub use crate::ffi::UrlSpan;

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
        if link.kind() == &LinkKind::Url
            && (s.starts_with("https://") || s.starts_with("http://"))
        {
            spans.push(UrlSpan {
                start: link.start(),
                end:   link.end(),
                url:   s.to_string(),
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
        let segment = rest.split('/').next().unwrap_or("").split('?').next().unwrap_or("");
        let valid = matches!(segment, "u" | "r" | "roomid" | "e");

        if valid {
            out.push(UrlSpan { start, end, url: uri.to_string() });
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
