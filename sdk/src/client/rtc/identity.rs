//! MSC4195 derived names: pseudonymous LiveKit room names and participant
//! identities, `Base64Unpadded(SHA256(CanonicalJSON([a, b])))`.

use std::collections::HashMap;
use std::sync::Arc;

use base64ct::{Base64Unpadded, Encoding};
use matrix_sdk::ruma::canonical_json::to_canonical_value;
use sha2::{Digest, Sha256};

fn derive(a: &str, b: &str) -> String {
    // A two-string array always serializes; the fallback is unreachable.
    let canonical = to_canonical_value([a, b]).map(|v| v.to_string()).unwrap_or_default();
    Base64Unpadded::encode_string(&Sha256::digest(canonical.as_bytes()))
}

/// The LiveKit room name for a Matrix room and slot: `[room_id, slot_id]`.
/// Servers derive it for tokens; we only use it to check what a token names.
pub fn livekit_room_name(room_id: &str, slot_id: &str) -> String {
    derive(room_id, slot_id)
}

/// The LiveKit participant identity of a member: `[sender, member.id]`.
pub fn livekit_participant_identity(sender: &str, member_id: &str) -> String {
    derive(sender, member_id)
}

/// What a connected LiveKit identity looks like next to the member we published.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IdentityShape {
    /// The hash of our user and member id: what MatrixRTC peers compute.
    Hashed,
    /// `{user_id}:{device_id}`, what older JWT services issue; peers parse it.
    Legacy,
    /// Neither: peers cannot tie the connection to our member event.
    Unexpected,
}

/// Classify the identity the SFU gave us against the id we published.
pub fn classify_identity(identity: &str, user_id: &str, device_id: &str, member_id: &str) -> IdentityShape {
    if identity == livekit_participant_identity(user_id, member_id) {
        IdentityShape::Hashed
    } else if identity == format!("{user_id}:{device_id}") {
        IdentityShape::Legacy
    } else {
        IdentityShape::Unexpected
    }
}

/// LiveKit identity -> `(user_id, device_id)` for members whose identity is
/// hashed (MSC4195) and so cannot be parsed. Shared by every connection of a
/// call. Two tasks write it, each from the member list it read itself (the
/// reconcile loop and the key handler), and their reads finish in any order, so
/// each takes a [`Stamp`] before it reads and installs with it: the newest
/// snapshot replaces the table, and an older one that arrives late can only add
/// what is missing, never take back a member a newer one has.
#[derive(Clone, Default)]
pub struct IdentityMap(Arc<parking_lot::Mutex<Identities>>);

#[derive(Default)]
struct Identities {
    issued: u64,
    /// The stamp of the snapshot the table was last replaced with.
    applied: u64,
    map: HashMap<String, (String, String)>,
}

/// The order a snapshot was started in; see [`IdentityMap::stamp`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stamp(u64);

impl IdentityMap {
    /// Take a stamp, before reading the members the snapshot is built from.
    pub fn stamp(&self) -> Stamp {
        let mut g = self.0.lock();
        g.issued += 1;
        Stamp(g.issued)
    }

    /// Install the snapshot started at `stamp`. Returns whether it replaced the
    /// table (it is the newest so far) or, being older, only added to it.
    pub fn replace_stamped(&self, stamp: Stamp, map: HashMap<String, (String, String)>) -> bool {
        let mut g = self.0.lock();
        if stamp.0 >= g.applied {
            g.applied = stamp.0;
            g.map = map;
            true
        } else {
            for (identity, owner) in map {
                g.map.entry(identity).or_insert(owner);
            }
            false
        }
    }

    /// Install a snapshot as the newest one, for a caller whose read is not
    /// racing another writer's.
    pub fn replace(&self, map: HashMap<String, (String, String)>) {
        let stamp = self.stamp();
        self.replace_stamped(stamp, map);
    }

