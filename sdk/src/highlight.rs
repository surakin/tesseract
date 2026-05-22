//! Syntax highlighting for chat-message code blocks.
//!
//! Given the code text and the language token from a `<pre><code
//! class="language-X">` fence, produce a flat list of colored runs
//! (`HighlightSpan`) the C++ layer paints. Colors come from a syntect theme
//! chosen by light/dark mode, so the caller does not need its own palette.
//!
//! On any miss (empty/unknown language, highlight error) this returns an empty
//! vec, which the caller treats as "render as plain monospace".

use crate::ffi::HighlightSpan;
use std::sync::OnceLock;
use syntect::easy::HighlightLines;
use syntect::highlighting::{Theme, ThemeSet};
use syntect::parsing::{SyntaxReference, SyntaxSet};
use syntect::util::LinesWithEndings;

fn syntax_set() -> &'static SyntaxSet {
    static SET: OnceLock<SyntaxSet> = OnceLock::new();
    SET.get_or_init(SyntaxSet::load_defaults_newlines)
}

fn theme(dark: bool) -> &'static Theme {
    static LIGHT: OnceLock<Theme> = OnceLock::new();
    static DARK: OnceLock<Theme> = OnceLock::new();
    let (slot, name) = if dark {
        (&DARK, "base16-ocean.dark")
    } else {
        (&LIGHT, "InspiredGitHub")
    };
    slot.get_or_init(|| {
        let mut ts = ThemeSet::load_defaults();
        ts.themes
            .remove(name)
            // Defaults always contain these names; fall back to any theme so
            // we never panic even if syntect's bundle changes.
            .or_else(|| ts.themes.values().next().cloned())
            .expect("syntect ships at least one default theme")
    })
}

/// Map a markdown fence token (e.g. "rust", "py", "c++") to a file-extension
/// token syntect recognizes. Returns the input unchanged when no alias applies
/// so a raw extension still works.
fn alias_to_extension(lang: &str) -> &str {
    match lang {
        "rust" => "rs",
        "python" => "py",
        "javascript" | "js" | "node" | "mjs" => "js",
        "typescript" | "ts" => "ts",
        "c++" | "cpp" | "cxx" | "cc" | "hpp" | "hxx" => "cpp",
        "c#" | "csharp" => "cs",
        "objective-c" | "objc" => "m",
        "golang" => "go",
        "ruby" => "rb",
        "kotlin" => "kt",
        "shell" | "bash" | "zsh" | "sh" => "sh",
        "yml" | "yaml" => "yaml",
        "markdown" => "md",
        "patch" => "diff",
        "haskell" => "hs",
        "perl" => "pl",
        other => other,
    }
}

fn find_syntax<'a>(ss: &'a SyntaxSet, lang: &str) -> Option<&'a SyntaxReference> {
    let lower = lang.trim().to_ascii_lowercase();
    if lower.is_empty() {
        return None;
    }
    ss.find_syntax_by_extension(alias_to_extension(&lower))
        .or_else(|| ss.find_syntax_by_token(&lower))
}

/// Highlight `code` as language `lang`. Empty result means "no highlighting"
/// (unknown language or error) — the caller should fall back to plain text.
pub fn highlight_code(code: &str, lang: &str, dark: bool) -> Vec<HighlightSpan> {
    let ss = syntax_set();
    let Some(syntax) = find_syntax(ss, lang) else {
        return Vec::new();
    };
    let mut hl = HighlightLines::new(syntax, theme(dark));
    let mut out = Vec::new();
    for line in LinesWithEndings::from(code) {
        let Ok(ranges) = hl.highlight_line(line, ss) else {
            return Vec::new();
        };
        for (style, text) in ranges {
            if text.is_empty() {
                continue;
            }
            let c = style.foreground;
            out.push(HighlightSpan {
                text: text.to_string(),
                r: c.r,
                g: c.g,
                b: c.b,
            });
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn joined(spans: &[HighlightSpan]) -> String {
        spans.iter().map(|s| s.text.as_str()).collect()
    }

    #[test]
    fn unknown_language_yields_no_spans() {
        assert!(highlight_code("x = 1", "definitely-not-a-language", false).is_empty());
    }

    #[test]
    fn empty_language_yields_no_spans() {
        assert!(highlight_code("let x = 1;", "", false).is_empty());
    }

    #[test]
    fn rust_keyword_gets_distinct_color() {
        let spans = highlight_code("fn main() {}\n", "rust", false);
        assert!(!spans.is_empty(), "rust should be recognized");
        // Round-trip: concatenated span text equals the input.
        assert_eq!(joined(&spans), "fn main() {}\n");
        // At least two different foreground colors appear (keyword vs default),
        // i.e. the output is actually highlighted rather than one flat color.
        let first = (spans[0].r, spans[0].g, spans[0].b);
        assert!(
            spans.iter().any(|s| (s.r, s.g, s.b) != first),
            "expected more than one color in highlighted output"
        );
    }

    #[test]
    fn extension_token_is_accepted() {
        // "py" is an extension alias for Python.
        let spans = highlight_code("x = 1\n", "py", false);
        assert!(!spans.is_empty());
        assert_eq!(joined(&spans), "x = 1\n");
    }

    #[test]
    fn dark_and_light_differ() {
        let light = highlight_code("fn main() {}\n", "rust", false);
        let dark = highlight_code("fn main() {}\n", "rust", true);
        assert_eq!(joined(&light), joined(&dark));
        // The two themes should not produce identical colors throughout.
        let same = light
            .iter()
            .zip(dark.iter())
            .all(|(a, b)| (a.r, a.g, a.b) == (b.r, b.g, b.b));
        assert!(!same, "light and dark themes should differ in color");
    }
}
