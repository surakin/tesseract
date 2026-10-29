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
//!
//! A device that restarts and rejoins while its old membership is still live is
//! the same `(user, device)` under a new session ([`CallMember::session_tag`]):
//! it is a joiner (it lost the key in use) and not a leaver.
//!
//! A key counts as in use only once the caller says it put it to use
//! ([`KeyRotator::activated`]); until then a joiner gets both the key we encrypt
//! with and the one coming, however long the activation is held back.

use std::collections::HashMap;
use std::time::{Duration, Instant};

use super::e2ee::{E2eeManager, Secret, SecretKey, SecretString};

/// A new key is sent first and only used for encryption after this delay, so
/// receivers have it by the time frames carry its index (MSC4143: 5 s).
///
/// By design, someone who leaves (or is removed) can still decrypt our media
/// until the new key is in use: this delay plus, at most, [`MAX_ACTIVATION_WAIT`]
/// after the rotation. Switching sooner would cut off members who do not have
/// the new key yet, and the leaver heard the call up to the moment of leaving
/// anyway; what the rotation protects is everything after that window.
pub const ACTIVATION_DELAY: Duration = Duration::from_secs(5);
/// Rotate even when membership is stable (the MSC asks for periodic rotation
/// without giving a period).
pub const PERIODIC_ROTATION: Duration = Duration::from_secs(30 * 60);

type Member = (String, String);
/// The live members with the session tag each one joined under.
pub type Sessions = HashMap<Member, String>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RotationAction {
    /// Send the key (base64 raw material, `index`) to these `(user, device)`s.
    Send { index: u8, key_b64: SecretString, to: Vec<Member> },
    /// Start encrypting our media with this key.
    Activate { index: u8, raw_key: SecretKey },
}

/// A new key waits for its delivery to settle before it is put to use, but not
/// longer than this after [`ACTIVATION_DELAY`]: the delivery starts when the key
/// is made, so by then it has had the delay already, and a recipient that is
/// still not served must not keep the call on the old key, which the people who
/// left still hold. A leaver can therefore read our media for at most
/// `ACTIVATION_DELAY + MAX_ACTIVATION_WAIT` (8 s) after the rotation, against 5 s
/// without the gate.
pub const MAX_ACTIVATION_WAIT: Duration = Duration::from_secs(3);

/// Holds an activation back until the key it activates is settled (delivered to
/// everyone, or owed only to recipients that cannot be reached) or
/// [`MAX_ACTIVATION_WAIT`] has passed.
#[derive(Default)]
pub struct ActivationGate {
    held: Option<(u8, SecretKey, Instant)>,
}

impl ActivationGate {
    /// Hold the activation of `index`; a newer one replaces an older still held.
    pub fn hold(&mut self, index: u8, raw_key: SecretKey, now: Instant) {
        self.held = Some((index, raw_key, now + MAX_ACTIVATION_WAIT));
    }

    /// The activation to apply now, if its key is settled (`settled(index)`)
    /// or the wait is over.
    pub fn release(&mut self, settled: impl Fn(u8) -> bool, now: Instant) -> Option<(u8, SecretKey)> {
        let (index, _, give_up_at) = self.held.as_ref()?;
        if settled(*index) || now >= *give_up_at {
            return self.held.take().map(|(i, k, _)| (i, k));
        }
        None
    }

    /// When the held activation is applied at the latest.
    pub fn deadline(&self) -> Option<Instant> {
        self.held.as_ref().map(|(_, _, at)| *at)
    }
}

pub struct KeyRotator {
    mgr: E2eeManager,
    /// The key we are encrypting with right now. It differs from the newest
    /// key (`mgr`) from a rotation until the caller confirms the activation.
    active_index: u8,
    active_b64: SecretString,
    members: Sessions,
    /// Time of the last rotation (or of creation).
    last_rotation: Instant,
    /// When the activation of the newest key is due, until it is asked for.
    activate_at: Option<Instant>,
    /// Someone left while a rotation was still waiting to activate: rotate once
    /// more when it does, instead of once per leaver.
    dirty: bool,
}

fn sorted(members: &Sessions) -> Vec<Member> {
    let mut v: Vec<Member> = members.keys().cloned().collect();
    v.sort();
    v
}