    pub fn resolve(&self, identity: &str) -> Option<(String, String)> {
        self.0.lock().map.get(identity).cloned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn room_name_matches_the_msc_test_vector() {
        assert_eq!(
            livekit_room_name("!roomid:example.com", "slot1234"),
            "O8437W3+jmzMVjoIP3tNwbm+XxHQk2iKpOA7aqw3qSc"
        );
    }

    #[test]
    fn participant_identity_matches_the_msc_test_vector() {
        assert_eq!(
            livekit_participant_identity("@alice:example.com", "memberABC"),
            "M3VnsLXrkbxIDINXO64u8PZzFyD9ZYFiQULR2dshWYs"
        );
    }

    #[test]
    fn non_ascii_and_escapes_are_canonical() {
        assert_eq!(
            livekit_participant_identity("@ü:x", "é\"\\"),
            "2aBJOe7ychR0N9DFEZlQ0T51rdcm4xgaSpDj1YSg4rA"
        );
    }

    #[test]
    fn identity_depends_on_both_inputs() {
        let a = livekit_participant_identity("@a:x", "m1");
        assert_ne!(a, livekit_participant_identity("@b:x", "m1"));
        assert_ne!(a, livekit_participant_identity("@a:x", "m2"));
    }

    #[test]
    fn the_connected_identity_is_checked_against_the_published_member_id() {
        let hashed = livekit_participant_identity("@a:x", "mid");
        assert_eq!(classify_identity(&hashed, "@a:x", "D", "mid"), IdentityShape::Hashed);
        assert_eq!(classify_identity("@a:x:D", "@a:x", "D", "mid"), IdentityShape::Legacy);
        // The hash of a different id (the token was requested under another one).
        let other = livekit_participant_identity("@a:x", "other");
        assert_eq!(classify_identity(&other, "@a:x", "D", "mid"), IdentityShape::Unexpected);
        assert_eq!(classify_identity("", "@a:x", "D", "mid"), IdentityShape::Unexpected);
    }

    #[test]
    fn identity_map_resolves_and_replaces() {
        let m = IdentityMap::default();
        assert_eq!(m.resolve("x"), None);
        m.replace(HashMap::from([("x".to_owned(), ("@a:x".to_owned(), "D".to_owned()))]));
        assert_eq!(m.resolve("x"), Some(("@a:x".to_owned(), "D".to_owned())));
        m.replace(HashMap::new());
        assert_eq!(m.resolve("x"), None);
    }

    #[test]
    fn a_late_older_snapshot_cannot_take_a_new_member_back() {
        let owner = |u: &str| (u.to_owned(), "D".to_owned());
        let m = IdentityMap::default();
        // The key handler starts first but its read is slow; the reconcile loop
        // starts after it, reads, and installs a snapshot with a new member.
        let slow = m.stamp();
        let fast = m.stamp();
        assert!(m.replace_stamped(fast, HashMap::from([("new".to_owned(), owner("@n:x")), ("old".to_owned(), owner("@o:x"))])));
        // The slow one lands later with the picture from before the join.
        assert!(!m.replace_stamped(slow, HashMap::from([("old".to_owned(), owner("@o:x")), ("gone".to_owned(), owner("@g:x"))])));
        assert_eq!(m.resolve("new"), Some(owner("@n:x")), "the newer snapshot's member stays");
        assert_eq!(m.resolve("old"), Some(owner("@o:x")));
        // It may still add what the newer one does not know yet (a read that
        // finished later but was stamped earlier), which the next replace clears.
        assert_eq!(m.resolve("gone"), Some(owner("@g:x")));
        let next = m.stamp();
        assert!(m.replace_stamped(next, HashMap::new()));
        assert_eq!(m.resolve("gone"), None);
        assert_eq!(m.resolve("new"), None);
    }

    #[test]
    fn the_newest_snapshot_replaces_and_removes() {
        let m = IdentityMap::default();
        let first = m.stamp();
        assert!(m.replace_stamped(first, HashMap::from([("a".to_owned(), ("@a:x".to_owned(), "D".to_owned()))])));
        let second = m.stamp();
        assert!(m.replace_stamped(second, HashMap::from([("b".to_owned(), ("@b:x".to_owned(), "D".to_owned()))])));
        assert_eq!(m.resolve("a"), None);
        assert!(m.resolve("b").is_some());
    }
}
