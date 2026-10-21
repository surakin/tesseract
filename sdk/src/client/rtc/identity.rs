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

/// LiveKit identity -> `(user_id, device_id)` for members whose identity is
/// hashed (MSC4195) and so cannot be parsed. Shared by every connection of a
/// call and refreshed by the member reconciler.
#[derive(Clone, Default)]
pub struct IdentityMap(Arc<parking_lot::Mutex<HashMap<String, (String, String)>>>);

impl IdentityMap {
    pub fn replace(&self, map: HashMap<String, (String, String)>) {
        *self.0.lock() = map;
    }

    pub fn resolve(&self, identity: &str) -> Option<(String, String)> {
        self.0.lock().get(identity).cloned()
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
    fn identity_map_resolves_and_replaces() {
        let m = IdentityMap::default();
        assert_eq!(m.resolve("x"), None);
        m.replace(HashMap::from([("x".to_owned(), ("@a:x".to_owned(), "D".to_owned()))]));
        assert_eq!(m.resolve("x"), Some(("@a:x".to_owned(), "D".to_owned())));
        m.replace(HashMap::new());
        assert_eq!(m.resolve("x"), None);
    }
}
