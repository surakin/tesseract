//! OpenGraph / Twitter-card / plain-HTML metadata extraction for sender-side
//! MSC4095 bundled URL previews fetched directly by Tesseract (the
//! "fetch previews directly" opt-in). Pure functions over bytes/strings so
//! they're unit-testable in isolation; the guarded network fetch lives in
//! `client::url_preview_gen`.
//!
//! Parsing uses `scraper` (html5ever, the same parser ruma-html already pulls
//! in) so malformed markup is recovered the way a browser would.

use scraper::{Html, Selector};
use std::collections::HashMap;
use url::Url;

/// Cap on the title length (in chars) carried in a bundled preview.
pub(crate) const MAX_TITLE_CHARS: usize = 300;
/// Cap on the description length (in chars) carried in a bundled preview.
pub(crate) const MAX_DESCRIPTION_CHARS: usize = 1000;
/// How far into the document the `<meta charset>` prescan looks, matching
/// the HTML spec's encoding-sniffing window.
const CHARSET_PRESCAN_BYTES: usize = 1024;

/// Preview metadata extracted from one HTML page.
#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct PageMeta {
    pub title: Option<String>,
    pub description: Option<String>,
    /// `og:url`, resolved and http(s)-only.
    pub canonical_url: Option<String>,
    /// Absolute http(s) image URL (resolved against `<base href>` or the
    /// page URL).
    pub image_url: Option<Url>,
    pub image_width: Option<u32>,
    pub image_height: Option<u32>,
}

impl PageMeta {
    fn has_content(&self) -> bool {
        self.title.is_some() || self.description.is_some() || self.image_url.is_some()
    }
}

/// True when a `Content-Type` header value denotes an HTML document.
pub(crate) fn is_html_content_type(content_type: &str) -> bool {
    let essence = content_type
        .split(';')
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    essence == "text/html" || essence == "application/xhtml+xml"
}

/// Decode an HTML response body to a `String`. Encoding precedence follows
/// the HTML spec's sniffing order: byte-order mark, then the `Content-Type`
/// header's `charset`, then a `<meta charset>` / `http-equiv` prescan of the
/// first 1024 bytes, then UTF-8. Undecodable bytes become U+FFFD.
pub(crate) fn decode_html(bytes: &[u8], content_type: Option<&str>) -> String {
    let encoding = encoding_rs::Encoding::for_bom(bytes)
        .map(|(enc, _)| enc)
        .or_else(|| {
            content_type
                .and_then(charset_param)
                .and_then(label_to_encoding)
        })
        .or_else(|| prescan_meta_charset(bytes).and_then(label_to_encoding))
        .unwrap_or(encoding_rs::UTF_8);
    // `decode` re-checks the BOM itself and strips it.
    let (text, _, _) = encoding.decode(bytes);
    text.into_owned()
}

fn label_to_encoding(label: &str) -> Option<&'static encoding_rs::Encoding> {
    let enc = encoding_rs::Encoding::for_label(label.trim().as_bytes())?;
    // A UTF-16 label outside a BOM is treated as UTF-8 (HTML spec) — the
    // label was necessarily read from ASCII-compatible bytes.
    if enc == encoding_rs::UTF_16LE || enc == encoding_rs::UTF_16BE {
        Some(encoding_rs::UTF_8)
    } else {
        Some(enc)
    }
}

/// `charset` parameter of a `Content-Type`-style value, unquoted.
fn charset_param(value: &str) -> Option<&str> {
    value.split(';').skip(1).find_map(|p| {
        let (k, v) = p.split_once('=')?;
        k.trim()
            .eq_ignore_ascii_case("charset")
            .then(|| v.trim().trim_matches(|c| c == '"' || c == '\''))
            .filter(|v| !v.is_empty())
    })
}

