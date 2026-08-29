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

/// An event's attachment-download outcome, as far as rendering cares.
/// A plain `bool` + `Option<&str>` pair would let "saved" and "too large"
/// be set together, which can never actually happen — this enum makes that
/// combination unrepresentable instead of needing to be guarded against.
pub(super) enum AttachmentState<'a> {
    /// No attachment, or none was attempted (plain-text export never
    /// downloads media; HTML export without images enabled never does
    /// either).
    None,
    /// Downloaded successfully; `path` is relative to the exported doc.
    Saved(&'a str),
    /// Exceeded the media size cap — deliberately distinct from a generic
    /// download failure so the exported doc can say why.
    TooLarge,
}

/// Cross-event context `HtmlSink` needs that a single `TimelineEvent`
/// doesn't carry on its own. `TextSink` ignores this entirely — grouping
/// and avatars are HTML-only concerns.
#[derive(Default)]
pub(super) struct EventContext<'a> {
    /// The previous item fed to `event()` in this window (real or
    /// virtual) — `None` at a window's start. Drives consecutive-message
    /// grouping. Reset per window, so grouping doesn't carry across a
    /// window boundary — a minor, cosmetic-only gap (see `mod.rs`).
    pub prev: Option<&'a TimelineEvent>,
    /// Relative path to the sender's downloaded avatar image, if one was
    /// fetched for this run. `None` falls back to a colored-initials
    /// circle.
    pub avatar_path: Option<&'a str>,
}

/// One output format's rendering rules. Implementors are pure functions of
/// their inputs — no shared mutable state — so header/event/footer calls
/// can be tested independently and in any order.
pub(super) trait ExportSink: Send + Sync {
    fn extension(&self) -> &'static str;
    fn header(&self, meta: &ExportMeta, labels: &Labels) -> String;
    fn event(&self, ev: &TimelineEvent, ctx: &EventContext<'_>, attachment: AttachmentState<'_>, labels: &Labels) -> String;
    fn footer(&self, labels: &Labels, complete: bool, event_count: u64) -> String;
}

fn sender_display(ev: &TimelineEvent) -> &str {
    if !ev.sender_name.is_empty() {
        &ev.sender_name
    } else {
        &ev.sender
    }
}

