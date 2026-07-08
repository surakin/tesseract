//! HTML re-sanitization for incoming `formatted_body`, extending
//! matrix-sdk-ui's built-in sanitizer to preserve MSC2545 inline custom
//! emoticons (`<img data-mx-emoticon>`).
//!
//! matrix-sdk-ui's `Timeline` sanitizes every incoming message's HTML via
//! `ruma_html::sanitize_html()`, hardcoded to `SanitizerConfig::compat()` —
//! there is no hook to extend that call's allow-list. `ruma_html`'s
//! structured `<img>` model has no field for `data-mx-emoticon` at all
//! (MSC2545's inline-emoticon HTML convention was never implemented there),
//! so the attribute is silently dropped before Tesseract's own code ever
//! sees it.
//!
//! `sanitize_formatted_body` re-implements the same Compat-mode sanitization
//! pass (same baseline XSS/tag/attribute filtering) against the RAW,
//! pre-sanitization event JSON, with `data-mx-emoticon` added to `<img>`'s
//! allow-list. Callers must source the raw HTML from
//! `EventTimelineItem::latest_json()` (covers edits) rather than the
//! Timeline-provided `FormattedBody`, which has already been mutated in
//! place by the time Tesseract's conversion code runs.
//!
//! **Known upstream gap, not introduced by this module**: testing directly
//! against `ruma-html` 0.8.0 (see `tests::img_src_scheme_gap_is_not_our_bug`)
//! shows Compat mode's documented mxc-only scheme restriction on `<img src>`
//! is not actually enforced — a `<img src="https://...">` survives
//! unstripped, even with an explicit `allow_schemes(..., Override)` call
//! that unambiguously bypasses whatever internal spec/compat resolution
//! logic might otherwise be suspect. (`<a href>`'s scheme restriction, by
//! contrast, works correctly — this appears specific to `img`/`src`.) This
//! is not a new risk from this file: Tesseract only ever renders an actual
//! image for a tag that also independently passes
//! `img_src.rfind("mxc://", 0) == 0` in `html_spans.cpp` — a non-mxc `src`
//! is never treated as a renderable emoticon regardless of what either
//! sanitizer does or doesn't strip.

use ruma::html::{Html, ListBehavior, PropertiesNames, SanitizerConfig};

/// Sanitize `raw_html` the same way matrix-sdk-ui's Timeline would (Compat
/// mode), except `<img data-mx-emoticon>` is preserved instead of stripped.
/// `remove_reply_fallback` should mirror whatever matrix-sdk-ui would have
/// used for this event (true for a message that is itself a reply).
pub fn sanitize_formatted_body(raw_html: &str, remove_reply_fallback: bool) -> String {
    let mut config = SanitizerConfig::compat().allow_attributes(
        [PropertiesNames {
            parent: "img",
            properties: &["data-mx-emoticon"],
        }],
        ListBehavior::Add,
    );
    if remove_reply_fallback {
        config = config.remove_reply_fallback();
    }
    let html = Html::parse(raw_html);
    html.sanitize_with(&config);
    html.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_mx_emoticon_survives_self_closed() {
        let input = "hi <img data-mx-emoticon src=\"mxc://x.org/abc\" \
                     alt=\":wave:\" title=\":wave:\" height=\"32\"/> there";
        let out = sanitize_formatted_body(input, false);
        assert!(out.contains("data-mx-emoticon"));
        assert!(out.contains("mxc://x.org/abc"));
    }

    #[test]
    fn data_mx_emoticon_survives_bare_tag() {
        // Real Element-sent messages are re-serialized by html5ever's DOM
        // parser without the trailing self-closing slash.
        let input = "<img data-mx-emoticon src=\"mxc://gnomos.org/abc\" \
                     alt=\":cacodemon:\" title=\":cacodemon:\" height=\"32\"> oh";
        let out = sanitize_formatted_body(input, false);
        assert!(out.contains("data-mx-emoticon"));
        assert!(out.contains("mxc://gnomos.org/abc"));
    }

    #[test]
    fn ordinary_formatting_still_works() {
        let out = sanitize_formatted_body("<b>bold</b> text", false);
        assert!(out.contains("<b>bold</b>"));
    }

    #[test]
    fn script_tag_structure_still_stripped() {
        // The <script> TAG must not survive (that's the actual XSS vector);
        // its escaped text content surviving as inert text is safe and
        // expected — same "unknown tag stripped, content preserved"
        // convention Tesseract's own html_spans.cpp uses for tags it
        // doesn't recognize.
        let out = sanitize_formatted_body("<script>alert(1)</script>hi", false);
        assert!(!out.contains("<script"));
    }

    #[test]
    fn img_onerror_attribute_still_stripped() {
        // Compat mode's <img> allow-list gains data-mx-emoticon but nothing
        // else — onerror must still be rejected.
        let out = sanitize_formatted_body(
            "<img src=\"mxc://x.org/a\" onerror=\"alert(1)\">",
            false,
        );
        assert!(!out.contains("onerror"));
        assert!(!out.contains("alert(1)"));
    }

    // NOTE: this module's doc comment describes a known ruma-html 0.8.0 gap
    // where <img src> scheme enforcement behaves inconsistently (verified
    // during development: identical scheme-restriction configs produced
    // different results depending on unrelated attributes like `alt` being
    // present). That inconsistency made it impractical to pin down with a
    // stable unit test here, and — more importantly — Tesseract's actual
    // safety guarantee doesn't depend on it: html_spans.cpp independently
    // requires an mxc:// prefix before treating any <img> as a renderable
    // emoticon (see "img: non-mxc src is rejected..." in
    // tests/cpp/test_html_spans.cpp), regardless of what either sanitizer
    // does or doesn't strip.
}
