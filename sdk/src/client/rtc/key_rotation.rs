//! When to rotate our MatrixRTC frame key, and when to start using the new one
//! (MSC4143 "Rotating keys"). Pure state machine: callers pass the clock in and
//! execute the returned actions (send to-device keys, set the LiveKit key).
//!
//! Joins do **not** rotate, like Element Call: a joiner is sent the key in use.
//! Rotating for a joiner left it unable to read the key still in use for the
//! activation delay, and LiveKit gives up on a participant's key after a few
//! failed decryptions, so the joiner then never recovered. The price is that a
//! joiner can decrypt earlier media of the key in use if it was recorded;
//! rotation on leaves and over time still protects what comes after.

use std::collections::HashSet;
use std::time::{Duration, Instant};

use super::e2ee::E2eeManager;

/// A new key is sent first and only used for encryption after this delay, so
/// receivers have it by the time frames carry its index (MSC4143: 5 s).
pub const ACTIVATION_DELAY: Duration = Duration::from_secs(5);
/// Rotate even when membership is stable (the MSC asks for periodic rotation
/// without giving a period).
pub const PERIODIC_ROTATION: Duration = Duration::from_secs(30 * 60);

type Member = (String, String);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RotationAction {
    /// Send the key (base64 raw material, `index`) to these `(user, device)`s.
    Send { index: u8, key_b64: String, to: Vec<Member> },
    /// Start encrypting our media with this key.
    Activate { index: u8, raw_key: [u8; 32] },
}

pub struct KeyRotator {
    mgr: E2eeManager,
    /// The key we are encrypting with right now. It differs from the newest
    /// key (`mgr`) while a rotation waits to activate.
    active_index: u8,
    active_b64: String,
    members: HashSet<Member>,
    /// Time of the last rotation (or of creation).
    last_rotation: Instant,
    /// When the newest sent key becomes the encryption key, if it is not yet.
    activate_at: Option<Instant>,
    /// Someone left while a rotation was still waiting to activate: rotate once
    /// more when it does, instead of once per leaver.
    dirty: bool,
}

fn sorted(members: &HashSet<Member>) -> Vec<Member> {
    let mut v: Vec<Member> = members.iter().cloned().collect();
    v.sort();
    v
}

impl KeyRotator {
    pub fn new(members: HashSet<Member>, now: Instant) -> Self {
        let mgr = E2eeManager::new();
        let (active_index, active_b64) = (mgr.own_index(), mgr.own_key_b64());
        Self { mgr, active_index, active_b64, members, last_rotation: now, activate_at: None, dirty: false }
    }

    /// The key in use from the start: `(index, base64, raw)`.
    pub fn initial(&self) -> (u8, String, [u8; 32]) {
        (self.mgr.own_index(), self.mgr.own_key_b64(), *self.mgr.own_raw_key())
    }

    fn rotate(&mut self, now: Instant) -> RotationAction {
        let (index, key_b64) = self.mgr.rotate();
        self.last_rotation = now;
        self.activate_at = Some(now + ACTIVATION_DELAY);
        RotationAction::Send { index, key_b64, to: sorted(&self.members) }
    }

    /// What a joiner needs: the key in use now, and the newer one if a rotation
    /// is waiting to activate.
    fn keys_for_joiners(&self, joined: &[Member]) -> Vec<RotationAction> {
        let mut out = vec![RotationAction::Send {
            index: self.active_index,
            key_b64: self.active_b64.clone(),
            to: joined.to_vec(),
        }];
        if self.mgr.own_index() != self.active_index {
            out.push(RotationAction::Send {
                index: self.mgr.own_index(),
                key_b64: self.mgr.own_key_b64(),
                to: joined.to_vec(),
            });
        }
        out
    }

