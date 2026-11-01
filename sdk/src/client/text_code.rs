//! Language-neutral codes for the short texts the SDK layer used to write in
//! English (previews of non-text messages, undecryptable-message reasons,
//! pinned-message actions). A code is `\u{1}name`, optionally followed by
//! `\u{1}argument`; the C++ UI decodes it into a translated string
//! (`views::sdk_text::localize`). Text that doesn't start with `\u{1}` is
//! ordinary prose (a message body, or a preview cached before codes existed)
//! and is shown as it is.

const MARK: char = '\u{1}';

/// The code `name`.
pub(crate) fn text_code(name: &str) -> String {
    format!("{MARK}{name}")
}

/// The code `name` carrying one argument (a poll question, a count).
pub(crate) fn text_code_arg(name: &str, arg: impl std::fmt::Display) -> String {
    format!("{MARK}{name}{MARK}{arg}")
}

/// `text` from a message, safe to put where a code could stand: the marker is
/// removed, so a sender can't write a code of their own (a body starting with
/// the marker would show up as one of our phrases in a quote or preview).
pub(crate) fn user_text(text: &str) -> String {
    text.replace(MARK, "")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_have_a_stable_wire_format() {
        assert_eq!(text_code("image"), "\u{1}image");
        assert_eq!(text_code_arg("poll", "Lunch?"), "\u{1}poll\u{1}Lunch?");
        assert_eq!(text_code_arg("pin_added_n", 3), "\u{1}pin_added_n\u{1}3");
    }

    #[test]
    fn user_text_cannot_spell_a_code() {
        assert_eq!(user_text("\u{1}deleted"), "deleted");
        assert_eq!(user_text("a\u{1}b\u{1}c"), "abc");
        assert_eq!(user_text("plain"), "plain");
    }
}

#[cfg(test)]
mod preview_tests {
    #[test]
    fn a_first_line_preview_drops_the_marker() {
        assert_eq!(super::super::first_line("\u{1}deleted"), "deleted");
        assert_eq!(
            super::super::poll_permalink_preview("\u{1}x"),
            "\u{1}poll\u{1}x"
        );
    }
}