impl KeyRotator {
    pub fn new(members: Sessions, now: Instant) -> Self {
        let mgr = E2eeManager::new();
        let (active_index, active_b64) = (mgr.own_index(), mgr.own_key_b64());
        Self { mgr, active_index, active_b64, members, last_rotation: now, activate_at: None, dirty: false }
    }

    /// The key in use from the start: `(index, base64, raw)`.
    pub fn initial(&self) -> (u8, SecretString, SecretKey) {
        (self.mgr.own_index(), self.mgr.own_key_b64(), self.mgr.own_secret_key())
    }

    fn rotate(&mut self, now: Instant) -> RotationAction {
        let (index, key_b64) = self.mgr.rotate();
        self.last_rotation = now;
        self.activate_at = Some(now + ACTIVATION_DELAY);
        RotationAction::Send { index, key_b64, to: sorted(&self.members) }
    }

    /// A rotation has been made and not confirmed as in use yet.
    fn rotation_pending(&self) -> bool {
        self.mgr.own_index() != self.active_index
    }

    /// The indices of the key in use and of the newest one: all a member can
    /// still need from us.
    pub fn live_indices(&self) -> Vec<u8> {
        let mut v = vec![self.active_index];
        if self.rotation_pending() {
            v.push(self.mgr.own_index());
        }
        v
    }

    /// What a joiner needs: the key in use now, and the newer one if a rotation
    /// has not been put to use yet.
    fn keys_for_joiners(&self, joined: &[Member]) -> Vec<RotationAction> {
        let mut out = vec![RotationAction::Send {
            index: self.active_index,
            key_b64: self.active_b64.clone(),
            to: joined.to_vec(),
        }];
        if self.rotation_pending() {
            out.push(RotationAction::Send {
                index: self.mgr.own_index(),
                key_b64: self.mgr.own_key_b64(),
                to: joined.to_vec(),
            });
        }
        out
    }