    /// Feed the current live member set; returns what to do about the change.
    pub fn on_members_changed(&mut self, current: &HashSet<Member>, now: Instant) -> Vec<RotationAction> {
        let joined: Vec<Member> = {
            let mut j: Vec<Member> = current.difference(&self.members).cloned().collect();
            j.sort();
            j
        };
        let left = self.members.difference(current).next().is_some();
        self.members = current.clone();
        if !left && joined.is_empty() {
            return Vec::new();
        }
        if !left {
            return self.keys_for_joiners(&joined);
        }
        // Someone left: the remaining members get a new key.
        if self.activate_at.is_some() {
            self.dirty = true;
            return if joined.is_empty() { Vec::new() } else { self.keys_for_joiners(&joined) };
        }
        let mut out = vec![self.rotate(now)];
        if !joined.is_empty() {
            // The new key reaches the joiner too, but it still has to read the old one meanwhile.
            out.push(RotationAction::Send {
                index: self.active_index,
                key_b64: self.active_b64.clone(),
                to: joined,
            });
        }
        out
    }

    /// Time-driven work: activation, the deferred rotation, periodic rotation.
    pub fn poll(&mut self, now: Instant) -> Vec<RotationAction> {
        let mut out = Vec::new();
        if let Some(at) = self.activate_at {
            if now >= at {
                self.activate_at = None;
                self.active_index = self.mgr.own_index();
                self.active_b64 = self.mgr.own_key_b64();
                out.push(RotationAction::Activate {
                    index: self.active_index,
                    raw_key: *self.mgr.own_raw_key(),
                });
                if self.dirty {
                    self.dirty = false;
                    out.push(self.rotate(now));
                    return out;
                }
            }
        }
        if self.activate_at.is_none()
            && now.saturating_duration_since(self.last_rotation) >= PERIODIC_ROTATION
        {
            out.push(self.rotate(now));
        }
        out
    }

    /// The next instant `poll` has work to do.
    pub fn next_deadline(&self) -> Option<Instant> {
        Some(self.activate_at.unwrap_or(self.last_rotation + PERIODIC_ROTATION))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;
    use std::time::{Duration, Instant};

    fn m(user: &str) -> (String, String) {
        (user.to_owned(), "DEV".to_owned())
    }

    fn set(users: &[&str]) -> HashSet<(String, String)> {
        users.iter().map(|u| m(u)).collect()
    }

    fn sends(actions: &[RotationAction]) -> Vec<(u8, Vec<(String, String)>)> {
        actions
            .iter()
            .filter_map(|a| match a {
                RotationAction::Send { index, to, .. } => Some((*index, to.clone())),
                _ => None,
            })
            .collect()
    }

    fn activations(actions: &[RotationAction]) -> Vec<u8> {
        actions
            .iter()
            .filter_map(|a| match a {
                RotationAction::Activate { index, .. } => Some(*index),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn initial_key_is_index_zero() {
        let t0 = Instant::now();
        let r = KeyRotator::new(set(&["@a:x"]), t0);
        let (idx, b64, raw) = r.initial();
        assert_eq!(idx, 0);
        assert!(!b64.is_empty());
        assert_eq!(raw.len(), 32);
    }

    #[test]
    fn nothing_changes_nothing_happens() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        assert!(r.on_members_changed(&set(&["@a:x", "@b:x"]), t0).is_empty());
        assert!(r.poll(t0 + Duration::from_secs(1)).is_empty());
    }

    #[test]
    fn leave_sends_new_key_but_activates_after_5s() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        let acts = r.on_members_changed(&set(&["@a:x"]), now);
        assert_eq!(sends(&acts), vec![(1, vec![m("@a:x")])]);
        assert!(activations(&acts).is_empty(), "must not use the new key yet");
        assert!(r.poll(now + Duration::from_secs(4)).is_empty());
        let later = r.poll(now + ACTIVATION_DELAY);
        assert_eq!(activations(&later), vec![1]);
    }

    #[test]
    fn leaves_within_window_coalesce_into_one_rotation() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x", "@c:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        let first = r.on_members_changed(&set(&["@a:x", "@c:x"]), now);
        assert_eq!(sends(&first), vec![(1, vec![m("@a:x"), m("@c:x")])]);
        // Another leave inside the window: no immediate second rotation.
        let second = r.on_members_changed(&set(&["@a:x"]), now + Duration::from_secs(2));
        assert!(sends(&second).is_empty());
        // The window ends: key 1 goes live and ONE more rotation covers both leavers.
        let end = r.poll(now + ACTIVATION_DELAY);
        assert_eq!(activations(&end), vec![1]);
        assert_eq!(sends(&end), vec![(2, vec![m("@a:x")])]);
        assert_eq!(activations(&r.poll(now + ACTIVATION_DELAY * 2)), vec![2]);
        assert!(r.poll(now + ACTIVATION_DELAY * 3).is_empty());
    }

    #[test]
    fn a_join_sends_the_key_in_use_and_does_not_rotate() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x"]), t0);
        let acts = r.on_members_changed(&set(&["@a:x", "@b:x"]), t0 + Duration::from_secs(3));
        assert_eq!(sends(&acts), vec![(0, vec![m("@b:x")])]);
        assert!(r.poll(t0 + Duration::from_secs(60)).is_empty(), "nothing is pending");
    }

