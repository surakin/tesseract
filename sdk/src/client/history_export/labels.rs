//! Fixed-order label templates for room-history export prose.
//!
//! Rust never composes English text for anything user-visible (see the
//! convention documented at `membership_action_str` in
//! `client/timeline_convert.rs`, and `MessageListView.cpp:828` on the C++
//! side). For exports, `HistoryExportController::build_labels()` supplies a
//! fixed-order `Vec<String>` of `tk::tr`/`tk::trf` templates across the FFI
//! (`RoomExportOptionsFfi::labels`); this module only substitutes `{0}`,
//! `{1}`, … placeholders — it never invents wording of its own.
//!
//! The membership entries deliberately match
//! `MessageListView.cpp::membership_expanded_phrase`'s exact wording
//! (including its by-actor/no-actor split) rather than inventing new
//! phrasing, so: translators aren't asked to translate the same sentiment
//! twice, and an exported room's history reads consistently with what the
//! live view already showed for the same events. One case doesn't
//! transfer cleanly: `KnockRetracted`'s live phrasing embeds a possessive
//! pronoun (`m.target_pronoun`) this export pipeline has no data for, so it
//! falls back to a pronoun-free passive form instead.

/// Index into the `labels` vector supplied by C++. Order must match
/// `HistoryExportController::build_labels()` on the C++ side exactly.
/// `ExportLabel::COUNT` plus this module's own tests guard the Rust side;
/// the C++ side is covered by its own label-table-completeness test.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(usize)]
pub(super) enum ExportLabel {
    /// "History of {0}" — {0} = room display name.
    HeaderTitle = 0,
    /// "Exported {0}" — {0} = pre-formatted export timestamp.
    ExportedOn = 1,
    /// "Unable to decrypt this message" — no args.
    UnableToDecrypt = 2,
    /// "Message deleted" — no args.
    Redacted = 3,
    /// "(edited)" — no args.
    Edited = 4,
    /// "Attachment: {0}" — {0} = relative path of a saved attachment.
    AttachmentSaved = 5,
    /// "Attachment too large: {0}" — {0} = original filename.
    AttachmentSkipped = 6,
    /// "Image unavailable: {0}" — {0} = original filename.
    AttachmentUnavailable = 7,
    /// "In reply to {0}" — {0} = short snippet of the replied-to message.
    ReplyTo = 8,
    /// "Reactions: {0}" — {0} = pre-joined "👍 3, ❤ 1" summary.
    ReactionsLine = 9,
    /// "{0} joined the room" — {0} = target display name.
    MembershipJoined = 10,
    /// "{0} left the room" — {0} = target display name.
    MembershipLeft = 11,
    /// "{0} was banned by {1}" — {0} = target, {1} = actor.
    MembershipBannedByActor = 12,
    /// "{0} was banned" — {0} = target.
    MembershipBannedNoActor = 13,
    /// "{0} was unbanned by {1}" — {0} = target, {1} = actor.
    MembershipUnbannedByActor = 14,
    /// "{0} is no longer banned" — {0} = target.
    MembershipUnbannedNoActor = 15,
    /// "{0} was removed by {1}" — {0} = target, {1} = actor.
    MembershipKickedByActor = 16,
    /// "{0} was removed" — {0} = target.
    MembershipKickedNoActor = 17,
    /// "{0} was invited by {1}" — {0} = target, {1} = actor.
    MembershipInvitedByActor = 18,
    /// "{0} received an invitation" — {0} = target.
    MembershipInvitedNoActor = 19,
    /// "{0} was removed and banned by {1}" — {0} = target, {1} = actor.
    MembershipKickedAndBannedByActor = 20,
    /// "{0} was kicked and banned" — {0} = target.
    MembershipKickedAndBannedNoActor = 21,
    /// "{0} has accepted the invitation" — {0} = target.
    MembershipInvitationAccepted = 22,
    /// "{0} has rejected the invitation" — {0} = target.
    MembershipInvitationRejected = 23,
    /// "{0}'s invitation was revoked by {1}" — {0} = target, {1} = actor.
    MembershipInvitationRevokedByActor = 24,
    /// "{0}'s invitation was revoked" — {0} = target.
    MembershipInvitationRevokedNoActor = 25,
    /// "{0} requested to join" — {0} = target.
    MembershipKnocked = 26,
    /// "{0}'s request to join was approved by {1}" — {0} = target, {1} = actor.
    MembershipKnockAcceptedByActor = 27,
    /// "{0}'s join request was approved" — {0} = target.
    MembershipKnockAcceptedNoActor = 28,
    /// "{0}'s request to join was withdrawn" — {0} = target. Pronoun-free
    /// fallback for the live view's "{0} withdrew {1} request to join"
    /// (which needs a possessive pronoun this pipeline doesn't have).
    MembershipKnockRetracted = 29,
    /// "{0}'s request to join was denied by {1}" — {0} = target, {1} = actor.
    MembershipKnockDeniedByActor = 30,
    /// "{0}'s join request was denied" — {0} = target.
    MembershipKnockDeniedNoActor = 31,
}