    /// Feed the current live member set; returns what to do about the change.
    pub fn on_members_changed(&mut self, current: &Sessions, now: Instant) -> Vec<RotationAction> {
        // New to us, or back under another session: either needs the key in use.
        let joined: Vec<Member> = {
            let mut j: Vec<Member> = current
                .iter()
                .filter(|(m, tag)| self.members.get(*m) != Some(*tag))
                .map(|(m, _)| m.clone())
                .collect();
            j.sort();
            j
        };
        let left = self.members.keys().any(|m| !current.contains_key(m));
        self.members = current.clone();
        if !left && joined.is_empty() {
            return Vec::new();
        }
        if !left {
            return self.keys_for_joiners(&joined);
        }
        // Someone left: the remaining members get a new key.
        if self.rotation_pending() {
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

    /// Time-driven work: the activation request and periodic rotation. The
    /// activation is asked for once; the key only counts as in use after
    /// [`Self::activated`].
    pub fn poll(&mut self, now: Instant) -> Vec<RotationAction> {
        let mut out = Vec::new();
        if self.activate_at.is_some_and(|at| now >= at) {
            self.activate_at = None;
            out.push(RotationAction::Activate {
                index: self.mgr.own_index(),
                raw_key: self.mgr.own_secret_key(),
            });
        }
        if !self.rotation_pending() && now.saturating_duration_since(self.last_rotation) >= PERIODIC_ROTATION {
            out.push(self.rotate(now));
        }
        out
    }

    /// The caller put key `index` to use. From now on it is the key in use, and
    /// the rotation that leavers during the wait asked for is made.
    pub fn activated(&mut self, index: u8, now: Instant) -> Vec<RotationAction> {
        if !self.rotation_pending() || index != self.mgr.own_index() {
            return Vec::new();
        }
        self.active_index = index;
        self.active_b64 = self.mgr.own_key_b64();
        if self.dirty {
            self.dirty = false;
            return vec![self.rotate(now)];
        }
        Vec::new()
    }

    /// The next instant `poll` has work to do.
    pub fn next_deadline(&self) -> Option<Instant> {
        Some(self.activate_at.unwrap_or(self.last_rotation + PERIODIC_ROTATION))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    fn m(user: &str) -> (String, String) {
        (user.to_owned(), "DEV".to_owned())
    }

    fn set(users: &[&str]) -> Sessions {
        users.iter().map(|u| (m(u), "s1".to_owned())).collect()
    }

    /// `set` with `user` rejoined under another session.
    fn restarted(mut s: Sessions, user: &str) -> Sessions {
        s.insert(m(user), "s2".to_owned());
        s
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
        assert!(sends(&end).is_empty(), "the second rotation waits for the first to be in use");
        let more = r.activated(1, now + ACTIVATION_DELAY);
        assert_eq!(sends(&more), vec![(2, vec![m("@a:x")])]);
        assert_eq!(activations(&r.poll(now + ACTIVATION_DELAY * 2)), vec![2]);
        assert!(r.activated(2, now + ACTIVATION_DELAY * 2).is_empty());
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
        r.activated(1, now + ACTIVATION_DELAY);
        let acts = r.on_members_changed(&set(&["@a:x", "@c:x"]), now + ACTIVATION_DELAY + Duration::from_secs(1));
        assert_eq!(sends(&acts), vec![(1, vec![m("@c:x")])]);
    }

    #[test]
    fn a_joiner_while_the_activation_is_held_still_gets_both_keys() {
        // The activation was asked for but our encryptor is still on key 0 (the
        // gate holds it): a joiner given only key 1 could not read our media.
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        r.on_members_changed(&set(&["@a:x"]), now);
        assert_eq!(activations(&r.poll(now + ACTIVATION_DELAY)), vec![1]);
        let held = now + ACTIVATION_DELAY + Duration::from_secs(1);
        let acts = r.on_members_changed(&set(&["@a:x", "@c:x"]), held);
        assert_eq!(sends(&acts), vec![(0, vec![m("@c:x")]), (1, vec![m("@c:x")])]);
        assert_eq!(r.live_indices(), vec![0, 1]);
        // Once the caller has put key 1 to use, key 1 alone is enough.
        r.activated(1, held);
        let after = r.on_members_changed(&set(&["@a:x", "@c:x", "@d:x"]), held);
        assert_eq!(sends(&after), vec![(1, vec![m("@d:x")])]);
        assert_eq!(r.live_indices(), vec![1]);
    }

    #[test]
    fn a_leave_while_the_activation_is_held_rotates_once_it_is_in_use() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x", "@c:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        r.on_members_changed(&set(&["@a:x", "@b:x"]), now);
        r.poll(now + ACTIVATION_DELAY);
        let held = now + ACTIVATION_DELAY + Duration::from_secs(1);
        assert!(sends(&r.on_members_changed(&set(&["@a:x"]), held)).is_empty(), "no rotation while one is pending");
        assert!(r.poll(held + Duration::from_secs(1)).is_empty(), "the activation is asked for once");
        assert_eq!(sends(&r.activated(1, held + Duration::from_secs(2))), vec![(2, vec![m("@a:x")])]);
    }

    #[test]
    fn a_confirmation_for_another_key_changes_nothing() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        assert!(r.activated(0, t0).is_empty(), "nothing is pending");
        r.on_members_changed(&set(&["@a:x"]), t0);
        r.poll(t0 + ACTIVATION_DELAY);
        assert!(r.activated(7, t0 + ACTIVATION_DELAY).is_empty());
        assert_eq!(r.live_indices(), vec![0, 1], "key 1 is still not in use");
    }

    #[test]
    fn periodic_rotation_waits_for_a_pending_rotation_to_be_in_use() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(1);
        r.on_members_changed(&set(&["@a:x"]), now);
        r.poll(now + ACTIVATION_DELAY);
        // A full interval later the leave rotation is still not in use: no second key is made.
        assert!(sends(&r.poll(now + PERIODIC_ROTATION)).is_empty());
        r.activated(1, now + PERIODIC_ROTATION);
        assert_eq!(sends(&r.poll(now + PERIODIC_ROTATION)).len(), 1, "and then it is due");
    }