/// Cheap prescan for `charset=` inside the document's first 1024 bytes. Good
/// enough for `<meta charset="x">` and `<meta http-equiv="Content-Type"
/// content="text/html; charset=x">` — both put the label right after
/// `charset=`.
fn prescan_meta_charset(bytes: &[u8]) -> Option<&str> {
    let head = &bytes[..bytes.len().min(CHARSET_PRESCAN_BYTES)];
    let lower = head.to_ascii_lowercase();
    let needle = b"charset";
    let mut from = 0;
    while let Some(pos) = find(&lower[from..], needle) {
        let mut i = from + pos + needle.len();
        from = i;
        while i < head.len() && head[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= head.len() || head[i] != b'=' {
            continue;
        }
        i += 1;
        while i < head.len()
            && (head[i].is_ascii_whitespace() || head[i] == b'"' || head[i] == b'\'')
        {
            i += 1;
        }
        let start = i;
        while i < head.len() && (head[i].is_ascii_alphanumeric() || b"-_:.".contains(&head[i])) {
            i += 1;
        }
        if i > start {
            return std::str::from_utf8(&head[start..i]).ok();
        }
    }
    None
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// Collapse runs of whitespace, trim, and cap at `max_chars` (on a char
/// boundary, with an ellipsis when truncated). `None` when nothing is left.
pub(crate) fn clean_text(raw: &str, max_chars: usize) -> Option<String> {
    let collapsed = raw.split_whitespace().collect::<Vec<_>>().join(" ");
    if collapsed.is_empty() {
        return None;
    }
    if collapsed.chars().count() <= max_chars {
        return Some(collapsed);
    }
    let mut out: String = collapsed
        .chars()
        .take(max_chars.saturating_sub(1))
        .collect();
    out.push('…');
    Some(out)
}

/// Resolve `raw` against `base` and keep it only if the result is http(s).
fn resolve_http_url(base: &Url, raw: &str) -> Option<Url> {
    let raw = raw.trim();
    if raw.is_empty() {
        return None;
    }
    let url = base.join(raw).ok()?;
    matches!(url.scheme(), "http" | "https").then_some(url)
}

/// Extract preview metadata from `html`, a page fetched from `page_url` (the
/// final URL after redirects). Field precedence: OpenGraph, then Twitter
/// card, then plain `<title>` / `<meta name="description">`. Returns `None`
/// when the page has no title, description or image at all.
pub(crate) fn extract(html: &str, page_url: &Url) -> Option<PageMeta> {
    let doc = Html::parse_document(html);

    // First occurrence wins for every key, so `og:image:width` pairs with
    // the first `og:image` (OpenGraph's structured-property convention).
    let meta_sel = Selector::parse("meta[content]").ok()?;
    let mut tags: HashMap<String, String> = HashMap::new();
    for el in doc.select(&meta_sel) {
        let v = el.value();
        let Some(key) = v.attr("property").or_else(|| v.attr("name")) else {
            continue;
        };
        let key = key.trim().to_ascii_lowercase();
        if key.is_empty() {
            continue;
        }
        if let Some(content) = v.attr("content") {
            tags.entry(key).or_insert_with(|| content.to_owned());
        }
    }
    let tag = |keys: &[&str]| -> Option<&str> {
        keys.iter()
            .filter_map(|k| tags.get(*k))
            .map(|s| s.as_str())
            .find(|s| !s.trim().is_empty())
    };

    let base = Selector::parse("base[href]")
        .ok()
        .and_then(|sel| doc.select(&sel).next())
        .and_then(|el| el.value().attr("href"))
        .and_then(|href| resolve_http_url(page_url, href))
        .unwrap_or_else(|| page_url.clone());

    let html_title = ["head > title", "title"].iter().find_map(|s| {
        let sel = Selector::parse(s).ok()?;
        let text: String = doc.select(&sel).next()?.text().collect();
        clean_text(&text, MAX_TITLE_CHARS)
    });

    let title = tag(&["og:title", "twitter:title"])
        .and_then(|s| clean_text(s, MAX_TITLE_CHARS))
        .or(html_title)
        .or_else(|| tag(&["og:site_name"]).and_then(|s| clean_text(s, MAX_TITLE_CHARS)));
    let description = tag(&["og:description", "twitter:description", "description"])
        .and_then(|s| clean_text(s, MAX_DESCRIPTION_CHARS));
    let canonical_url = tag(&["og:url"])
        .and_then(|s| resolve_http_url(&base, s))
        .map(|u| u.to_string());

    // Prefer the https `secure_url` variant; fall back through the plain
    // OpenGraph and Twitter-card forms.
    let image_url = tag(&["og:image:secure_url"])
        .and_then(|s| resolve_http_url(&base, s))
        .filter(|u| u.scheme() == "https")
        .or_else(|| {
            tag(&[
                "og:image",
                "og:image:url",
                "twitter:image",
                "twitter:image:src",
            ])
            .and_then(|s| resolve_http_url(&base, s))
        });
    let dim = |key: &str| {
        tag(&[key])
            .and_then(|s| s.trim().parse::<u32>().ok())
            .filter(|&d| d > 0)
    };
    let (image_width, image_height) = if image_url.is_some() {
        (dim("og:image:width"), dim("og:image:height"))
    } else {
        (None, None)
    };

    let meta = PageMeta {
        title,
        description,
        canonical_url,
        image_url,
        image_width,
        image_height,
    };
    meta.has_content().then_some(meta)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn page() -> Url {
        Url::parse("https://example.com/articles/post.html").unwrap()
    }

    #[test]
    fn extracts_full_opengraph() {
        let html = r#"<html><head>
            <title>Fallback title</title>
            <meta property="og:title" content="OG Title">
            <meta property="og:description" content="OG   description
                spanning lines">
            <meta property="og:url" content="https://example.com/canonical">
            <meta property="og:image" content="https://cdn.example.com/a.png">
            <meta property="og:image:width" content="800">
            <meta property="og:image:height" content="400">
            </head><body></body></html>"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(m.title.as_deref(), Some("OG Title"));
        assert_eq!(
            m.description.as_deref(),
            Some("OG description spanning lines")
        );
        assert_eq!(
            m.canonical_url.as_deref(),
            Some("https://example.com/canonical")
        );
        assert_eq!(
            m.image_url.unwrap().as_str(),
            "https://cdn.example.com/a.png"
        );
        assert_eq!((m.image_width, m.image_height), (Some(800), Some(400)));
    }

    #[test]
    fn falls_back_to_twitter_then_plain_html() {
        let html = r#"<head>
            <meta name="twitter:title" content="TW Title">
            <meta name="description" content="Plain description">
            <meta name="twitter:image:src" content="/img/t.jpg">
            <title>Plain</title></head>"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(m.title.as_deref(), Some("TW Title"));
        assert_eq!(m.description.as_deref(), Some("Plain description"));
        assert_eq!(
            m.image_url.unwrap().as_str(),
            "https://example.com/img/t.jpg"
        );

        let only_title = "<html><head><title>  Just a   title </title></head></html>";
        let m = extract(only_title, &page()).unwrap();
        assert_eq!(m.title.as_deref(), Some("Just a title"));
        assert_eq!(m.description, None);
    }

    #[test]
    fn site_name_is_last_resort_title() {
        let html = r#"<meta property="og:site_name" content="Example Site">"#;
        assert_eq!(
            extract(html, &page()).unwrap().title.as_deref(),
            Some("Example Site")
        );
    }

    #[test]
    fn resolves_relative_image_against_page_url() {
        let html = r#"<meta property="og:image" content="../images/x.png">"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(
            m.image_url.unwrap().as_str(),
            "https://example.com/images/x.png"
        );
    }

    #[test]
    fn resolves_relative_image_against_base_href() {
        let html = r#"<head><base href="https://static.example.org/assets/">
            <meta property="og:image" content="x.png"></head>"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(
            m.image_url.unwrap().as_str(),
            "https://static.example.org/assets/x.png"
        );
    }

    #[test]
    fn prefers_https_secure_url_and_drops_non_http_images() {
        let html = r#"<meta property="og:image" content="http://e.com/p.png">
            <meta property="og:image:secure_url" content="https://e.com/s.png">"#;
        assert_eq!(
            extract(html, &page()).unwrap().image_url.unwrap().as_str(),
            "https://e.com/s.png"
        );

        let html = r#"<meta property="og:title" content="T">
            <meta property="og:image" content="javascript:alert(1)">
            <meta property="og:image:width" content="10">"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(m.image_url, None);
        assert_eq!(m.image_width, None);

        let html = r#"<meta property="og:image" content="data:image/png;base64,AAAA">"#;
        assert_eq!(extract(html, &page()), None);
    }

    #[test]
    fn first_occurrence_wins_and_bad_dimensions_are_ignored() {
        let html = r#"<meta property="og:title" content="First">
            <meta property="og:title" content="Second">
            <meta property="og:image" content="/a.png">
            <meta property="og:image:width" content="wide">
            <meta property="og:image:height" content="0">"#;
        let m = extract(html, &page()).unwrap();
        assert_eq!(m.title.as_deref(), Some("First"));
        assert_eq!((m.image_width, m.image_height), (None, None));
    }

    #[test]
    fn caps_long_text() {
        let long = "a".repeat(5000);
        let html = format!(
            r#"<meta property="og:title" content="{long}">
            <meta property="og:description" content="{long}">"#
        );
        let m = extract(&html, &page()).unwrap();
        assert_eq!(m.title.unwrap().chars().count(), MAX_TITLE_CHARS);
        assert_eq!(
            m.description.unwrap().chars().count(),
            MAX_DESCRIPTION_CHARS
        );
    }

    #[test]
    fn empty_or_contentless_page_yields_none() {
        assert_eq!(extract("", &page()), None);
        assert_eq!(
            extract("<html><body><p>hi</p></body></html>", &page()),
            None
        );
        assert_eq!(
            extract(r#"<meta property="og:title" content="   ">"#, &page()),
            None
        );
    }

    #[test]
    fn survives_malformed_html() {
        let html = r#"<html><head><meta property="og:title" content="Broken"
            <title>unclosed <b>markup<meta property="og:description" content="Still here">"#;
        let m = extract(html, &page()).unwrap();
        assert!(m.title.is_some() || m.description.is_some());
    }

    #[test]
    fn content_type_detection() {
        assert!(is_html_content_type("text/html"));
        assert!(is_html_content_type("Text/HTML; charset=utf-8"));
        assert!(is_html_content_type("application/xhtml+xml"));
        assert!(!is_html_content_type("image/png"));
        assert!(!is_html_content_type("application/json"));
    }

    #[test]
    fn decodes_using_header_charset() {
        // "café" in ISO-8859-1.
        let bytes = b"<title>caf\xe9</title>";
        assert_eq!(
            decode_html(bytes, Some("text/html; charset=ISO-8859-1")),
            "<title>café</title>"
        );
    }

    #[test]
    fn decodes_using_meta_charset_prescan() {
        let bytes = b"<html><head><meta charset=\"windows-1252\"><title>caf\xe9 \x80</title>";
        assert!(decode_html(bytes, Some("text/html")).contains("café €"));

        let bytes =
            b"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\">\xe9";
        assert!(decode_html(bytes, None).ends_with('é'));
    }

    #[test]
    fn bom_beats_header_and_defaults_to_utf8() {
        let bytes = b"\xef\xbb\xbfcaf\xc3\xa9";
        assert_eq!(
            decode_html(bytes, Some("text/html; charset=iso-8859-1")),
            "café"
        );
        assert_eq!(decode_html("naïve".as_bytes(), None), "naïve");
        // Meta-declared UTF-16 without a BOM is read as UTF-8.
        let bytes = "<meta charset=\"utf-16\">é".as_bytes();
        assert!(decode_html(bytes, None).ends_with('é'));
    }
}