impl ExportLabel {
    pub(super) const COUNT: usize = 32;
}

/// The two label slots for one membership action that has both an
/// actor-driven and a no-actor phrasing.
pub(super) struct MembershipPair {
    pub(super) by_actor: ExportLabel,
    pub(super) no_actor: ExportLabel,
}

/// Resolved label(s) for one `TimelineEvent::membership_action` value —
/// either a single label (the action has only one phrasing, e.g. "joined")
/// or a by-actor/no-actor pair (e.g. "banned"), mirroring
/// `membership_expanded_phrase`'s `by_actor` branch exactly.
pub(super) enum MembershipLabels {
    Single(ExportLabel),
    Pair(MembershipPair),
}

/// Maps `TimelineEvent::membership_action`'s stable discriminant string
/// (never English prose — see the field doc in `bridge.rs`) to the
/// label(s) that render it. Returns `None` for an unrecognized/empty
/// discriminant.
pub(super) fn membership_labels(action: &str) -> Option<MembershipLabels> {
    use ExportLabel::*;
    use MembershipLabels::{Pair as P, Single as S};
    Some(match action {
        "joined" => S(MembershipJoined),
        "left" => S(MembershipLeft),
        "banned" => P(MembershipPair { by_actor: MembershipBannedByActor, no_actor: MembershipBannedNoActor }),
        "unbanned" => P(MembershipPair { by_actor: MembershipUnbannedByActor, no_actor: MembershipUnbannedNoActor }),
        "kicked" => P(MembershipPair { by_actor: MembershipKickedByActor, no_actor: MembershipKickedNoActor }),
        "invited" => P(MembershipPair { by_actor: MembershipInvitedByActor, no_actor: MembershipInvitedNoActor }),
        "kicked_and_banned" => P(MembershipPair {
            by_actor: MembershipKickedAndBannedByActor,
            no_actor: MembershipKickedAndBannedNoActor,
        }),
        "invitation_accepted" => S(MembershipInvitationAccepted),
        "invitation_rejected" => S(MembershipInvitationRejected),
        "invitation_revoked" => P(MembershipPair {
            by_actor: MembershipInvitationRevokedByActor,
            no_actor: MembershipInvitationRevokedNoActor,
        }),
        "knocked" => S(MembershipKnocked),
        "knock_accepted" => P(MembershipPair {
            by_actor: MembershipKnockAcceptedByActor,
            no_actor: MembershipKnockAcceptedNoActor,
        }),
        "knock_retracted" => S(MembershipKnockRetracted),
        "knock_denied" => P(MembershipPair {
            by_actor: MembershipKnockDeniedByActor,
            no_actor: MembershipKnockDeniedNoActor,
        }),
        _ => return None,
    })
}

/// Substitutes `{0}`, `{1}`, … in `template` with `args`, in order. An
/// out-of-range index, or a `{` that isn't a well-formed `{N}` run, is
/// emitted back literally rather than panicking — a version-skew or
/// malformed template should degrade visibly in the exported file, not
/// crash the export.
pub(super) fn interpolate(template: &str, args: &[&str]) -> String {
    let mut out = String::with_capacity(template.len());
    let mut chars = template.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '{' {
            out.push(c);
            continue;
        }
        let mut digits = String::new();
        while let Some(&d) = chars.peek() {
            if !d.is_ascii_digit() {
                break;
            }
            digits.push(d);
            chars.next();
        }
        if !digits.is_empty() && chars.peek() == Some(&'}') {
            chars.next(); // consume the closing '}'
            match digits.parse::<usize>().ok().and_then(|i| args.get(i)) {
                Some(v) => out.push_str(v),
                None => {
                    out.push('{');
                    out.push_str(&digits);
                    out.push('}');
                }
            }
        } else {
            out.push('{');
            out.push_str(&digits);
        }
    }
    out
}

/// Holds one export run's label templates, indexed by `ExportLabel`.
pub(super) struct Labels {
    templates: Vec<String>,
}

impl Labels {
    /// Pads/truncates `templates` to exactly `ExportLabel::COUNT` entries so
    /// a version-skewed caller (old C++ build, new Rust crate, or vice
    /// versa) degrades to blank labels for the entries it doesn't know
    /// about, rather than panicking on an out-of-bounds index.
    pub(super) fn new(mut templates: Vec<String>) -> Self {
        templates.resize(ExportLabel::COUNT, String::new());
        Self { templates }
    }

    pub(super) fn get(&self, label: ExportLabel) -> &str {
        &self.templates[label as usize]
    }

