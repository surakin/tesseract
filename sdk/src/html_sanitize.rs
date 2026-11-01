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
//! **Known upstream gap**: testing directly against `ruma-html` 0.8.0 shows
//! Compat mode's documented mxc-only scheme restriction on `<img src>` is
//! not actually enforced — a `<img src="https://...">` survives unstripped
//! (`<a href>`'s scheme restriction, by contrast, works correctly). This
//! module closes the gap itself: `strip_non_mxc_img_src` drops any non-`mxc://`
//! `src` after sanitizing, so neither the timeline nor exported HTML can
//! carry a remote image URL. `alt` is kept.

use ruma::html::{
    Attribute, Html, ListBehavior, NodeData, NodeRef, PropertiesNames, SanitizerConfig, StrTendril,
};

/// Sanitize `raw_html` the same way matrix-sdk-ui's Timeline would (Compat
/// mode), except:
///  * `<img data-mx-emoticon>` is preserved instead of stripped (MSC2545);
///  * `text-align` on `<td>`/`<th>` survives, so Markdown table column
///    alignment (`|:--|`, `|:-:|`, `|--:|`) is honoured for received
///    messages. `ruma-html` has no CSS-property-level filtering, so `style`
///    is allowed through the sanitizer and then narrowed to a canonical
///    `text-align:{left|center|right}` (or dropped) by
///    [`normalize_table_cell_styles`] — no other CSS can reach the wire.
///
/// `remove_reply_fallback` should mirror whatever matrix-sdk-ui would have
/// used for this event (true for a message that is itself a reply).
pub fn sanitize_formatted_body(raw_html: &str, remove_reply_fallback: bool) -> String {
    let mut config = SanitizerConfig::compat().allow_attributes(
        [
            PropertiesNames {
                parent: "img",
                properties: &["data-mx-emoticon"],
            },
            PropertiesNames {
                parent: "td",
                properties: &["style"],
            },
            PropertiesNames {
                parent: "th",
                properties: &["style"],
            },
        ],
        ListBehavior::Add,
    );
    if remove_reply_fallback {
        config = config.remove_reply_fallback();
    }
    let html = Html::parse(raw_html);
    html.sanitize_with(&config);
    normalize_table_cell_styles(&html);
    strip_non_mxc_img_src(&html);
    html.to_string()
}

/// Walk the sanitized DOM and reduce every `<td>`/`<th>` `style` attribute to
/// at most `style="text-align:left|center|right"`. Anything else in the
/// attribute — arbitrary CSS that the opaque-`style` allow-rule let through —
/// is discarded; a cell with no usable `text-align` loses its `style`
/// attribute entirely.
fn normalize_table_cell_styles(html: &Html) {
    // `Attribute` carries a `Tendril` (interior mutability); ruma-html itself
    // keys its attribute set on it. We only ever mutate the set structurally.
    #[allow(clippy::mutable_key_type)]
    fn visit(node: &NodeRef) {
        if let NodeData::Element(el) = node.data() {
            let name = el.name.local.as_ref();
            if name == "td" || name == "th" {
                let mut attrs = el.attrs.borrow_mut();
                let style_attr = attrs
                    .iter()
                    .find(|a| a.name.local.as_ref() == "style")
                    .cloned();
                if let Some(style_attr) = style_attr {
                    attrs.retain(|a| a.name.local.as_ref() != "style");
                    if let Some(align) = parse_text_align(&style_attr.value) {
                        attrs.insert(Attribute {
                            name: style_attr.name.clone(),
                            value: StrTendril::from(format!("text-align:{align}")),
                        });
                    }
                }
            }
        }
        for child in node.children() {
            visit(&child);
        }
    }
    for child in html.children() {
        visit(&child);
    }
}

/// Drop `src` from every `<img>` whose source isn't `mxc://`. ruma-html's
/// Compat config documents an mxc-only rule for `<img src>` but doesn't
/// enforce it (see module docs); without this, a remote `src` survives into
/// exported HTML and is fetched when the file is opened in a browser. `alt`
/// stays, so emoticons still read as their shortcode.
fn strip_non_mxc_img_src(html: &Html) {
    #[allow(clippy::mutable_key_type)]
    fn visit(node: &NodeRef) {
        if let NodeData::Element(el) = node.data() {
            if el.name.local.as_ref() == "img" {
                el.attrs
                    .borrow_mut()
                    .retain(|a| a.name.local.as_ref() != "src" || a.value.starts_with("mxc://"));
            }
        }
        for child in node.children() {
            visit(&child);
        }
    }
    for child in html.children() {
        visit(&child);
    }
}