    #[test]
    fn a_late_join_does_not_rotate_either() {
        // Rotating for a joiner would leave it unable to read the key still in
        // use, and LiveKit stops trying a participant's key after a few failures.
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x"]), t0);
        let acts = r.on_members_changed(&set(&["@a:x", "@b:x"]), t0 + Duration::from_secs(20 * 60));
        assert_eq!(sends(&acts), vec![(0, vec![m("@b:x")])]);
        assert!(activations(&acts).is_empty());
    }

    #[test]
    fn a_joiner_during_a_pending_rotation_gets_both_keys() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        r.on_members_changed(&set(&["@a:x"]), now); // b leaves: key 1 pending
        let acts = r.on_members_changed(&set(&["@a:x", "@c:x"]), now + Duration::from_secs(2));
        // The key in use first (so it can read us now), then the one coming.
        assert_eq!(sends(&acts), vec![(0, vec![m("@c:x")]), (1, vec![m("@c:x")])]);
    }

    #[test]
    fn after_activation_a_joiner_only_needs_the_new_key() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        r.on_members_changed(&set(&["@a:x"]), now);
        r.poll(now + ACTIVATION_DELAY);
        let acts = r.on_members_changed(&set(&["@a:x", "@c:x"]), now + ACTIVATION_DELAY + Duration::from_secs(1));
        assert_eq!(sends(&acts), vec![(1, vec![m("@c:x")])]);
    }

    #[test]
    fn a_join_together_with_a_leave_rotates_and_still_gets_the_key_in_use() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        let acts = r.on_members_changed(&set(&["@a:x", "@c:x"]), now);
        // The leaver is excluded from the new key; the joiner gets it plus the old one.
        assert_eq!(sends(&acts), vec![(1, vec![m("@a:x"), m("@c:x")]), (0, vec![m("@c:x")])]);
        assert!(r.poll(now + Duration::from_secs(1)).is_empty(), "full activation delay");
        assert_eq!(activations(&r.poll(now + ACTIVATION_DELAY)), vec![1]);
    }

    #[test]
    fn periodic_rotation_fires_after_interval() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x"]), t0);
        assert!(r.poll(t0 + PERIODIC_ROTATION - Duration::from_secs(1)).is_empty());
        let acts = r.poll(t0 + PERIODIC_ROTATION);
        assert_eq!(sends(&acts), vec![(1, vec![m("@a:x")])]);
        assert!(r.next_deadline().is_some());
    }

    #[test]
    fn index_wraps_255_to_0() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x"]), t0);
        let mut last = None;
        for i in 0..256u32 {
            let acts = r.poll(t0 + PERIODIC_ROTATION * (i + 1));
            if let Some((idx, _)) = sends(&acts).into_iter().next() {
                last = Some(idx);
            }
        }
        assert_eq!(last, Some(0));
    }
}
