//! Per-event rendering for room-history export, in each supported output
//! format. Pure string formatting — no I/O, no FFI beyond the already-
//! shared `crate::ffi::TimelineEvent` type, so this stays unit-testable
//! without a C++ toolchain or a live client.

use super::labels::{membership_labels, ExportLabel, Labels};
use crate::ffi::TimelineEvent;

/// Export-run-level metadata, independent of any single event.
pub(super) struct ExportMeta {
    pub room_name: String,
    pub exported_at_ms: u64,
}

/// One output format's rendering rules. Implementors are pure functions of
/// their inputs — no shared mutable state — so header/event/footer calls
/// can be tested independently and in any order.
pub(super) trait ExportSink: Send + Sync {
    fn extension(&self) -> &'static str;
    fn header(&self, meta: &ExportMeta, labels: &Labels) -> String;
    /// `media_rel_path` is `Some(path)` when this event's attachment was
    /// downloaded (HTML export with images enabled only); `None` otherwise,
    /// including for plain-text export, which never embeds media.
    fn event(&self, ev: &TimelineEvent, media_rel_path: Option<&str>, labels: &Labels) -> String;
    fn footer(&self, labels: &Labels, complete: bool, event_count: u64) -> String;
}

fn sender_display(ev: &TimelineEvent) -> &str {
    if !ev.sender_name.is_empty() {
        &ev.sender_name
    } else {
        &ev.sender
    }
}

/// Renders an `m.room.member` row via its `membership_action` discriminant,
/// matching `MessageListView.cpp::membership_expanded_phrase`'s exact
/// wording and by-actor/no-actor split (`by_actor` = sender differs from
/// the target, e.g. an admin banning someone else rather than someone
/// leaving on their own).
fn membership_line(ev: &TimelineEvent, labels: &Labels) -> String {
    let Some(resolved) = membership_labels(&ev.membership_action) else {
        return String::new();
    };
    let target = if !ev.membership_target_name.is_empty() {
        ev.membership_target_name.as_str()
    } else {
        ev.membership_target_user_id.as_str()
    };
    let by_actor = ev.sender != ev.membership_target_user_id;
    labels.format_membership(resolved, by_actor, target, sender_display(ev))
}

/// Resolves an event's body text through `labels` for the placeholder
/// cases (redacted, membership, virtual rows carry no body at all);
/// otherwise the sender's plain `body`. Reply quoting, the edited marker,
/// and reactions are layered on separately by each sink, since their
/// punctuation/markup differs between plain text and HTML.
fn body_text(ev: &TimelineEvent, labels: &Labels) -> String {
    if ev.msg_type == "m.room.member" {
        return membership_line(ev, labels);
    }
    if ev.msg_type == "m.redacted" {
        return labels.get(ExportLabel::Redacted).to_string();
    }
    if ev.msg_type == "m.utd" {
        return labels.get(ExportLabel::UnableToDecrypt).to_string();
    }
    ev.body.clone()
}

fn reactions_summary(ev: &TimelineEvent) -> String {
    ev.reactions
        .iter()
        .map(|r| format!("{} {}", r.key, r.count))
        .collect::<Vec<_>>()
        .join(", ")
}

/// Formats a Unix millisecond timestamp as "YYYY-MM-DD HH:MM:SS" in UTC.
/// Dependency-free civil-calendar math (Howard Hinnant's well-known
/// `civil_from_days` algorithm) rather than pulling in a date/time crate
/// just for this one call site.
pub(super) fn format_ts(ms: u64) -> String {
    let secs = (ms / 1000) as i64;
    let days = secs.div_euclid(86400);
    let time_of_day = secs.rem_euclid(86400);
    let (y, m, d) = civil_from_days(days);
    let (h, mi, s) = (time_of_day / 3600, (time_of_day / 60) % 60, time_of_day % 60);
    format!("{y:04}-{m:02}-{d:02} {h:02}:{mi:02}:{s:02}")
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // [1, 12]
    let y = yoe as i64 + era * 400 + if m <= 2 { 1 } else { 0 };
    (y, m, d)
}