/// Extract a canonical horizontal alignment from a CSS `style` value. Returns
/// `"left"`, `"center"` or `"right"`; `None` for a missing / unrecognised
/// `text-align` (including `justify`).
fn parse_text_align(style: &str) -> Option<&'static str> {
    for decl in style.split(';') {
        let mut parts = decl.splitn(2, ':');
        let prop = parts.next()?.trim();
        if !prop.eq_ignore_ascii_case("text-align") {
            continue;
        }
        return match parts.next()?.trim().to_ascii_lowercase().as_str() {
            "left" => Some("left"),
            "center" => Some("center"),
            "right" => Some("right"),
            _ => None,
        };
    }
    None
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
        let out =
            sanitize_formatted_body("<img src=\"mxc://x.org/a\" onerror=\"alert(1)\">", false);
        assert!(!out.contains("onerror"));
        assert!(!out.contains("alert(1)"));
    }

    #[test]
    fn table_cell_text_align_survives_canonical() {
        let input = "<table><tr><td style=\"text-align: right\">1</td></tr></table>";
        let out = sanitize_formatted_body(input, false);
        assert!(out.contains("text-align:right"), "got: {out}");
    }

    #[test]
    fn table_header_cell_text_align_survives() {
        let input =
            "<table><thead><tr><th style=\"text-align: center\">H</th></tr></thead></table>";
        let out = sanitize_formatted_body(input, false);
        assert!(out.contains("text-align:center"), "got: {out}");
    }

    #[test]
    fn table_cell_style_is_narrowed_to_text_align_only() {
        // Arbitrary CSS smuggled in alongside text-align must be discarded.
        let input = "<table><tr><td style=\"color:red;position:fixed;text-align:center\">x</td></tr></table>";
        let out = sanitize_formatted_body(input, false);
        assert!(out.contains("text-align:center"), "got: {out}");
        assert!(!out.contains("color"), "got: {out}");
        assert!(!out.contains("position"), "got: {out}");
    }

    #[test]
    fn table_cell_style_without_text_align_is_dropped() {
        let input = "<table><tr><td style=\"color:red\">x</td></tr></table>";
        let out = sanitize_formatted_body(input, false);
        assert!(!out.contains("style"), "got: {out}");
        assert!(!out.contains("color"), "got: {out}");
    }

    #[test]
    fn table_cell_text_align_justify_is_dropped() {
        let input = "<table><tr><td style=\"text-align:justify\">x</td></tr></table>";
        let out = sanitize_formatted_body(input, false);
        assert!(!out.contains("style"), "got: {out}");
        assert!(!out.contains("justify"), "got: {out}");
    }

    #[test]
    fn style_on_non_cell_element_still_stripped() {
        let input = "<p style=\"text-align:center\">x</p>";
        let out = sanitize_formatted_body(input, false);
        assert!(!out.contains("style"), "got: {out}");
        assert!(!out.contains("text-align"), "got: {out}");
    }

    #[test]
    fn img_with_remote_src_loses_src_but_keeps_alt() {
        let out =
            sanitize_formatted_body("<img src=\"https://evil.example/p.png\" alt=\"x\">", false);
        assert!(!out.contains("evil.example"), "got: {out}");
        assert!(out.contains("alt=\"x\""), "got: {out}");
    }

    #[test]
    fn mxc_emoticon_img_is_untouched() {
        let out = sanitize_formatted_body(
            "<img data-mx-emoticon src=\"mxc://example.org/abc\" alt=\":party:\">",
            false,
        );
        assert!(out.contains("src=\"mxc://example.org/abc\""), "got: {out}");
        assert!(out.contains("alt=\":party:\""), "got: {out}");
    }

    // NOTE: ruma-html 0.8.0 doesn't enforce the mxc-only rule for <img src>
    // (see the module docs); `strip_non_mxc_img_src` enforces it here, and
    // html_spans.cpp independently requires an mxc:// prefix before rendering.
}