    pub(super) fn format(&self, label: ExportLabel, args: &[&str]) -> String {
        interpolate(self.get(label), args)
    }

    /// Formats a membership row given its resolved label(s), `target`
    /// display name, and `actor` display name — `by_actor` (sender !=
    /// target) selects the pair's by-actor variant when present.
    pub(super) fn format_membership(
        &self,
        labels: MembershipLabels,
        by_actor: bool,
        target: &str,
        actor: &str,
    ) -> String {
        match labels {
            MembershipLabels::Single(label) => self.format(label, &[target]),
            MembershipLabels::Pair(pair) => {
                if by_actor {
                    self.format(pair.by_actor, &[target, actor])
                } else {
                    self.format(pair.no_actor, &[target])
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn interpolate_substitutes_in_order() {
        assert_eq!(
            interpolate("{0} joined the room", &["Alice"]),
            "Alice joined the room"
        );
        assert_eq!(
            interpolate("{0} was invited by {1}", &["Bob", "Alice"]),
            "Bob was invited by Alice"
        );
    }

    #[test]
    fn interpolate_leaves_unknown_index_literal() {
        assert_eq!(interpolate("{5} missing", &["a"]), "{5} missing");
    }

    #[test]
    fn interpolate_no_placeholders_passthrough() {
        assert_eq!(interpolate("(edited)", &[]), "(edited)");
    }

    #[test]
    fn interpolate_handles_unterminated_brace() {
        assert_eq!(interpolate("a {0 b", &["x"]), "a {0 b");
        assert_eq!(interpolate("trailing {", &[]), "trailing {");
    }

    #[test]
    fn interpolate_repeats_same_arg() {
        assert_eq!(interpolate("{0}-{0}", &["x"]), "x-x");
    }

    #[test]
    fn labels_short_input_is_padded_not_panicking() {
        let labels = Labels::new(vec!["History of {0}".to_string()]);
        assert_eq!(
            labels.format(ExportLabel::HeaderTitle, &["Room"]),
            "History of Room"
        );
        assert_eq!(labels.get(ExportLabel::Edited), "");
    }

    #[test]
    fn labels_long_input_is_truncated_not_panicking() {
        let templates = vec![String::new(); ExportLabel::COUNT + 10];
        let labels = Labels::new(templates);
        assert_eq!(labels.get(ExportLabel::MembershipKnockDeniedNoActor), "");
    }

    #[test]
    fn labels_format_by_index() {
        let mut templates = vec![String::new(); ExportLabel::COUNT];
        templates[ExportLabel::Edited as usize] = "(edited)".to_string();
        let labels = Labels::new(templates);
        assert_eq!(labels.format(ExportLabel::Edited, &[]), "(edited)");
    }

    fn full_labels() -> Labels {
        // One distinctive template per slot, keyed by index, so a
        // format_membership test can assert exactly which slot fired.
        let mut t = vec![String::new(); ExportLabel::COUNT];
        for (i, s) in t.iter_mut().enumerate() {
            *s = format!("SLOT{i}:{{0}}");
        }
        // Pair slots need a second placeholder to exercise the actor arg.
        for i in [12, 14, 16, 18, 20, 24, 27, 30] {
            t[i] = format!("SLOT{i}:{{0}}:{{1}}");
        }
        Labels::new(t)
    }

    #[test]
    fn membership_labels_maps_all_known_discriminants() {
        let known = [
            "joined",
            "left",
            "banned",
            "unbanned",
            "kicked",
            "invited",
            "kicked_and_banned",
            "invitation_accepted",
            "invitation_rejected",
            "invitation_revoked",
            "knocked",
            "knock_accepted",
            "knock_retracted",
            "knock_denied",
        ];
        for action in known {
            assert!(membership_labels(action).is_some(), "missing mapping for {action}");
        }
    }

    #[test]
    fn membership_labels_unknown_is_none() {
        assert!(membership_labels("").is_none());
        assert!(membership_labels("something_new").is_none());
    }

    #[test]
    fn format_membership_single_ignores_by_actor() {
        let labels = full_labels();
        let single = membership_labels("joined").unwrap();
        assert_eq!(
            labels.format_membership(single, true, "Alice", "Bob"),
            "SLOT10:Alice"
        );
    }

    #[test]
    fn format_membership_pair_by_actor_uses_both_args() {
        let labels = full_labels();
        let pair = membership_labels("banned").unwrap();
        assert_eq!(
            labels.format_membership(pair, true, "Bob", "Alice"),
            "SLOT12:Bob:Alice"
        );
    }

    #[test]
    fn format_membership_pair_no_actor_uses_target_only() {
        let labels = full_labels();
        let pair = membership_labels("banned").unwrap();
        assert_eq!(labels.format_membership(pair, false, "Bob", "Alice"), "SLOT13:Bob");
    }
}