/// Deterministic 8-way hash of a display name, for the HTML export's
/// per-sender name/avatar coloring. Mirrors the *scheme*
/// `MessageListView.cpp::sender_color()` uses (hash name -> pick from an
/// 8-entry palette) but not its exact hash — that's C++ `std::hash`, and
/// Rust's own default hasher is randomized per-process (unusable here: the
/// same name must map to the same color every time this binary runs). A
/// small fixed FNV-1a keeps this pure and stable across runs/platforms.
fn sender_color_index(name: &str) -> u8 {
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in name.as_bytes() {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    (hash % 8) as u8
}

/// First character of a display name, uppercased, for the initials-circle
/// avatar fallback. `"?"` for an empty name (shouldn't happen in practice —
/// `sender_display` already falls back to the bare Matrix ID — but a
/// pure function should still be total).
fn initial(name: &str) -> String {
    match name.chars().next() {
        Some(c) => c.to_uppercase().collect(),
        None => "?".to_string(),
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

/// Formats a Unix millisecond timestamp as "HH:MM" in UTC, for the HTML
/// export's per-message timestamp (the live timeline's own `format_hhmm`
/// equivalent — the full date is already established by the export's
/// running order, so repeating it per message would just be noise).
fn format_hm(ms: u64) -> String {
    let secs = (ms / 1000) as i64;
    let time_of_day = secs.rem_euclid(86400);
    format!("{:02}:{:02}", time_of_day / 3600, (time_of_day / 60) % 60)
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

/// Ports `fit_media()` (`ui/shared/views/media_utils.h`) exactly, so an
/// exported image renders at the same pixel size the live timeline would
/// give it: scale to fit inside `(max_w, max_h)`, preserving aspect ratio,
/// never upscaling past the natural size. Same fallback as the original
/// when dimensions are unknown (`<= 0`): `(max_w, max_h / 2)` — a plain
/// 2:1 box, not aspect-correct, but this *is* the live view's own
/// fallback, so matching it (quirk included) is the point.
fn fit_media(natural_w: f64, natural_h: f64, max_w: f64, max_h: f64) -> (f64, f64) {
    if natural_w <= 0.0 || natural_h <= 0.0 {
        return (max_w, max_h * 0.5);
    }
    let sx = max_w / natural_w;
    let sy = max_h / natural_h;
    let s = sx.min(sy).min(1.0);
    (natural_w * s, natural_h * s)
}

/// Caps mirroring `client/include/tesseract/visual.h`'s
/// `kMaxInlineImageWidth`/`kMaxInlineImageHeight` (320×200) and
/// `kStickerSize` (256, square). The live view additionally intersects
/// these with the chat pane's available column width, which has no
/// equivalent here — the exported document has its own fixed max-width
/// instead (see `HtmlSink::header`'s `.attachment img` rule), so the caps
/// are used directly.
fn media_display_size(ev: &TimelineEvent) -> (f64, f64) {
    let (max_w, max_h) = if ev.msg_type == "m.sticker" { (256.0, 256.0) } else { (320.0, 200.0) };
    fit_media(ev.width as f64, ev.height as f64, max_w, max_h)
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

    fn event(&self, ev: &TimelineEvent, _ctx: &EventContext<'_>, attachment: AttachmentState<'_>, labels: &Labels) -> String {
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
        // Text export stays silent for a missing/oversized attachment (no
        // room to explain why) — only a successful save gets a mention.
        if let AttachmentState::Saved(path) = attachment {
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

/// Consecutive same-sender messages within this many ms collapse into one
/// visual group (avatar/name/timestamp shown once) — mirrors
/// `Settings::message_group_interval_s`'s default (300s). The export has
/// no way to see the user's actual configured value (a C++-side runtime
/// setting, never threaded into `RoomExportOptions`); hardcoding the
/// default is a deliberate, minor simplification rather than adding an
/// options field + UI control for a cosmetic-only knob.
const GROUP_INTERVAL_MS: u64 = 300_000;

pub(super) struct HtmlSink;

impl HtmlSink {
    /// True when `ev` should render as a grouped continuation of `prev`:
    /// same sender, `prev` isn't virtual or a membership row, within the
    /// grouping window, and `ev` isn't a reply (a reply always starts a
    /// fresh group, even from the same sender — matches `is_cont` in
    /// `MessageListView.cpp`).
    fn is_continuation(ev: &TimelineEvent, prev: Option<&TimelineEvent>) -> bool {
        if !ev.in_reply_to_id.is_empty() {
            return false;
        }
        let Some(p) = prev else { return false };
        p.sender == ev.sender
            && !p.msg_type.starts_with("virtual.")
            && p.msg_type != "m.room.member"
            && ev.timestamp.saturating_sub(p.timestamp) <= GROUP_INTERVAL_MS
    }
}

impl ExportSink for HtmlSink {
    fn extension(&self) -> &'static str {
        "html"
    }

    fn header(&self, meta: &ExportMeta, labels: &Labels) -> String {
        let title = labels.format(ExportLabel::HeaderTitle, &[&meta.room_name]);
        let exported = labels.format(ExportLabel::ExportedOn, &[&format_ts(meta.exported_at_ms)]);
        format!(
            "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n\
             <meta name=\"color-scheme\" content=\"light dark\">\n\
             <title>{}</title>\n\
             <style>\n\
             :root{{\n\
             --bg:#F8F9FA;--text:#111111;--text2:#76767B;--muted:#76767C;\n\
             --border:#D0D3D8;--accent:#0072ED;--hover:rgba(0,0,0,.06);\n\
             --chip-bg:#EBEDF0;--chip-border:#D0D3D8;\n\
             --chip-bg-me:#CFE3FF;--chip-border-me:#9CC4FF;--chip-text-me:#004A9E;\n\
             --avatar-ink:#FFFFFF;\n\
             --s0:#C0392B;--s1:#D35400;--s2:#6E7D00;--s3:#1E8449;\n\
             --s4:#117A65;--s5:#1565C0;--s6:#6A1B9A;--s7:#AD1457;\n\
             }}\n\
             @media (prefers-color-scheme:dark){{\n\
             :root{{\n\
             --bg:#202327;--text:#F0F0F2;--text2:#A0A0A8;--muted:#84848C;\n\
             --border:#33363B;--accent:#4DA3FF;--hover:rgba(255,255,255,.08);\n\
             --chip-bg:#2A2D33;--chip-border:#33363B;\n\
             --chip-bg-me:#1F3A66;--chip-border-me:#2D55A0;--chip-text-me:#BFD8FF;\n\
             --avatar-ink:#1B1D21;\n\
             --s0:#FF8A80;--s1:#FFAB40;--s2:#D4E157;--s3:#69F0AE;\n\
             --s4:#4DD0E1;--s5:#82B1FF;--s6:#CE93D8;--s7:#F48FB1;\n\
             }}\n\
             }}\n\
             *{{box-sizing:border-box}}\n\
             body{{font-family:-apple-system,\"Segoe UI Variable Text\",\"Segoe UI\",system-ui,sans-serif;\
             font-size:13px;background:var(--bg);color:var(--text);max-width:720px;margin:2em auto;padding:0 1em}}\n\
             h1{{font-size:1.3em}}\n\
             .exported{{color:var(--muted);font-size:0.85em}}\n\
             .msg{{display:grid;grid-template-columns:32px 1fr;column-gap:10px;padding:4px 0;align-items:start}}\n\
             .msg.cont{{padding-top:1px}}\n\
             .msg.sys{{grid-template-columns:1fr;text-align:center;color:var(--muted);font-size:0.85em;padding:2px 0}}\n\
             .avatar{{width:32px;height:32px;border-radius:50%;object-fit:cover;display:block}}\n\
             .avatar.initials{{display:flex;align-items:center;justify-content:center;font-weight:600;font-size:0.85em;color:var(--avatar-ink)}}\n\
             .content{{min-width:0}}\n\
             .head{{display:flex;align-items:baseline;gap:6px;margin-bottom:1px}}\n\
             .sender{{font-weight:600;font-size:0.95em}}\n\
             .ts{{color:var(--muted);font-size:0.75em}}\n\
             .body.muted{{color:var(--muted);font-style:italic}}\n\
             .edited{{color:var(--muted);font-size:0.85em}}\n\
             .reply{{border-left:3px solid var(--accent);background:var(--hover);border-radius:4px;padding:3px 8px;margin:2px 0 4px;font-size:0.9em}}\n\
             .reply .rsender{{display:block;font-weight:600;color:var(--text2)}}\n\
             .reply .rbody{{color:var(--muted)}}\n\
             .reactions{{margin-top:4px}}\n\
             .chip{{display:inline-block;padding:2px 8px;margin:0 4px 4px 0;border-radius:12px;background:var(--chip-bg);border:1px solid var(--chip-border);font-size:0.85em}}\n\
             .chip.me{{background:var(--chip-bg-me);border-color:var(--chip-border-me);color:var(--chip-text-me)}}\n\
             .attachment img{{max-width:100%;height:auto;border-radius:8px}}\n\
             .s0{{color:var(--s0)}} .s1{{color:var(--s1)}} .s2{{color:var(--s2)}} .s3{{color:var(--s3)}}\n\
             .s4{{color:var(--s4)}} .s5{{color:var(--s5)}} .s6{{color:var(--s6)}} .s7{{color:var(--s7)}}\n\
             .avatar.initials.s0{{background:var(--s0)}} .avatar.initials.s1{{background:var(--s1)}}\n\
             .avatar.initials.s2{{background:var(--s2)}} .avatar.initials.s3{{background:var(--s3)}}\n\
             .avatar.initials.s4{{background:var(--s4)}} .avatar.initials.s5{{background:var(--s5)}}\n\
             .avatar.initials.s6{{background:var(--s6)}} .avatar.initials.s7{{background:var(--s7)}}\n\
             </style>\n</head><body>\n\
             <h1>{title}</h1>\n<p class=\"exported\">{exported}</p>\n",
            html_escape(&meta.room_name)
        )
    }

    fn event(&self, ev: &TimelineEvent, ctx: &EventContext<'_>, attachment: AttachmentState<'_>, labels: &Labels) -> String {
        if ev.msg_type.starts_with("virtual.") {
            return String::new();
        }
        let is_member = ev.msg_type == "m.room.member";
        let is_placeholder = is_member || ev.msg_type == "m.redacted" || ev.msg_type == "m.utd";
        let is_cont = !is_member && Self::is_continuation(ev, ctx.prev);
        let color_idx = sender_color_index(sender_display(ev));

        let mut classes = vec!["msg"];
        if is_member {
            classes.push("sys");
        }
        if is_cont {
            classes.push("cont");
        }

        let mut out = String::new();
        out.push_str(&format!("<div class=\"{}\">", classes.join(" ")));

        if !is_member {
            if is_cont {
                out.push_str("<div></div>");
            } else if let Some(path) = ctx.avatar_path {
                out.push_str(&format!("<img class=\"avatar\" src=\"{}\" alt=\"\">", html_escape(path)));
            } else {
                out.push_str(&format!(
                    "<div class=\"avatar initials s{color_idx}\">{}</div>",
                    html_escape(&initial(sender_display(ev)))
                ));
            }
        }

        out.push_str("<div class=\"content\">");
        if !is_member && !is_cont {
            out.push_str(&format!(
                "<div class=\"head\"><span class=\"sender s{color_idx}\">{}</span><span class=\"ts\">{}</span></div>",
                html_escape(sender_display(ev)),
                format_hm(ev.timestamp)
            ));
        }

        if !ev.in_reply_to_id.is_empty() {
            if !ev.in_reply_to_sender_name.is_empty() {
                out.push_str(&format!(
                    "<div class=\"reply\"><span class=\"rsender\">{}</span><span class=\"rbody\">{}</span></div>",
                    html_escape(&ev.in_reply_to_sender_name),
                    html_escape(&ev.in_reply_to_body)
                ));
            } else {
                out.push_str(&format!(
                    "<div class=\"reply\">{}</div>",
                    html_escape(&labels.format(ExportLabel::ReplyTo, &[&ev.in_reply_to_body]))
                ));
            }
        }

        out.push_str(if is_placeholder { "<span class=\"body muted\">" } else { "<span class=\"body\">" });
        if is_placeholder {
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
        match attachment {
            AttachmentState::Saved(path) => {
                let (w, h) = media_display_size(ev);
                out.push_str(&format!(
                    "<div class=\"attachment\"><a href=\"{path}\" target=\"_blank\"><img src=\"{path}\" alt=\"{alt}\" width=\"{w:.0}\" height=\"{h:.0}\"></a></div>",
                    path = html_escape(path),
                    alt = html_escape(sender_display(ev)),
                    w = w, h = h,
                ));
            }
            // `TooLarge` can only ever be set for an image event (see
            // `mod.rs`'s image pass), so no `is_image_event` guard is
            // needed here the way the `None` arm below needs one.
            AttachmentState::TooLarge => {
                out.push_str(&format!(
                    "<div class=\"attachment\">{}</div>",
                    html_escape(&labels.format(ExportLabel::AttachmentSkipped, &[&image_display_name(ev)]))
                ));
            }
            AttachmentState::None if is_image_event(ev) => {
                out.push_str(&format!(
                    "<div class=\"attachment\">{}</div>",
                    html_escape(&labels.format(
                        ExportLabel::AttachmentUnavailable,
                        &[&image_display_name(ev)]
                    ))
                ));
            }
            AttachmentState::None => {}
        }
        if !ev.reactions.is_empty() {
            out.push_str("<div class=\"reactions\">");
            for r in &ev.reactions {
                let cls = if r.reacted_by_me { "chip me" } else { "chip" };
                out.push_str(&format!(
                    "<span class=\"{cls}\">{} {}</span>",
                    html_escape(&r.key),
                    r.count
                ));
            }
            out.push_str("</div>");
        }
        out.push_str("</div>"); // .content
        out.push_str("</div>\n"); // .msg
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

    #[test]
    fn fit_media_scales_down_preserving_aspect_ratio() {
        assert_eq!(fit_media(1600.0, 1000.0, 320.0, 200.0), (320.0, 200.0));
        assert_eq!(fit_media(3200.0, 1000.0, 320.0, 200.0), (320.0, 100.0)); // width-bound
        assert_eq!(fit_media(1600.0, 2000.0, 320.0, 200.0), (160.0, 200.0)); // height-bound
    }

    #[test]
    fn fit_media_never_upscales() {
        assert_eq!(fit_media(40.0, 20.0, 320.0, 200.0), (40.0, 20.0));
    }

    #[test]
    fn fit_media_falls_back_to_a_2_to_1_box_when_dimensions_are_unknown() {
        assert_eq!(fit_media(0.0, 0.0, 320.0, 200.0), (320.0, 100.0));
    }

    #[test]
    fn sender_color_index_is_deterministic() {
        assert_eq!(sender_color_index("Alice"), sender_color_index("Alice"));
        assert!(sender_color_index("Alice") < 8);
    }

    #[test]
    fn sender_color_index_spreads_distinct_names() {
        let names = ["Alice", "Bob", "Carol", "Dave", "Eve", "Frank", "Grace", "Heidi"];
        let indices: std::collections::HashSet<u8> = names.iter().map(|n| sender_color_index(n)).collect();
        assert!(indices.len() > 1, "8 distinct names all hashed to the same index");
    }

    #[test]
    fn initial_uppercases_first_character() {
        assert_eq!(initial("alice"), "A");
        assert_eq!(initial("Bob"), "B");
    }

    #[test]
    fn initial_empty_name_falls_back_to_question_mark() {
        assert_eq!(initial(""), "?");
    }

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
        t[ExportLabel::AttachmentSkipped as usize] = "Attachment too large: {0}".into();
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
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert_eq!(line, "[2020-01-01 00:00:00] Alice: hello world\n");
    }

    #[test]
    fn text_event_virtual_row_is_empty() {
        let mut ev = base_event();
        ev.msg_type = "virtual.date_divider".into();
        assert_eq!(TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels()), "");
    }

    #[test]
    fn text_event_redacted_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.redacted".into();
        ev.body = String::new();
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(line.contains("Message deleted"), "{line}");
    }

    #[test]
    fn text_event_undecryptable_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.utd".into();
        ev.body = "🔒 Sent before you joined this room".into();
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(line.contains("Unable to decrypt this message"), "{line}");
        assert!(!line.contains("Sent before you joined"), "{line}");
    }

    #[test]
    fn html_event_undecryptable_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.utd".into();
        ev.body = "🔒 Sent before you joined this room".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("Unable to decrypt this message"), "{out}");
        assert!(!out.contains("Sent before you joined"), "{out}");
        assert!(out.contains("class=\"body muted\""), "{out}");
    }

    #[test]
    fn text_event_edited_marker() {
        let mut ev = base_event();
        ev.is_edited = true;
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(line.trim_end().ends_with("(edited)"), "{line}");
    }

    #[test]
    fn text_event_reply_prefix() {
        let mut ev = base_event();
        ev.in_reply_to_id = "$0".into();
        ev.in_reply_to_body = "original message".into();
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
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
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
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
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
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
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(line.contains("Bob was invited by Alice"), "{line}");
    }

    #[test]
    fn text_event_attachment_saved_path() {
        let ev = base_event();
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/abc-photo.jpg"), &labels());
        assert!(line.contains("Attachment: media/abc-photo.jpg"), "{line}");
    }

    #[test]
    fn html_event_escapes_body() {
        let mut ev = base_event();
        ev.body = "<script>alert(1)</script>".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(!out.contains("<script>"), "{out}");
        assert!(out.contains("&lt;script&gt;"), "{out}");
    }

    #[test]
    fn html_event_prefers_sanitized_formatted_body() {
        let mut ev = base_event();
        ev.formatted_body = "<b>bold</b>".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("<b>bold</b>"), "{out}");
    }

    #[test]
    fn html_event_embeds_image_when_media_present() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/abc-photo.jpg"), &labels());
        assert!(out.contains("<img src=\"media/abc-photo.jpg\""), "{out}");
    }

    #[test]
    fn html_event_image_is_wrapped_in_a_click_through_link() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/abc-photo.jpg"), &labels());
        assert!(out.contains("<a href=\"media/abc-photo.jpg\" target=\"_blank\"><img"), "{out}");
    }

    #[test]
    fn html_event_image_uses_scaled_display_size_matching_the_live_timeline() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        // 1600x1000, 2x the 320x200 image cap on both axes — scales by 0.2.
        ev.width = 1600;
        ev.height = 1000;
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/abc-photo.jpg"), &labels());
        assert!(out.contains("width=\"320\" height=\"200\""), "{out}");
    }

    #[test]
    fn html_event_sticker_uses_the_square_sticker_cap() {
        let mut ev = base_event();
        ev.msg_type = "m.sticker".into();
        ev.width = 512;
        ev.height = 512;
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/sticker.png"), &labels());
        assert!(out.contains("width=\"256\" height=\"256\""), "{out}");
    }

    #[test]
    fn html_event_image_never_upscales_past_its_natural_size() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        ev.width = 40;
        ev.height = 20;
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::Saved("media/tiny.jpg"), &labels());
        assert!(out.contains("width=\"40\" height=\"20\""), "{out}");
    }

    #[test]
    fn html_event_image_unavailable_without_media() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        ev.image_filename = "photo.jpg".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("Image unavailable: photo.jpg"), "{out}");
    }

    #[test]
    fn html_event_attachment_too_large_uses_label() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        ev.image_filename = "photo.jpg".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::TooLarge, &labels());
        assert!(out.contains("Attachment too large: photo.jpg"), "{out}");
        assert!(!out.contains("Image unavailable"), "{out}");
    }

    #[test]
    fn text_event_attachment_too_large_is_silent() {
        let mut ev = base_event();
        ev.msg_type = "m.image".into();
        ev.image_filename = "photo.jpg".into();
        let line = TextSink.event(&ev, &EventContext::default(), AttachmentState::TooLarge, &labels());
        assert!(!line.contains("too large"), "{line}");
        assert!(!line.contains("Attachment"), "{line}");
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

    // ── grouping ─────────────────────────────────────────────────────────

    #[test]
    fn html_event_first_in_group_shows_avatar_and_header() {
        let ev = base_event();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("class=\"msg\""), "{out}");
        assert!(!out.contains("class=\"msg cont\""), "{out}");
        assert!(out.contains("class=\"head\""), "{out}");
        assert!(out.contains("avatar initials"), "{out}");
    }

    #[test]
    fn html_event_continuation_omits_avatar_and_header() {
        let prev = base_event();
        let mut ev = base_event();
        ev.event_id = "$2".into();
        ev.timestamp = prev.timestamp + 1000; // well within the grouping window
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"msg cont\""), "{out}");
        assert!(!out.contains("class=\"head\""), "{out}");
        assert!(!out.contains("avatar initials"), "{out}");
    }

    #[test]
    fn html_event_reply_always_starts_a_new_group() {
        let prev = base_event();
        let mut ev = base_event();
        ev.event_id = "$2".into();
        ev.timestamp = prev.timestamp + 1000;
        ev.in_reply_to_id = "$0".into();
        ev.in_reply_to_body = "earlier".into();
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"head\""), "a reply must not be a continuation: {out}");
    }

    #[test]
    fn html_event_virtual_predecessor_breaks_grouping() {
        let mut prev = base_event();
        prev.msg_type = "virtual.date_divider".into();
        let mut ev = base_event();
        ev.event_id = "$2".into();
        ev.timestamp = prev.timestamp + 1000;
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"head\""), "{out}");
    }

    #[test]
    fn html_event_gap_beyond_the_window_breaks_grouping() {
        let prev = base_event();
        let mut ev = base_event();
        ev.event_id = "$2".into();
        ev.timestamp = prev.timestamp + GROUP_INTERVAL_MS + 1;
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"head\""), "{out}");
    }

    #[test]
    fn html_event_different_sender_breaks_grouping() {
        let prev = base_event();
        let mut ev = base_event();
        ev.event_id = "$2".into();
        ev.sender = "@bob:example.org".into();
        ev.sender_name = "Bob".into();
        ev.timestamp = prev.timestamp + 1000;
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"head\""), "{out}");
    }

    #[test]
    fn html_event_membership_row_is_never_a_continuation() {
        let prev = base_event();
        let mut ev = base_event();
        ev.msg_type = "m.room.member".into();
        ev.membership_action = "joined".into();
        ev.membership_target_user_id = ev.sender.clone();
        ev.membership_target_name = ev.sender_name.clone();
        ev.timestamp = prev.timestamp + 1000;
        let ctx = EventContext { prev: Some(&prev), avatar_path: None };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("class=\"msg sys\""), "{out}");
        assert!(!out.contains("avatar"), "membership rows get no avatar cell: {out}");
    }

    // ── avatars ──────────────────────────────────────────────────────────

    #[test]
    fn html_event_no_avatar_path_renders_initials_fallback() {
        let ev = base_event();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("avatar initials"), "{out}");
        assert!(out.contains('A'), "{out}"); // Alice's initial
        assert!(!out.contains("<img class=\"avatar\""), "{out}");
    }

    #[test]
    fn html_event_avatar_path_renders_img() {
        let ev = base_event();
        let ctx = EventContext { prev: None, avatar_path: Some("media/alice-avatar.jpg") };
        let out = HtmlSink.event(&ev, &ctx, AttachmentState::None, &labels());
        assert!(out.contains("<img class=\"avatar\" src=\"media/alice-avatar.jpg\""), "{out}");
        assert!(!out.contains("avatar initials"), "{out}");
    }

    // ── reply quotes ─────────────────────────────────────────────────────

    #[test]
    fn html_event_reply_with_known_sender_renders_two_line_card() {
        let mut ev = base_event();
        ev.in_reply_to_id = "$0".into();
        ev.in_reply_to_sender_name = "Bob".into();
        ev.in_reply_to_body = "original message".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("class=\"rsender\">Bob<"), "{out}");
        assert!(out.contains("class=\"rbody\">original message<"), "{out}");
    }

    #[test]
    fn html_event_reply_with_unknown_sender_falls_back_to_label() {
        let mut ev = base_event();
        ev.in_reply_to_id = "$0".into();
        ev.in_reply_to_body = "original message".into();
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("In reply to original message"), "{out}");
        assert!(!out.contains("class=\"rsender\""), "{out}");
    }

    // ── reactions ────────────────────────────────────────────────────────

    #[test]
    fn html_event_reactions_render_one_chip_per_group() {
        let mut ev = base_event();
        ev.reactions = vec![
            crate::ffi::ReactionGroup { key: "\u{1F44D}".into(), count: 3, ..Default::default() },
            crate::ffi::ReactionGroup { key: "\u{2764}".into(), count: 1, reacted_by_me: true, ..Default::default() },
        ];
        let out = HtmlSink.event(&ev, &EventContext::default(), AttachmentState::None, &labels());
        assert!(out.contains("class=\"chip\">\u{1F44D} 3<"), "{out}");
        assert!(out.contains("class=\"chip me\">\u{2764} 1<"), "{out}");
    }
}