fn html_escape(s: &str) -> String {
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

// ── Plain text ──────────────────────────────────────────────────────────

pub(super) struct TextSink;

impl ExportSink for TextSink {
    fn extension(&self) -> &'static str {
        "txt"
    }

    fn header(&self, meta: &ExportMeta, labels: &Labels) -> String {
        let title = labels.format(ExportLabel::HeaderTitle, &[&meta.room_name]);
        let exported = labels.format(ExportLabel::ExportedOn, &[&format_ts(meta.exported_at_ms)]);
        format!("{title}\n{exported}\n\n")
    }

    fn event(&self, ev: &TimelineEvent, media_rel_path: Option<&str>, labels: &Labels) -> String {
        if ev.msg_type.starts_with("virtual.") {
            return String::new();
        }
        let mut out = String::new();
        out.push('[');
        out.push_str(&format_ts(ev.timestamp));
        out.push(']');
        if ev.msg_type != "m.room.member" {
            out.push(' ');
            out.push_str(sender_display(ev));
            out.push(':');
        }
        out.push(' ');
        if !ev.in_reply_to_id.is_empty() {
            out.push_str(&labels.format(ExportLabel::ReplyTo, &[&ev.in_reply_to_body]));
            out.push_str(" — ");
        }
        out.push_str(&body_text(ev, labels));
        if ev.is_edited {
            out.push(' ');
            out.push_str(labels.get(ExportLabel::Edited));
        }
        if let Some(path) = media_rel_path {
            out.push(' ');
            out.push_str(&labels.format(ExportLabel::AttachmentSaved, &[path]));
        }
        if !ev.reactions.is_empty() {
            out.push('\n');
            out.push_str(&labels.format(ExportLabel::ReactionsLine, &[&reactions_summary(ev)]));
        }
        out.push('\n');
        out
    }

    fn footer(&self, _labels: &Labels, complete: bool, event_count: u64) -> String {
        if complete {
            format!("\n— {event_count} messages —\n")
        } else {
            format!("\n— {event_count} messages (export incomplete) —\n")
        }
    }
}

// ── HTML ────────────────────────────────────────────────────────────────

pub(super) struct HtmlSink;

impl ExportSink for HtmlSink {
    fn extension(&self) -> &'static str {
        "html"
    }

    fn header(&self, meta: &ExportMeta, labels: &Labels) -> String {
        let title = labels.format(ExportLabel::HeaderTitle, &[&meta.room_name]);
        let exported = labels.format(ExportLabel::ExportedOn, &[&format_ts(meta.exported_at_ms)]);
        format!(
            "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n\
             <title>{}</title>\n\
             <style>\n\
             body{{font-family:sans-serif;max-width:720px;margin:2em auto;padding:0 1em}}\n\
             .msg{{margin-bottom:0.75em}}\n\
             .ts{{color:#888;font-size:0.85em;margin-right:0.5em}}\n\
             .sender{{font-weight:600;margin-right:0.4em}}\n\
             .edited{{color:#888;font-size:0.85em}}\n\
             .reply{{border-left:3px solid #ccc;margin:0.25em 0 0.25em 0;padding-left:0.6em;color:#666}}\n\
             .reactions{{color:#666;font-size:0.85em}}\n\
             .attachment img{{max-width:100%;border-radius:4px}}\n\
             </style>\n</head><body>\n\
             <h1>{title}</h1>\n<p class=\"exported\">{exported}</p>\n",
            html_escape(&meta.room_name)
        )
    }

    fn event(&self, ev: &TimelineEvent, media_rel_path: Option<&str>, labels: &Labels) -> String {
        if ev.msg_type.starts_with("virtual.") {
            return String::new();
        }
        let mut out = String::new();
        out.push_str("<div class=\"msg\">");
        out.push_str(&format!("<span class=\"ts\">[{}]</span>", format_ts(ev.timestamp)));
        if ev.msg_type != "m.room.member" {
            out.push_str(&format!(
                "<span class=\"sender\">{}:</span>",
                html_escape(sender_display(ev))
            ));
        }
        if !ev.in_reply_to_id.is_empty() {
            out.push_str(&format!(
                "<div class=\"reply\">{}</div>",
                html_escape(&labels.format(ExportLabel::ReplyTo, &[&ev.in_reply_to_body]))
            ));
        }
        out.push_str("<span class=\"body\">");
        if ev.msg_type == "m.room.member" || ev.msg_type == "m.redacted" || ev.msg_type == "m.utd" {
            out.push_str(&html_escape(&body_text(ev, labels)));
        } else if !ev.formatted_body.is_empty() {
            out.push_str(&crate::html_sanitize::sanitize_formatted_body(
                &ev.formatted_body,
                /* remove_reply_fallback */ true,
            ));
        } else {
            out.push_str(&html_escape(&ev.body));
        }
        out.push_str("</span>");
        if ev.is_edited {
            out.push_str(&format!(
                " <span class=\"edited\">{}</span>",
                html_escape(labels.get(ExportLabel::Edited))
            ));
        }
        match media_rel_path {
            Some(path) => {
                out.push_str(&format!(
                    "<div class=\"attachment\"><img src=\"{}\" alt=\"{}\"></div>",
                    html_escape(path),
                    html_escape(sender_display(ev))
                ));
            }
            None if is_image_event(ev) => {
                out.push_str(&format!(
                    "<div class=\"attachment\">{}</div>",
                    html_escape(&labels.format(
                        ExportLabel::AttachmentUnavailable,
                        &[&image_display_name(ev)]
                    ))
                ));
            }
            None => {}
        }
        if !ev.reactions.is_empty() {
            out.push_str(&format!(
                "<div class=\"reactions\">{}</div>",
                html_escape(&labels.format(ExportLabel::ReactionsLine, &[&reactions_summary(ev)]))
            ));
        }
        out.push_str("</div>\n");
        out
    }

    fn footer(&self, _labels: &Labels, complete: bool, event_count: u64) -> String {
        let note = if complete {
            format!("{event_count} messages")
        } else {
            format!("{event_count} messages (export incomplete)")
        };
        format!("<hr><p class=\"footer\">{}</p>\n</body></html>\n", html_escape(&note))
    }
}