    #[test]
    fn a_leaver_can_read_our_media_for_a_bounded_time() {
        // Rotation at t, activation asked for at t + ACTIVATION_DELAY, put to use
        // when the gate lets go: at the latest MAX_ACTIVATION_WAIT after that.
        assert!(ACTIVATION_DELAY + MAX_ACTIVATION_WAIT <= Duration::from_secs(8));
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let mut g = ActivationGate::default();
        let acts = r.on_members_changed(&set(&["@a:x"]), t0);
        assert!(activations(&acts).is_empty());
        let asked = t0 + ACTIVATION_DELAY;
        for a in r.poll(asked) {
            if let RotationAction::Activate { index, raw_key } = a {
                g.hold(index, raw_key, asked);
            }
        }
        // Delivery never settles: the gate still lets go in time.
        assert!(g.release(|_| false, asked + MAX_ACTIVATION_WAIT - Duration::from_millis(1)).is_none());
        assert!(g.release(|_| false, asked + MAX_ACTIVATION_WAIT).is_some());
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
    fn a_same_device_rejoin_gets_the_key_in_use_without_a_rotation() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        let acts = r.on_members_changed(&restarted(set(&["@a:x", "@b:x"]), "@b:x"), now);
        assert_eq!(sends(&acts), vec![(0, vec![m("@b:x")])], "only the restarted device, the key in use");
        assert!(r.poll(now + ACTIVATION_DELAY).is_empty(), "nothing rotates");
        // The same session reported again is not a join.
        assert!(r.on_members_changed(&restarted(set(&["@a:x", "@b:x"]), "@b:x"), now).is_empty());
    }

    #[test]
    fn a_rejoin_during_a_pending_rotation_gets_both_keys() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x", "@c:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        r.on_members_changed(&set(&["@a:x", "@b:x"]), now); // c leaves: key 1 pending
        let acts = r.on_members_changed(&restarted(set(&["@a:x", "@b:x"]), "@b:x"), now + Duration::from_secs(1));
        assert_eq!(sends(&acts), vec![(0, vec![m("@b:x")]), (1, vec![m("@b:x")])]);
    }

    #[test]
    fn a_rejoin_together_with_a_leave_rotates_once_and_still_gets_the_old_key() {
        let t0 = Instant::now();
        let mut r = KeyRotator::new(set(&["@a:x", "@b:x", "@c:x"]), t0);
        let now = t0 + Duration::from_secs(60);
        let acts = r.on_members_changed(&restarted(set(&["@a:x", "@b:x"]), "@b:x"), now);
        assert_eq!(
            sends(&acts),
            vec![(1, vec![m("@a:x"), m("@b:x")]), (0, vec![m("@b:x")])]
        );
    }

    #[test]
    fn an_activation_waits_for_the_key_to_be_settled() {
        let t0 = Instant::now();
        let mut g = ActivationGate::default();
        assert!(g.release(|_| true, t0).is_none(), "nothing held");
        g.hold(3, Secret::new([7; 32]), t0);
        assert_eq!(g.deadline(), Some(t0 + MAX_ACTIVATION_WAIT));
        assert!(g.release(|_| false, t0 + Duration::from_secs(1)).is_none());
        assert_eq!(g.release(|i| i == 3, t0 + Duration::from_secs(2)), Some((3, Secret::new([7; 32]))));
        assert!(g.release(|_| true, t0 + Duration::from_secs(3)).is_none(), "released once");
        assert!(g.deadline().is_none());
    }

    #[test]
    fn a_stuck_delivery_cannot_hold_an_activation_forever() {
        let t0 = Instant::now();
        let mut g = ActivationGate::default();
        g.hold(1, Secret::new([1; 32]), t0);
        assert!(g.release(|_| false, t0 + MAX_ACTIVATION_WAIT - Duration::from_millis(1)).is_none());
        assert_eq!(g.release(|_| false, t0 + MAX_ACTIVATION_WAIT), Some((1, Secret::new([1; 32]))));
    }

    #[test]
    fn a_newer_activation_replaces_a_held_one() {
        let t0 = Instant::now();
        let mut g = ActivationGate::default();
        g.hold(1, Secret::new([1; 32]), t0);
        g.hold(2, Secret::new([2; 32]), t0 + Duration::from_secs(1));
        assert_eq!(g.release(|_| true, t0 + Duration::from_secs(2)), Some((2, Secret::new([2; 32]))));
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
            let at = t0 + PERIODIC_ROTATION * (i + 1);
            let acts = r.poll(at);
            if let Some((idx, _)) = sends(&acts).into_iter().next() {
                last = Some(idx);
                assert_eq!(activations(&r.poll(at + ACTIVATION_DELAY)), vec![idx]);
                r.activated(idx, at + ACTIVATION_DELAY);
            }
        }
        assert_eq!(last, Some(0));
    }
}