fn is_image_event(ev: &TimelineEvent) -> bool {
    matches!(ev.msg_type.as_str(), "m.image" | "m.sticker")
}

fn image_display_name(ev: &TimelineEvent) -> String {
    if !ev.image_filename.is_empty() {
        ev.image_filename.clone()
    } else if !ev.file_name.is_empty() {
        ev.file_name.clone()
    } else {
        ev.event_id.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn labels() -> Labels {
        let mut t = vec![String::new(); ExportLabel::COUNT];
        t[ExportLabel::HeaderTitle as usize] = "History of {0}".into();
        t[ExportLabel::ExportedOn as usize] = "Exported {0}".into();
        t[ExportLabel::Redacted as usize] = "Message deleted".into();
        t[ExportLabel::UnableToDecrypt as usize] = "Unable to decrypt this message".into();
        t[ExportLabel::Edited as usize] = "(edited)".into();
        t[ExportLabel::ReplyTo as usize] = "In reply to {0}".into();
        t[ExportLabel::ReactionsLine as usize] = "Reactions: {0}".into();
        t[ExportLabel::AttachmentSaved as usize] = "Attachment: {0}".into();
        t[ExportLabel::AttachmentUnavailable as usize] = "Image unavailable: {0}".into();
        t[ExportLabel::MembershipJoined as usize] = "{0} joined the room".into();
        t[ExportLabel::MembershipInvitedByActor as usize] = "{0} was invited by {1}".into();
        t[ExportLabel::MembershipInvitedNoActor as usize] = "{0} received an invitation".into();
        Labels::new(t)
    }

    fn base_event() -> TimelineEvent {
        TimelineEvent {
            event_id: "$1".into(),
            sender: "@alice:example.org".into(),
            sender_name: "Alice".into(),
            body: "hello world".into(),
            timestamp: 1_577_836_800_000, // 2020-01-01 00:00:00 UTC
            msg_type: "m.text".into(),
            ..Default::default()
        }
    }

    #[test]
    fn format_ts_known_epoch_values() {
        assert_eq!(format_ts(0), "1970-01-01 00:00:00");
        assert_eq!(format_ts(1_000_000_000_000), "2001-09-09 01:46:40"); // Unix billennium
        assert_eq!(format_ts(1_577_836_800_000), "2020-01-01 00:00:00");
    }

    #[test]
    fn text_event_basic_line() {
        let ev = base_event();
        let line = TextSink.event(&ev, None, &labels());
        assert_eq!(line, "[2020-01-01 00:00:00] Alice: hello world\n");
    }

    #[test]
    fn text_event_virtual_row_is_empty() {
        let mut ev = base_event();
        ev.msg_type = "virtual.date_divider".into();
        assert_eq!(TextSink.event(&ev, None, &labels()), "");
    }

    #[test]
    fn text_event_redacted_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.redacted".into();
        ev.body = String::new();
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("Message deleted"), "{line}");
    }

    #[test]
    fn text_event_undecryptable_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.utd".into();
        ev.body = "🔒 Sent before you joined this room".into();
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("Unable to decrypt this message"), "{line}");
        assert!(!line.contains("Sent before you joined"), "{line}");
    }

    #[test]
    fn html_event_undecryptable_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.utd".into();
        ev.body = "🔒 Sent before you joined this room".into();
        let out = HtmlSink.event(&ev, None, &labels());
        assert!(out.contains("Unable to decrypt this message"), "{out}");
        assert!(!out.contains("Sent before you joined"), "{out}");
    }

    #[test]
    fn text_event_edited_marker() {
        let mut ev = base_event();
        ev.is_edited = true;
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.trim_end().ends_with("(edited)"), "{line}");
    }

    #[test]
    fn text_event_reply_prefix() {
        let mut ev = base_event();
        ev.in_reply_to_id = "$0".into();
        ev.in_reply_to_body = "original message".into();
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("In reply to original message"), "{line}");
    }

    #[test]
    fn text_event_reactions_line() {
        let mut ev = base_event();
        ev.reactions = vec![crate::ffi::ReactionGroup {
            key: "\u{1F44D}".into(),
            count: 3,
            ..Default::default()
        }];
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("Reactions: \u{1F44D} 3"), "{line}");
    }

    #[test]
    fn text_event_membership_joined() {
        let mut ev = base_event();
        ev.msg_type = "m.room.member".into();
        ev.membership_action = "joined".into();
        // Self-driven transitions: the target IS the sender, per the real
        // converter's `membership_target_user_id: change.user_id()`.
        ev.membership_target_user_id = ev.sender.clone();
        ev.membership_target_name = ev.sender_name.clone();
        ev.body = String::new();
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("Alice joined the room"), "{line}");
        assert!(!line.contains("Alice:"), "membership rows omit the sender prefix: {line}");
    }

    #[test]
    fn text_event_membership_invited_has_target() {
        let mut ev = base_event();
        ev.msg_type = "m.room.member".into();
        ev.membership_action = "invited".into();
        ev.membership_target_user_id = "@bob:example.org".into();
        ev.membership_target_name = "Bob".into();
        let line = TextSink.event(&ev, None, &labels());
        assert!(line.contains("Bob was invited by Alice"), "{line}");
    }

    #[test]
    fn text_event_attachment_saved_path() {
        let ev = base_event();
        let line = TextSink.event(&ev, Some("media/abc-photo.jpg"), &labels());
        assert!(line.contains("Attachment: media/abc-photo.jpg"), "{line}");
    }

    #[test]
    fn html_event_escapes_body() {
        let mut ev = base_event();
        ev.body = "<script>alert(1)</script>".into();
        let out = HtmlSink.event(&ev, None, &labels());
        assert!(!out.contains("<script>"), "{out}");
        assert!(out.contains("&lt;script&gt;"), "{out}");
    }

    #[test]
    fn html_event_prefers_sanitized_formatted_body() {
        let mut ev = base_event();
        ev.formatted_body = "<b>bold</b>".into();
        let out = HtmlSink.event(&ev, None, &labels());
        assert!(out.contains("<b>bold</b>"), "{out}");
    }

    #[test]
    fn html_event_embeds_image_when_media_present() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        let out = HtmlSink.event(&ev, Some("media/abc-photo.jpg"), &labels());
        assert!(out.contains("<img src=\"media/abc-photo.jpg\""), "{out}");
    }

    #[test]
    fn html_event_image_unavailable_without_media() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        ev.image_filename = "photo.jpg".into();
        let out = HtmlSink.event(&ev, None, &labels());
        assert!(out.contains("Image unavailable: photo.jpg"), "{out}");
    }

    #[test]
    fn html_header_and_footer_are_balanced() {
        let meta = ExportMeta { room_name: "Test & Room".into(), exported_at_ms: 0 };
        let header = HtmlSink.header(&meta, &labels());
        assert!(header.contains("<html>"));
        assert!(header.contains("Test &amp; Room"));
        let footer = HtmlSink.footer(&labels(), true, 42);
        assert!(footer.contains("</html>"));
        assert!(footer.contains("42 messages"));
    }

    #[test]
    fn footer_notes_incomplete_export() {
        let footer = TextSink.footer(&labels(), false, 5);
        assert!(footer.contains("incomplete"), "{footer}");
    }
}
