//! Live call members and the LiveKit SFU each one publishes on.
//!
//! Element Call (`compatibility` mode) selects SFUs with `multi_sfu`: every
//! client publishes on its own homeserver's SFU and subscribes on the SFU of
//! every other member. Legacy clients use `oldest_membership`, where all
//! members share the oldest member's SFU. This mirrors matrix-js-sdk's
//! `CallMembership.getTransport()`.

use std::collections::{BTreeSet, HashSet};

use matrix_sdk::ruma::{
    events::{
        call::member::{ActiveFocus, Application, CallMemberEventContent, CallScope, Focus, MembershipData},
        room::member::MembershipState,
    },
    DeviceId, MilliSecondsSinceUnixEpoch, UInt, UserId,
};

pub const SELECTION_MULTI_SFU: &str = "multi_sfu";
pub const SELECTION_OLDEST: &str = "oldest_membership";

/// A LiveKit focus: the JWT service URL and the room alias.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LkFocus {
    pub service_url: String,
    pub alias: String,
    /// The SFU WebSocket URL, when the member's event names one (MSC4195).
    pub sfu_url: Option<String>,
    /// Set when the member publishes on another homeserver's SFU.
    pub server_name: Option<String>,
}

impl LkFocus {
    /// What identifies this SFU: the JWT service URL when known, else the SFU URL.
    pub fn key(&self) -> &str {
        if self.service_url.is_empty() {
            self.sfu_url.as_deref().unwrap_or("")
        } else {
            &self.service_url
        }
    }
}

/// Whether `url` can be the base of a JWT service we may call: an http(s) URL
/// with a host. An empty or malformed one (a member that published only an SFU
/// `url`, or garbage) is never an SFU to subscribe to.
pub fn usable_service_url(url: &str) -> bool {
    reqwest::Url::parse(url.trim())
        .map(|u| matches!(u.scheme(), "http" | "https") && u.host_str().is_some())
        .unwrap_or(false)
}

/// An SFU URL reduced to what names the server: whether it is TLS (`wss` and
/// `https` are the same endpoint), the lowercased host, the port (the scheme's
/// default when omitted) and the path without trailing slashes. Query and
/// fragment are dropped. `None` for anything that is not a ws/wss/http/https URL
/// with a host.
fn sfu_url_identity(url: &str) -> Option<(bool, String, u16, String)> {
    let u = reqwest::Url::parse(url.trim()).ok()?;
    let tls = match u.scheme() {
        "wss" | "https" => true,
        "ws" | "http" => false,
        _ => return None,
    };
    let host = u.host_str()?.trim_end_matches('.').to_ascii_lowercase();
    let port = u.port().unwrap_or(if tls { 443 } else { 80 });
    Some((tls, host, port, u.path().trim_end_matches('/').to_owned()))
}

/// Whether two SFU URLs name the same server, however each is spelled
/// (`wss://sfu.x.org`, `wss://SFU.x.org:443/`, `https://sfu.x.org` ...). A URL
/// that cannot be read matches nothing, not even itself.
pub fn same_sfu_url(a: &str, b: &str) -> bool {
    match (sfu_url_identity(a), sfu_url_identity(b)) {
        (Some(x), Some(y)) => x == y,
        _ => false,
    }
}

/// Which membership format a call member was read from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemberSource {
    Legacy,
    MatrixRtc,
}

/// One live (unexpired, joined) call member device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CallMember {
    pub user_id: String,
    pub device_id: String,
    pub state_key: String,
    pub joined_at: MilliSecondsSinceUnixEpoch,
    pub focus_selection: String,
    pub foci_preferred: Vec<LkFocus>,
    /// The MatrixRTC `member.id` (for legacy members, the state key).
    pub member_id: String,
    pub source: MemberSource,
    /// `m.audio` / `m.video` (or the reference implementation's spelling).
    pub intent: Option<String>,
    pub capabilities: Option<Vec<String>>,
    /// A legacy membership's `created_ts` as the sender wrote it, unclamped:
    /// only an identity for [`Self::session_tag`], never a time to order or
    /// expire by (that is `joined_at`).
    pub claimed_at_ms: Option<u64>,
}

impl CallMember {
    pub fn key(&self) -> (String, String) {
        (self.user_id.clone(), self.device_id.clone())
    }

    /// What tells one session of a device from the next, for a device that
    /// leaves and rejoins within its old membership's lifetime. A MatrixRTC
    /// member has a fresh `member.id` per session. A legacy one has only its
    /// `created_ts` (the state key stays the same per device): clients keep it
    /// across refreshes and take a new one when they join, so a changed value
    /// means a new session. The value is taken as written: the clamped
    /// `joined_at` of a sender whose clock runs ahead is the event time on one
    /// event and the claim on the next, which would look like a new session.
    /// Without a `created_ts` it is the event time, which then also changes on
    /// a refresh: that costs a resent key, nothing more.
    pub fn session_tag(&self) -> String {
        match self.source {
            MemberSource::MatrixRtc => self.member_id.clone(),
            MemberSource::Legacy => self.claimed_at_ms.unwrap_or(u64::from(self.joined_at.0)).to_string(),
        }
    }
}

/// How strictly the sender of a call member is checked against the room.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum JoinCheck {
    /// The sender must be a joined member. The room's member list is fetched
    /// first if it is not loaded yet; for who may receive a media key.
    Strict,
    /// Local store only, no request: a sender the store does not know is kept
    /// and one known not to be joined is dropped; cheap enough for the room
    /// list. For display only: never to decide who receives a media key.
    Cached,
    /// Local store only, no request, and a sender the store does not know (or
    /// cannot read) is dropped: only a sender known to be joined counts. For
    /// deciding who receives a media key when the member list could not be
    /// fetched in time.
    KnownJoined,
}

/// Whether a sender with this membership (`None`: not in the store) counts.
pub fn sender_counts(membership: Option<&MembershipState>, check: JoinCheck) -> bool {
    match (membership, check) {
        (Some(m), _) => *m == MembershipState::Join,
        (None, JoinCheck::Strict | JoinCheck::KnownJoined) => false,
        (None, JoinCheck::Cached) => true,
    }
}

/// The members whose sender is in `keep`: a kicked or banned user's call
/// membership stays valid on its own until it expires, but is no call member.
pub fn retain_senders(members: Vec<CallMember>, keep: &HashSet<String>) -> Vec<CallMember> {
    members.into_iter().filter(|m| keep.contains(&m.user_id)).collect()
}

/// What looking a sender up in the room gave.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SenderLookup {
    /// The room answered: the sender's membership, `None` if it has none.
    Known(Option<MembershipState>),
    /// The lookup failed (a members fetch that did not complete, a store error):
    /// that says nothing about whether the sender is joined.
    Unknown,
}

/// Whether the sender counts. An `Unknown` lookup repeats the verdict of the
/// last pass (`previous`), so a request that failed once does not turn a joined
/// member into a leaver; without one it is as unknown as `check` treats a
/// sender the room does not know.
pub fn judge_sender(lookup: &SenderLookup, previous: Option<bool>, check: JoinCheck) -> bool {
    match lookup {
        SenderLookup::Known(m) => sender_counts(m.as_ref(), check),
        SenderLookup::Unknown => previous.unwrap_or_else(|| sender_counts(None, check)),
    }
}

/// The verdict worth remembering for later passes: one the room gave, not one
/// that only reflects what the local store happens not to know.
pub fn lasting_verdict(lookup: &SenderLookup, check: JoinCheck) -> Option<bool> {
    match (lookup, check) {
        (SenderLookup::Known(Some(m)), _) => Some(*m == MembershipState::Join),
        (SenderLookup::Known(None), JoinCheck::Strict) => Some(false),
        _ => None,
    }
}

/// The last verdict per (room, sender), bounded.
#[derive(Default)]
pub struct VerdictCache {
    map: std::collections::HashMap<(String, String), bool>,
}

impl VerdictCache {
    const CAP: usize = 2048;

    pub fn get(&self, room: &str, sender: &str) -> Option<bool> {
        self.map.get(&(room.to_owned(), sender.to_owned())).copied()
    }

    pub fn set(&mut self, room: &str, sender: &str, verdict: bool) {
        if self.map.len() >= Self::CAP {
            self.map.clear();
        }
        self.map.insert((room.to_owned(), sender.to_owned()), verdict);
    }
}

fn verdicts() -> &'static std::sync::Mutex<VerdictCache> {
    static VERDICTS: std::sync::OnceLock<std::sync::Mutex<VerdictCache>> = std::sync::OnceLock::new();
    VERDICTS.get_or_init(Default::default)
}

/// Look `sender` up under `check`. A strict lookup that fails (the member
/// list could not be fetched) falls back on what the store has; `fetch_ok` goes
/// false at the first failed fetch, so the rest of a pass does not repeat it.
async fn lookup_sender(
    room: &matrix_sdk::Room,
    sender: &UserId,
    check: JoinCheck,
    fetch_ok: &mut bool,
) -> SenderLookup {
    let membership = |m: Option<matrix_sdk::room::RoomMember>| m.map(|m| m.membership().clone());
    match check {
        JoinCheck::Cached | JoinCheck::KnownJoined => match room.get_member_no_sync(sender).await {
            Ok(m) => SenderLookup::Known(membership(m)),
            Err(_) => SenderLookup::Unknown,
        },
        JoinCheck::Strict => {
            if *fetch_ok {
                match room.get_member(sender).await {
                    Ok(m) => return SenderLookup::Known(membership(m)),
                    Err(e) => {
                        *fetch_ok = false;
                        tracing::debug!("rtc: member list fetch failed ({e}); using the local store");
                    }
                }
            }
            match room.get_member_no_sync(sender).await {
                Ok(Some(m)) => SenderLookup::Known(membership(Some(m))),
                _ => SenderLookup::Unknown,
            }
        }
    }
}

/// Which of `senders` count as call members of `room` under `check`.
pub async fn senders_to_keep(
    room: &matrix_sdk::Room,
    senders: Vec<String>,
    check: JoinCheck,
) -> HashSet<String> {
    let mut keep: HashSet<String> = HashSet::new();
    let mut seen: HashSet<String> = HashSet::new();
    let room_id = room.room_id().to_string();
    let mut fetch_ok = true;
    for sender in senders {
        if !seen.insert(sender.clone()) {
            continue;
        }
        let Ok(user) = UserId::parse(&sender) else { continue };
        let lookup = lookup_sender(room, &user, check, &mut fetch_ok).await;
        let previous = verdicts().lock().unwrap_or_else(|p| p.into_inner()).get(&room_id, &sender);
        let counts = judge_sender(&lookup, previous, check);
        if let Some(verdict) = lasting_verdict(&lookup, check) {
            verdicts().lock().unwrap_or_else(|p| p.into_inner()).set(&room_id, &sender, verdict);
        }
        if counts {
            keep.insert(sender);
        } else {
            tracing::debug!("rtc: {sender} is not a call member of {room_id}: {lookup:?} (last pass: {previous:?})");
        }
    }
    keep
}

/// How long after its newest event a legacy membership may be live, however
/// long a lifetime it claims (Element Call uses four hours and refreshes well
/// before). Counted from the event, not from the join: a call that has run for
/// days keeps being refreshed, and each refresh is a new event.
pub const MAX_LEGACY_EXPIRES_MS: u64 = 24 * 3_600_000;
/// How much older than its own event a membership's `created_ts` is believed
/// to be: the same window. A member that joined longer ago than this is
/// ordered as if it joined then, so a made-up ancient `created_ts` cannot make
/// its sender the oldest member (the SFU choice follows the oldest).
pub const MAX_LEGACY_AGE_MS: u64 = MAX_LEGACY_EXPIRES_MS;
/// The lifetime of a flat membership that names none.
const DEFAULT_LEGACY_EXPIRES_MS: u64 = 4 * 3_600_000;

/// When a legacy membership joined, as MSC3401 defines it:
/// `MIN(content.created_ts, event.origin_server_ts)`. `created_ts` is the
/// sender's word, `origin_server_ts` the server's: a `created_ts` after the
/// event itself is impossible and is ignored, so it can neither push a
/// membership's expiry into the future nor make it look younger. One claiming
/// to be older is believed (that is how a refreshed membership keeps its place,
/// and the same value other clients order by), up to [`MAX_LEGACY_AGE_MS`]
/// before the event.
pub fn legacy_join_ms(created_ms: Option<u64>, origin_ms: u64) -> u64 {
    created_ms.map_or(origin_ms, |c| c.clamp(origin_ms.saturating_sub(MAX_LEGACY_AGE_MS), origin_ms))
}

/// When a legacy membership started, for working out when its claimed lifetime
/// ends: `MIN(created_ts, origin_server_ts)` with no age limit. The lifetime a
/// refresh claims counts from the real join, so the age limit of
/// [`legacy_join_ms`] (an ordering rule) must not move it.
pub fn legacy_started_ms(created_ms: Option<u64>, origin_ms: u64) -> u64 {
    created_ms.map_or(origin_ms, |c| c.min(origin_ms))
}

/// Whether a legacy membership is still live at `now_ms`: it started at
/// `join_ms` ([`legacy_started_ms`]) and claims the lifetime `expires_ms`, and it is no more than
/// [`MAX_LEGACY_EXPIRES_MS`] after its event (`origin_ms`). The cap counts from
/// the event: our own refresh sets `expires` to the call's age plus four hours,
/// so a long call's lifetime can exceed a day while its newest event is fresh.
pub fn legacy_is_live(join_ms: u64, expires_ms: u64, origin_ms: u64, now_ms: u64) -> bool {
    now_ms <= join_ms.saturating_add(expires_ms).min(origin_ms.saturating_add(MAX_LEGACY_EXPIRES_MS))
}

/// Whether a membership belongs to the room's default call: `scope` `m.room`
/// (or not given) and no `call_id`. Other calls of the room are other sessions
/// whose members are not in ours, and whose media keys are not ours to give.
pub fn is_room_call(scope: Option<&str>, call_id: Option<&str>) -> bool {
    scope.is_none_or(|s| s == "m.room") && call_id.is_none_or(str::is_empty)
}

fn typed_is_room_call(m: &MembershipData<'_>) -> bool {
    match m.application() {
        Application::Call(c) => c.scope == CallScope::Room && c.call_id.is_empty(),
        _ => false,
    }
}

fn typed_expires_ms(m: &MembershipData<'_>) -> u64 {
    let d = match m {
        MembershipData::Legacy(d) => d.expires,
        MembershipData::Session(d) => d.expires,
        _ => return 0,
    };
    u64::try_from(d.as_millis()).unwrap_or(u64::MAX)
}

/// Extract the live members from one `org.matrix.msc3401.call.member` event.
///
/// Skips expired and leave memberships, non-call applications and our own
/// user+device (a stale membership of a previous session).
#[cfg(test)]
pub fn members_from_call_member(
    content: &CallMemberEventContent,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    own_user: &UserId,
    own_device: &DeviceId,
) -> Vec<CallMember> {
    members_from_call_member_inner(
        content,
        state_key,
        sender,
        origin_server_ts,
        Some((own_user, own_device)),
        u64::from(MilliSecondsSinceUnixEpoch::now().0),
    )
}

fn members_from_call_member_inner(
    content: &CallMemberEventContent,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    exclude: Option<(&UserId, &DeviceId)>,
    now_ms: u64,
) -> Vec<CallMember> {
    content
        .memberships()
        .into_iter()
        .filter(typed_is_room_call)
        // Not ruma's own expiry check: it trusts a `created_ts` from the future
        // and any `expires` (see `legacy_started_ms`, `legacy_is_live`).
        .filter(|m| {
            let origin_ms = u64::from(origin_server_ts.0);
            let started = legacy_started_ms(m.created_ts().map(|t| u64::from(t.0)), origin_ms);
            legacy_is_live(started, typed_expires_ms(m), origin_ms, now_ms)
        })
        .filter(|m| !exclude.is_some_and(|(u, d)| sender == u && m.device_id() == d))
        .map(|m| {
            let focus_selection = match m.focus_active() {
                ActiveFocus::Livekit(a) => a.focus_selection.as_str().to_owned(),
                _ => String::new(),
            };
            let foci_preferred = m
                .foci_preferred()
                .iter()
                .filter_map(|f| match f {
                    Focus::Livekit(lk) if !lk.service_url.is_empty() => Some(LkFocus {
                        service_url: lk.service_url.clone(),
                        alias: lk.alias.clone(),
                        sfu_url: None,
                        server_name: None,
                    }),
                    _ => None,
                })
                .collect();
            CallMember {
                user_id: sender.to_string(),
                device_id: m.device_id().to_string(),
                state_key: state_key.to_owned(),
                joined_at: MilliSecondsSinceUnixEpoch(
                    UInt::new(legacy_join_ms(m.created_ts().map(|t| u64::from(t.0)), u64::from(origin_server_ts.0)))
                        .unwrap_or(UInt::MIN),
                ),
                focus_selection,
                foci_preferred,
                member_id: state_key.to_owned(),
                source: MemberSource::Legacy,
                intent: match m.call_intent() {
                    Some(matrix_sdk::ruma::events::rtc::notification::CallIntent::Video) => Some("video".to_owned()),
                    Some(matrix_sdk::ruma::events::rtc::notification::CallIntent::Audio) => Some("audio".to_owned()),
                    _ => None,
                },
                capabilities: None,
                claimed_at_ms: m.created_ts().map(|t| u64::from(t.0)),
            }
        })
        .collect()
}

/// The transport of the oldest membership, counting our own (`own`) too.
/// Ties are broken by state key so every client agrees.
fn oldest_transport(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> Option<LkFocus> {
    let mut best: Option<(MilliSecondsSinceUnixEpoch, &str, &LkFocus)> =
        own.map(|(ts, f)| (ts, "", f));
    for m in members {
        let Some(first) = m.foci_preferred.iter().find(|f| usable_service_url(&f.service_url)) else {
            continue;
        };
        let cand = (m.joined_at, m.state_key.as_str(), first);
        if best.map_or(true, |b| (cand.0, cand.1) < (b.0, b.1)) {
            best = Some(cand);
        }
    }
    best.map(|(_, _, f)| f.clone())
}

/// The SFU a member publishes its media on, or `None` if its selection method
/// is unknown or it advertises no LiveKit focus we can use: one with a JWT
/// service URL, or one naming the SFU WebSocket we are connected to ourselves.
pub fn transport_of(
    member: &CallMember,
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> Option<LkFocus> {
    match member.focus_selection.as_str() {
        SELECTION_MULTI_SFU => member
            .foci_preferred
            .iter()
            .find(|f| usable_service_url(&f.service_url))
            .cloned()
            .or_else(|| {
                // A transport with only an SFU URL is on our SFU if that is ours.
                let (_, local) = own?;
                let ours = local.sfu_url.as_deref()?;
                member
                    .foci_preferred
                    .iter()
                    .any(|f| f.sfu_url.as_deref().is_some_and(|u| same_sfu_url(u, ours)))
                    .then(|| local.clone())
            }),
        SELECTION_OLDEST => oldest_transport(members, own),
        _ => None,
    }
}

/// The remote SFUs we must be subscribed to: every member's transport except
/// our own local SFU (which the primary connection already covers).
pub fn desired_sfus(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
    local_service_url: &str,
) -> BTreeSet<String> {
    members
        .iter()
        .filter_map(|m| transport_of(m, members, own))
        .map(|f| f.service_url)
        .filter(|u| u != local_service_url && usable_service_url(u))
        .collect()
}

/// Which SFU each live member publishes on, keyed by `(user_id, device_id)`.
/// Members whose transport can't be determined are left out and so are not
/// held to any SFU, except a MatrixRTC member that did publish transports none
/// of which we can use: it maps to the empty string, which matches no SFU, so
/// no audio or video is attributed to it anywhere.
pub fn transport_map(
    members: &[CallMember],
    own: Option<(MilliSecondsSinceUnixEpoch, &LkFocus)>,
) -> std::collections::HashMap<(String, String), String> {
    members
        .iter()
        .filter_map(|m| match transport_of(m, members, own) {
            Some(f) => Some((m.key(), f.service_url)),
            None if m.source == MemberSource::MatrixRtc
                && m.focus_selection == SELECTION_MULTI_SFU
                && !m.foci_preferred.is_empty() =>
            {
                Some((m.key(), String::new()))
            }
            None => None,
        })
        .collect()
}

/// Members whose user is not in `ignored`: an ignored user's call does not
/// show up in the "call in progress" banner.
pub fn without_ignored(members: Vec<CallMember>, ignored: &HashSet<String>) -> Vec<CallMember> {
    if ignored.is_empty() {
        return members;
    }
    members.into_iter().filter(|m| !ignored.contains(&m.user_id)).collect()
}

/// One user with a live call membership, for the "call in progress" banner.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Participant {
    pub user_id: String,
    pub joined_at: MilliSecondsSinceUnixEpoch,
    /// `"audio"` / `"video"` from the membership's `m.call.intent`.
    pub intent: Option<&'static str>,
}

/// Distinct users, oldest join first, and the call's overall intent: `"video"`
/// if any member is in it with video, else `"audio"` if any declared audio,
/// else empty.
pub fn summarize_participants(mut participants: Vec<Participant>) -> (Vec<String>, String) {
    participants.sort_by(|a, b| (a.joined_at, &a.user_id).cmp(&(b.joined_at, &b.user_id)));
    let intent = if participants.iter().any(|p| p.intent == Some("video")) {
        "video"
    } else if participants.iter().any(|p| p.intent == Some("audio")) {
        "audio"
    } else {
        ""
    };
    let mut users: Vec<String> = Vec::new();
    for p in &participants {
        if !users.contains(&p.user_id) {
            users.push(p.user_id.clone());
        }
    }
    (users, intent.to_owned())
}

/// The live members of one `org.matrix.msc3401.call.member` event, from its
/// raw content. ruma's typed model is tried first (it also covers the old
/// `memberships` array); when it finds no membership, the flat session form is
/// read leniently: newer clients leave out fields ruma insists on (such as
/// `livekit_alias`), which makes ruma silently see an empty membership.
pub fn members_from_event_json(
    content: &serde_json::Value,
    state_key: &str,
    sender: &UserId,
    origin_server_ts: MilliSecondsSinceUnixEpoch,
    exclude: Option<(&UserId, &DeviceId)>,
    now_ms: u64,
) -> Vec<CallMember> {
    if let Ok(typed) = serde_json::from_value::<CallMemberEventContent>(content.clone()) {
        if !typed.memberships().is_empty() {
            return members_from_call_member_inner(&typed, state_key, sender, origin_server_ts, exclude, now_ms);
        }
    }
    let Some(obj) = content.as_object() else { return Vec::new() };
    if obj.get("application").and_then(serde_json::Value::as_str) != Some("m.call") {
        return Vec::new();
    }
    if !is_room_call(
        obj.get("scope").and_then(serde_json::Value::as_str),
        obj.get("call_id").and_then(serde_json::Value::as_str),
    ) {
        return Vec::new();
    }
    let Some(device_id) = obj.get("device_id").and_then(serde_json::Value::as_str) else {
        return Vec::new();
    };
    if exclude.is_some_and(|(u, d)| sender == u && d.as_str() == device_id) {
        return Vec::new();
    }
    let origin_ms = u64::from(origin_server_ts.0);
    let claimed_at_ms = obj.get("created_ts").and_then(serde_json::Value::as_u64);
    let base_ms = legacy_join_ms(claimed_at_ms, origin_ms);
    let expires_ms = obj.get("expires").and_then(serde_json::Value::as_u64).unwrap_or(DEFAULT_LEGACY_EXPIRES_MS);
    if !legacy_is_live(legacy_started_ms(claimed_at_ms, origin_ms), expires_ms, origin_ms, now_ms) {
        return Vec::new();
    }
    let foci_preferred = obj
        .get("foci_preferred")
        .and_then(serde_json::Value::as_array)
        .into_iter()
        .flatten()
        .filter(|f| f.get("type").and_then(serde_json::Value::as_str) == Some("livekit"))
        .filter_map(|f| {
            let service_url = f.get("livekit_service_url")?.as_str()?.to_owned();
            (!service_url.is_empty()).then(|| LkFocus {
                service_url,
                alias: f.get("livekit_alias").and_then(serde_json::Value::as_str).unwrap_or_default().to_owned(),
                sfu_url: None,
                server_name: None,
            })
        })
        .collect();
    let focus_selection = content
        .pointer("/focus_active/focus_selection")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let intent = match obj.get("m.call.intent").and_then(serde_json::Value::as_str) {
        Some(i @ ("audio" | "video")) => Some(i.to_owned()),
        _ => None,
    };
    vec![CallMember {
        user_id: sender.to_string(),
        device_id: device_id.to_owned(),
        state_key: state_key.to_owned(),
        joined_at: MilliSecondsSinceUnixEpoch(UInt::new(base_ms).unwrap_or(UInt::MIN)),
        focus_selection,
        foci_preferred,
        member_id: state_key.to_owned(),
        source: MemberSource::Legacy,
        intent,
        capabilities: None,
        claimed_at_ms,
    }]
}

/// The `call_id` of a call member event, from the flat session form or the
/// first membership of the old `memberships` array; empty when there is none.
pub fn call_id_of(content: &serde_json::Value) -> String {
    content
        .get("call_id")
        .or_else(|| content.pointer("/memberships/0/call_id"))
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned()
}

/// Every live call member of `room` (legacy format) whose sender passes
/// `check`, optionally leaving out one user+device, read from the raw state
/// events. The one place every legacy read goes through.
pub async fn read_call_members(
    room: &matrix_sdk::Room,
    exclude: Option<(&UserId, &DeviceId)>,
    check: JoinCheck,
) -> Vec<CallMember> {
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    use matrix_sdk::ruma::events::StateEventType;

    let now_ms = u64::from(MilliSecondsSinceUnixEpoch::now().0);
    let mut members = Vec::new();
    for raw in room
        .get_state_events(StateEventType::from("org.matrix.msc3401.call.member"))
        .await
        .unwrap_or_default()
    {
        let json = match &raw {
            RawAnySyncOrStrippedState::Sync(r) => r.json().get(),
            RawAnySyncOrStrippedState::Stripped(_) => continue,
        };
        let Ok(v) = serde_json::from_str::<serde_json::Value>(json) else { continue };
        let (Some(sender), Some(state_key), Some(origin)) = (
            v.get("sender").and_then(serde_json::Value::as_str).and_then(|s| UserId::parse(s).ok()),
            v.get("state_key").and_then(serde_json::Value::as_str),
            v.get("origin_server_ts").and_then(serde_json::Value::as_u64),
        ) else {
            continue;
        };
        let origin = MilliSecondsSinceUnixEpoch(UInt::new(origin).unwrap_or(UInt::MIN));
        members.extend(members_from_event_json(
            v.get("content").unwrap_or(&serde_json::Value::Null),
            state_key,
            &sender,
            origin,
            exclude,
            now_ms,
        ));
    }
    if members.is_empty() {
        return members;
    }
    let keep = senders_to_keep(room, members.iter().map(|m| m.user_id.clone()).collect(), check).await;
    retain_senders(members, &keep)
}

/// Read every live remote call member of `room` from the local state store.
pub async fn read_live_members(
    room: &matrix_sdk::Room,
    own_user: &UserId,
    own_device: &DeviceId,
    check: JoinCheck,
) -> Vec<CallMember> {
    read_call_members(room, Some((own_user, own_device)), check).await
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use matrix_sdk::ruma::{
        events::call::member::{
            ActiveFocus, ActiveLivekitFocus, Application, CallApplicationContent,
            CallMemberEventContent, CallScope, Focus, LivekitFocus,
        },
        owned_device_id, user_id, UInt,
    };

    use super::*;

    /// A minute ago plus `ms`. The base is read once, so equal arguments give
    /// equal timestamps even when the clock ticks between calls (ties must
    /// stay ties for the state-key tie-break tests).
    fn ts(ms: u32) -> MilliSecondsSinceUnixEpoch {
        static BASE: std::sync::OnceLock<UInt> = std::sync::OnceLock::new();
        let base =
            *BASE.get_or_init(|| MilliSecondsSinceUnixEpoch::now().0 - UInt::from(60_000u32));
        MilliSecondsSinceUnixEpoch(base + UInt::from(ms))
    }

    /// Build a member event as the wire would carry it; `selection` is the
    /// `focus_active.focus_selection` string.
    fn content(
        device: &str,
        selection: &str,
        url: &str,
        created: Option<MilliSecondsSinceUnixEpoch>,
    ) -> CallMemberEventContent {
        let mut c = CallMemberEventContent::new(
            Application::Call(CallApplicationContent::new(String::new(), CallScope::Room)),
            device.into(),
            ActiveFocus::Livekit(ActiveLivekitFocus::new()),
            vec![Focus::Livekit(LivekitFocus::new(
                "!room:a.org".to_owned(),
                url.to_owned(),
            ))],
            created,
            Some(Duration::from_secs(3600)),
        );
        let mut json = serde_json::to_value(&c).unwrap();
        json["focus_active"]["focus_selection"] = selection.into();
        c = serde_json::from_value(json).unwrap();
        c
    }

    fn members(
        c: &CallMemberEventContent,
        key: &str,
        sender: &str,
        origin: MilliSecondsSinceUnixEpoch,
    ) -> Vec<CallMember> {
        members_from_call_member(
            c,
            key,
            &UserId::parse(sender).unwrap(),
            origin,
            user_id!("@me:a.org"),
            &owned_device_id!("MYDEV"),
        )
    }

    fn one(c: CallMemberEventContent, key: &str, sender: &str, t: u32) -> Vec<CallMember> {
        members(&c, key, sender, ts(t))
    }

    #[test]
    fn multi_sfu_uses_own_first_focus() {
        let m = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k",
            "@b:b.org",
            1,
        );
        assert_eq!(m[0].focus_selection, "multi_sfu");
        assert_eq!(
            transport_of(&m[0], &m, None).unwrap().service_url,
            "https://sfu.b.org"
        );
    }

    #[test]
    fn oldest_membership_follows_oldest_member() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", None),
            "kb",
            "@b:b.org",
            100,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "kc",
            "@c:c.org",
            50,
        ));
        assert_eq!(
            transport_of(&all[0], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
        assert_eq!(
            transport_of(&all[1], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
    }

    #[test]
    fn oldest_membership_counts_our_own_membership() {
        let all = one(content("D1", "oldest_membership", "https://sfu.b.org", None), "kb", "@b:b.org", 100);
        let own = LkFocus { service_url: "https://sfu.a.org".into(), alias: "!r".into(), sfu_url: None, server_name: None };
        // we joined before b, so b (legacy) publishes on our SFU
        assert_eq!(
            transport_of(&all[0], &all, Some((ts(10), &own)))
                .unwrap()
                .service_url,
            "https://sfu.a.org"
        );
    }

    #[test]
    fn created_ts_beats_origin_server_ts() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", Some(ts(10))),
            "kb",
            "@b:b.org",
            900,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "kc",
            "@c:c.org",
            50,
        ));
        assert_eq!(
            transport_of(&all[1], &all, None).unwrap().service_url,
            "https://sfu.b.org"
        );
    }

    #[test]
    fn oldest_tie_broken_by_state_key() {
        let mut all = one(
            content("D1", "oldest_membership", "https://sfu.b.org", None),
            "kb",
            "@b:b.org",
            5,
        );
        all.extend(one(
            content("D2", "oldest_membership", "https://sfu.c.org", None),
            "ka",
            "@c:c.org",
            5,
        ));
        assert_eq!(
            transport_of(&all[0], &all, None).unwrap().service_url,
            "https://sfu.c.org"
        );
    }

    #[test]
    fn unknown_selection_has_no_transport() {
        let m = one(
            content("D1", "something_new", "https://sfu.b.org", None),
            "k",
            "@b:b.org",
            1,
        );
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn own_device_excluded_other_device_not() {
        assert!(one(
            content("MYDEV", "multi_sfu", "https://sfu.a.org", None),
            "k",
            "@me:a.org",
            1
        )
        .is_empty());
        assert_eq!(
            one(
                content("OTHER", "multi_sfu", "https://sfu.a.org", None),
                "k",
                "@me:a.org",
                1
            )
            .len(),
            1
        );
    }

    #[test]
    fn leave_and_expired_are_ignored() {
        assert!(one(CallMemberEventContent::new_empty(None), "k", "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(one(
            content("D1", "multi_sfu", "https://sfu.b.org", Some(long_ago)),
            "k",
            "@b:b.org",
            1
        )
        .is_empty());
    }

    #[test]
    fn member_without_service_url_has_no_transport() {
        let m = one(content("D1", "multi_sfu", "", None), "k", "@b:b.org", 1);
        assert!(m[0].foci_preferred.is_empty());
        assert!(transport_of(&m[0], &m, None).is_none());
    }

    #[test]
    fn desired_sfus_excludes_local_and_dedups() {
        let mut all = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k1",
            "@b:b.org",
            1,
        );
        all.extend(one(
            content("D2", "multi_sfu", "https://sfu.b.org", None),
            "k2",
            "@c:b.org",
            2,
        ));
        all.extend(one(
            content("D3", "multi_sfu", "https://sfu.a.org", None),
            "k3",
            "@d:a.org",
            3,
        ));
        let want = desired_sfus(&all, None, "https://sfu.a.org");
        assert_eq!(
            want.into_iter().collect::<Vec<_>>(),
            vec!["https://sfu.b.org".to_owned()]
        );
    }

    fn rtc_member(user: &str, device: &str, foci: Vec<LkFocus>) -> CallMember {
        CallMember {
            user_id: user.into(), device_id: device.into(), state_key: String::new(), joined_at: ts(1),
            focus_selection: SELECTION_MULTI_SFU.into(), foci_preferred: foci, member_id: format!("m-{device}"),
            source: MemberSource::MatrixRtc, intent: None, capabilities: None, claimed_at_ms: None,
        }
    }

    fn focus(service: &str, sfu: Option<&str>) -> LkFocus {
        LkFocus { service_url: service.into(), alias: String::new(), sfu_url: sfu.map(str::to_owned), server_name: None }
    }

    #[test]
    fn service_urls_must_be_usable_http_urls() {
        assert!(usable_service_url("https://jwt.a.org"));
        assert!(usable_service_url("http://localhost:8080/sfu"));
        for bad in ["", "   ", "jwt.a.org", "wss://sfu.a.org", "https://", "not a url"] {
            assert!(!usable_service_url(bad), "{bad:?}");
        }
    }

    #[test]
    fn a_url_only_transport_is_never_an_sfu_to_subscribe_to() {
        let m = rtc_member("@b:b.org", "D", vec![focus("", Some("wss://sfu.b.org"))]);
        assert!(transport_of(&m, &[m.clone()], None).is_none());
        assert!(desired_sfus(&[m.clone()], None, "https://sfu.a.org").is_empty(), "no empty entry");
        // A usable second transport is picked instead of the useless first.
        let both = rtc_member("@b:b.org", "D", vec![focus("", Some("wss://x")), focus("https://sfu.b.org", None)]);
        assert_eq!(desired_sfus(&[both], None, "https://sfu.a.org").len(), 1);
        // Garbage in the service URL is skipped as well.
        let junk = rtc_member("@b:b.org", "D", vec![focus("not a url", None)]);
        assert!(desired_sfus(&[junk], None, "https://sfu.a.org").is_empty());
    }

    #[test]
    fn a_url_only_transport_naming_our_own_sfu_is_on_our_sfu() {
        let own = focus("https://sfu.a.org", Some("wss://sfu.a.org"));
        let mine = rtc_member("@b:b.org", "D", vec![focus("", Some("wss://sfu.a.org"))]);
        let elsewhere = rtc_member("@c:b.org", "E", vec![focus("", Some("wss://sfu.c.org"))]);
        let all = vec![mine.clone(), elsewhere.clone()];
        assert_eq!(transport_of(&mine, &all, Some((ts(1), &own))).unwrap().service_url, "https://sfu.a.org");
        assert!(transport_of(&elsewhere, &all, Some((ts(1), &own))).is_none());
        assert!(desired_sfus(&all, Some((ts(1), &own)), "https://sfu.a.org").is_empty());
    }

    #[test]
    fn equivalent_spellings_of_an_sfu_url_match() {
        let base = "wss://sfu.a.org";
        for same in [
            "wss://sfu.a.org",
            "wss://sfu.a.org/",
            "WSS://SFU.A.ORG",
            "wss://sfu.a.org:443",
            "wss://sfu.a.org:443/",
            "https://sfu.a.org",
            "https://sfu.a.org:443/",
            " wss://sfu.a.org ",
            "wss://sfu.a.org/?token=x",
        ] {
            assert!(same_sfu_url(base, same), "{same:?}");
            assert!(same_sfu_url(same, base), "{same:?}");
        }
        assert!(same_sfu_url("ws://sfu.a.org", "http://sfu.a.org:80/"));
        assert!(same_sfu_url("wss://sfu.a.org/livekit/sfu", "https://sfu.a.org/livekit/sfu/"));
    }

    #[test]
    fn a_different_server_never_matches() {
        let base = "wss://sfu.a.org";
        for other in [
            "wss://sfu.b.org",
            "wss://sfu.a.org.evil.org",
            "wss://sfu.a.org:7881",
            "ws://sfu.a.org",
            "http://sfu.a.org",
            "wss://sfu.a.org/livekit",
            "",
            "not a url",
            "ftp://sfu.a.org",
        ] {
            assert!(!same_sfu_url(base, other), "{other:?}");
        }
        assert!(!same_sfu_url("", ""), "an unreadable URL matches nothing");
    }

    #[test]
    fn a_url_only_transport_matches_our_sfu_in_any_spelling() {
        let own = focus("https://jwt.a.org", Some("wss://sfu.a.org"));
        for spelled in ["wss://sfu.a.org/", "wss://SFU.a.org:443", "https://sfu.a.org"] {
            let m = rtc_member("@b:b.org", "D", vec![focus("", Some(spelled))]);
            assert_eq!(
                transport_of(&m, &[m.clone()], Some((ts(1), &own))).map(|f| f.service_url),
                Some("https://jwt.a.org".to_owned()),
                "{spelled:?}"
            );
        }
        let other = rtc_member("@c:b.org", "E", vec![focus("", Some("wss://sfu.c.org"))]);
        assert!(transport_of(&other, &[other.clone()], Some((ts(1), &own))).is_none());
    }

    #[test]
    fn a_member_we_cannot_place_is_attributed_to_no_sfu() {
        let own = focus("https://sfu.a.org", Some("wss://sfu.a.org"));
        let useless = rtc_member("@c:b.org", "E", vec![focus("", Some("wss://sfu.c.org"))]);
        let none_published = rtc_member("@d:b.org", "F", Vec::new());
        let ok = rtc_member("@b:b.org", "D", vec![focus("https://sfu.b.org", None)]);
        let map = transport_map(&[useless.clone(), none_published.clone(), ok.clone()], Some((ts(1), &own)));
        assert_eq!(map[&useless.key()], "", "matches no SFU, so nothing is attributed");
        assert_eq!(map[&ok.key()], "https://sfu.b.org");
        assert!(!map.contains_key(&none_published.key()), "no transports at all: left out as before");
    }

    #[test]
    fn ignored_users_are_dropped_from_member_lists() {
        let a = rtc_member("@a:x", "D", Vec::new());
        let b = rtc_member("@b:x", "D", Vec::new());
        let ignored: HashSet<String> = ["@a:x".to_owned()].into();
        let left = without_ignored(vec![a.clone(), b.clone()], &ignored);
        assert_eq!(left.iter().map(|m| m.user_id.as_str()).collect::<Vec<_>>(), vec!["@b:x"]);
        assert_eq!(without_ignored(vec![a, b], &HashSet::new()).len(), 2);
    }

    #[test]
    fn transport_map_lists_each_members_sfu() {
        let mut all = one(
            content("D1", "multi_sfu", "https://sfu.b.org", None),
            "k1",
            "@b:b.org",
            1,
        );
        all.extend(one(
            content("D2", "something_new", "https://sfu.c.org", None),
            "k2",
            "@c:c.org",
            2,
        ));
        let map = transport_map(&all, None);
        assert_eq!(
            map.get(&("@b:b.org".to_owned(), "D1".to_owned())).unwrap(),
            "https://sfu.b.org"
        );
        assert!(!map.contains_key(&("@c:c.org".to_owned(), "D2".to_owned())));
    }

    /// The "call in progress" participants of one event, through the same
    /// raw-JSON path production uses.
    fn participants(c: &CallMemberEventContent, sender: &str, t: u32) -> Vec<Participant> {
        let members = members_from_event_json(
            &serde_json::to_value(c).unwrap(),
            "_k_m.call",
            &UserId::parse(sender).unwrap(),
            ts(t),
            Some((user_id!("@me:a.org"), &owned_device_id!("MYDEV"))),
            u64::from(MilliSecondsSinceUnixEpoch::now().0),
        );
        crate::client::rtc::slots::participants_from_members(&members)
    }

    #[test]
    fn participants_exclude_this_device_leave_and_expired() {
        assert!(participants(
            &content("MYDEV", "multi_sfu", "https://s", None),
            "@me:a.org",
            1
        )
        .is_empty());
        assert_eq!(
            participants(
                &content("OTHER", "multi_sfu", "https://s", None),
                "@me:a.org",
                1
            )
            .len(),
            1
        );
        assert!(participants(&CallMemberEventContent::new_empty(None), "@b:b.org", 1).is_empty());
        let long_ago = MilliSecondsSinceUnixEpoch(UInt::from(1_000u32));
        assert!(participants(
            &content("D", "multi_sfu", "https://s", Some(long_ago)),
            "@b:b.org",
            1
        )
        .is_empty());
    }

    #[test]
    fn summary_orders_by_join_time_and_dedups_users() {
        let mut all = participants(
            &content("D1", "multi_sfu", "https://s", None),
            "@late:b.org",
            50,
        );
        all.extend(participants(
            &content("D2", "multi_sfu", "https://s", None),
            "@early:b.org",
            10,
        ));
        all.extend(participants(
            &content("D3", "multi_sfu", "https://s", None),
            "@late:b.org",
            60,
        ));
        let (users, intent) = summarize_participants(all);
        assert_eq!(
            users,
            vec!["@early:b.org".to_owned(), "@late:b.org".to_owned()]
        );
        assert_eq!(intent, "");
    }

    #[test]
    fn summary_intent_prefers_video() {
        let p = |user: &str, intent| Participant {
            user_id: user.to_owned(),
            joined_at: ts(1),
            intent,
        };
        assert_eq!(
            summarize_participants(vec![p("@a:x", Some("audio"))]).1,
            "audio"
        );
        assert_eq!(
            summarize_participants(vec![p("@a:x", Some("audio")), p("@b:x", Some("video"))]).1,
            "video"
        );
        assert_eq!(summarize_participants(vec![p("@a:x", None)]).1, "");
    }

    #[test]
    fn created_ts_survives_serialization() {
        let created = ts(7);
        let json = serde_json::to_value(content(
            "D1",
            "multi_sfu",
            "https://sfu.b.org",
            Some(created),
        ))
        .unwrap();
        assert_eq!(json["created_ts"], serde_json::to_value(created).unwrap());
        assert_eq!(json["focus_active"]["focus_selection"], "multi_sfu");
    }

    // ── members_from_event_json: events ruma's typed model cannot read ─────────

    /// ruma checks expiry against the real clock, so the tests do too.
    fn now_ms() -> u64 {
        u64::from(MilliSecondsSinceUnixEpoch::now().0)
    }

    fn at_ms(ms: u64) -> MilliSecondsSinceUnixEpoch {
        MilliSecondsSinceUnixEpoch(UInt::new(ms).unwrap())
    }

    /// The member event Element's phone app wrote: no `livekit_alias`.
    fn phone_content() -> serde_json::Value {
        serde_json::json!({
            "application": "m.call", "call_id": "",
            "device_id": "PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw",
            "expires": 14_400_000,
            "foci_preferred": [{"livekit_service_url": "https://jwt.example.org/livekit/jwt", "type": "livekit"}],
            "focus_active": {"focus_selection": "multi_sfu", "type": "livekit"},
            "m.call.intent": "video",
            "membershipID": "@surak:example.org:PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw",
            "scope": "m.room"
        })
    }

    fn json_members(content: serde_json::Value, origin: u64, exclude: Option<(&UserId, &DeviceId)>) -> Vec<CallMember> {
        members_from_event_json(
            &content,
            "_@surak:example.org_PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw_m.call",
            user_id!("@surak:example.org"),
            at_ms(origin),
            exclude,
            now_ms(),
        )
    }

    #[test]
    fn a_member_event_without_livekit_alias_still_counts() {
        let got = json_members(phone_content(), now_ms() - 60_000, None);
        assert_eq!(got.len(), 1);
        let m = &got[0];
        assert_eq!(m.user_id, "@surak:example.org");
        assert_eq!(m.device_id, "PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw");
        assert_eq!(m.focus_selection, "multi_sfu");
        assert_eq!(m.foci_preferred[0].service_url, "https://jwt.example.org/livekit/jwt");
        assert_eq!(m.intent.as_deref(), Some("video"));
        assert_eq!(m.source, MemberSource::Legacy);
    }

    #[test]
    fn an_event_with_a_livekit_alias_still_counts_once() {
        let mut c = phone_content();
        c["foci_preferred"][0]["livekit_alias"] = serde_json::json!("!r:example.org");
        assert_eq!(json_members(c, now_ms() - 60_000, None).len(), 1);
    }

    #[test]
    fn an_expired_membership_is_ignored() {
        // Written five hours ago with a four hour lifetime.
        assert!(json_members(phone_content(), now_ms() - 5 * 3_600_000, None).is_empty());
        // A `created_ts` later than the event itself is impossible and ignored.
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(now_ms() - 3_600_000);
        assert!(json_members(c, now_ms() - 5 * 3_600_000, None).is_empty());
        // An earlier one is how a refreshed membership keeps its join time.
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(now_ms() - 3 * 3_600_000);
        let got = json_members(c, now_ms() - 60_000, None);
        assert_eq!(u64::from(got[0].joined_at.0), now_ms() - 3 * 3_600_000);
    }

    #[test]
    fn a_made_up_created_ts_cannot_extend_or_rejuvenate_a_membership() {
        // From the future: the join time is the event's.
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(now_ms() + 10 * 3_600_000);
        let origin = now_ms() - 60_000;
        let got = json_members(c, origin, None);
        assert_eq!(u64::from(got[0].joined_at.0), origin);
        // From long ago: past its lifetime, so not live, however old it claims to be.
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(1_000u64);
        assert!(json_members(c, now_ms() - 60_000, None).is_empty());
    }

    #[test]
    fn a_claimed_lifetime_is_capped_at_a_day_after_the_event() {
        let mut c = phone_content();
        c["expires"] = serde_json::json!(u64::MAX / 2);
        // Written 25 hours ago: dead, whatever it claims.
        assert!(json_members(c.clone(), now_ms() - 25 * 3_600_000, None).is_empty());
        assert_eq!(json_members(c, now_ms() - 23 * 3_600_000, None).len(), 1);
        assert!(legacy_is_live(0, u64::MAX, 100, 100 + MAX_LEGACY_EXPIRES_MS));
        assert!(!legacy_is_live(0, u64::MAX, 100, 100 + MAX_LEGACY_EXPIRES_MS + 1));
    }

    #[test]
    fn a_call_older_than_a_day_stays_live_while_its_member_keeps_refreshing() {
        // Joined 30 h ago. Our refresh a minute ago claims `expires` = age + 4 h.
        let created = now_ms() - 30 * 3_600_000;
        let origin = now_ms() - 60_000;
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(created);
        c["expires"] = serde_json::json!(origin - created + 4 * 3_600_000);
        let got = json_members(c.clone(), origin, None);
        assert_eq!(got.len(), 1, "refreshing, so still in the call");
        // Ordered as at most a day old, whatever its age.
        assert_eq!(u64::from(got[0].joined_at.0), origin - MAX_LEGACY_AGE_MS);
        // The same member that stopped refreshing 5 h ago has run out its lifetime
        // (created + 25 h + 4 h = 1 h ago); the age limit on ordering does not extend it.
        let origin = now_ms() - 5 * 3_600_000;
        c["expires"] = serde_json::json!(origin - created + 4 * 3_600_000);
        assert!(json_members(c, origin, None).is_empty());
    }

    #[test]
    fn a_made_up_ancient_created_ts_does_not_make_its_sender_the_oldest() {
        let origin = now_ms() - 60_000;
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(1_000u64);
        c["expires"] = serde_json::json!(u64::MAX / 2);
        let got = json_members(c, origin, None);
        assert_eq!(got.len(), 1, "live for a day after its event");
        assert_eq!(u64::from(got[0].joined_at.0), origin - MAX_LEGACY_AGE_MS);
        assert_eq!(legacy_join_ms(Some(1_000), origin), origin - MAX_LEGACY_AGE_MS);
        assert_eq!(legacy_join_ms(Some(origin - 5), origin), origin - 5);
        assert_eq!(legacy_join_ms(Some(origin + 5), origin), origin);
    }

    #[test]
    fn a_senders_clock_running_ahead_does_not_change_the_session_tag() {
        // First event: `created_ts` is ahead of the server's time for the event.
        let origin1 = now_ms() - 3_600_000;
        let ahead = origin1 + 120_000;
        let mut c = phone_content();
        c["created_ts"] = serde_json::json!(ahead);
        let first = json_members(c.clone(), origin1, None);
        // A refresh an hour later carries the same claim.
        let refreshed = json_members(c, now_ms() - 60_000, None);
        assert_eq!(first[0].session_tag(), refreshed[0].session_tag());
        assert_eq!(first[0].session_tag(), ahead.to_string());
        // A new session takes a new claim.
        let mut c2 = phone_content();
        c2["created_ts"] = serde_json::json!(now_ms() - 30_000);
        let rejoined = json_members(c2, now_ms() - 30_000, None);
        assert_ne!(first[0].session_tag(), rejoined[0].session_tag());
    }

    #[test]
    fn only_the_rooms_default_call_has_members() {
        assert!(is_room_call(None, None));
        assert!(is_room_call(Some("m.room"), Some("")));
        assert!(!is_room_call(Some("m.user"), Some("")));
        assert!(!is_room_call(Some("m.room"), Some("side-call")));
        let mut c = phone_content();
        c["scope"] = serde_json::json!("m.user");
        assert!(json_members(c, now_ms(), None).is_empty());
        let mut c = phone_content();
        c["call_id"] = serde_json::json!("other");
        assert!(json_members(c, now_ms(), None).is_empty());
        // The typed path is held to the same rule.
        let other_call = {
            let mut j = serde_json::to_value(content("D1", "multi_sfu", "https://s", None)).unwrap();
            j["call_id"] = serde_json::json!("other");
            serde_json::from_value::<CallMemberEventContent>(j).unwrap()
        };
        assert!(one(other_call, "k", "@b:b.org", 1).is_empty());
        let user_scoped = {
            let mut j = serde_json::to_value(content("D1", "multi_sfu", "https://s", None)).unwrap();
            j["scope"] = serde_json::json!("m.user");
            serde_json::from_value::<CallMemberEventContent>(j).unwrap()
        };
        assert!(one(user_scoped, "k", "@b:b.org", 1).is_empty());
    }

    #[test]
    fn a_typed_membership_cannot_use_a_future_created_ts_either() {
        let future = at_ms(now_ms() + 10 * 3_600_000);
        let m = one(content("D1", "oldest_membership", "https://s", Some(future)), "k", "@b:b.org", 1);
        assert_eq!(m[0].joined_at, ts(1), "the event's own time, not the claim");
    }

    #[test]
    fn empty_other_application_and_deviceless_content_is_ignored() {
        assert!(json_members(serde_json::json!({}), now_ms(), None).is_empty(), "a leave");
        let mut other = phone_content();
        other["application"] = serde_json::json!("m.whiteboard");
        assert!(json_members(other, now_ms(), None).is_empty());
        let mut no_device = phone_content();
        no_device.as_object_mut().unwrap().remove("device_id");
        assert!(json_members(no_device, now_ms(), None).is_empty());
    }

    #[test]
    fn our_own_device_is_excluded_when_asked() {
        let me = user_id!("@surak:example.org");
        let dev = owned_device_id!("PWV0V67kd3/SBN0d3sb5ur8GPNN0DXeWsZvv48B8BGw");
        assert!(json_members(phone_content(), now_ms(), Some((me, &dev))).is_empty());
        let other = owned_device_id!("SOMEOTHER");
        assert_eq!(json_members(phone_content(), now_ms(), Some((me, &other))).len(), 1);
    }

    #[test]
    fn an_event_without_a_service_url_is_a_member_without_a_transport() {
        let mut c = phone_content();
        c["foci_preferred"] = serde_json::json!([]);
        let got = json_members(c, now_ms(), None);
        assert_eq!(got.len(), 1);
        assert!(got[0].foci_preferred.is_empty());
    }

    #[test]
    fn call_id_is_read_from_either_membership_form() {
        assert_eq!(call_id_of(&serde_json::json!({"call_id": "abc", "application": "m.call"})), "abc");
        assert_eq!(
            call_id_of(&serde_json::json!({"memberships": [{"application": "m.call", "call_id": "xyz"}]})),
            "xyz"
        );
        assert_eq!(call_id_of(&serde_json::json!({"memberships": [{"application": "m.call"}]})), "");
        assert_eq!(call_id_of(&serde_json::json!({})), "");
    }

    // ── Room membership of the sender ──────────────────────────────────────────

    #[test]
    fn only_joined_senders_count_when_strict() {
        assert!(sender_counts(Some(&MembershipState::Join), JoinCheck::Strict));
        for gone in [MembershipState::Leave, MembershipState::Ban, MembershipState::Invite] {
            assert!(!sender_counts(Some(&gone), JoinCheck::Strict), "{gone:?}");
            assert!(!sender_counts(Some(&gone), JoinCheck::Cached), "{gone:?}");
        }
        assert!(!sender_counts(None, JoinCheck::Strict), "an unknown sender gets no key");
    }

    #[test]
    fn the_cached_check_keeps_senders_the_store_does_not_know() {
        assert!(sender_counts(None, JoinCheck::Cached));
    }

    #[test]
    fn the_known_joined_check_drops_senders_the_store_does_not_know() {
        let joined = Some(MembershipState::Join);
        assert!(sender_counts(joined.as_ref(), JoinCheck::KnownJoined));
        for gone in [MembershipState::Leave, MembershipState::Ban, MembershipState::Invite] {
            assert!(!sender_counts(Some(&gone), JoinCheck::KnownJoined), "{gone:?}");
        }
        assert!(!sender_counts(None, JoinCheck::KnownJoined), "missing from the store: no key");
        // A failed lookup with no earlier verdict is not joined either...
        assert!(!judge_sender(&SenderLookup::Unknown, None, JoinCheck::KnownJoined));
        assert!(!judge_sender(&SenderLookup::Known(None), None, JoinCheck::KnownJoined));
        // ...but still repeats a verdict the room gave earlier.
        assert!(judge_sender(&SenderLookup::Unknown, Some(true), JoinCheck::KnownJoined));
        assert!(!judge_sender(&SenderLookup::Unknown, Some(false), JoinCheck::KnownJoined));
        // What the store does not know is never remembered as a verdict.
        assert_eq!(lasting_verdict(&SenderLookup::Known(None), JoinCheck::KnownJoined), None);
        assert_eq!(lasting_verdict(&SenderLookup::Unknown, JoinCheck::KnownJoined), None);
    }

    #[test]
    fn a_failed_lookup_repeats_the_last_verdict() {
        let unknown = SenderLookup::Unknown;
        assert!(judge_sender(&unknown, Some(true), JoinCheck::Strict), "joined last time: still joined");
        assert!(!judge_sender(&unknown, Some(false), JoinCheck::Strict));
        assert!(!judge_sender(&unknown, None, JoinCheck::Strict), "no earlier verdict: fail closed");
        assert!(judge_sender(&unknown, None, JoinCheck::Cached), "the cheap read keeps what it cannot tell");
    }

    #[test]
    fn an_answer_from_the_room_beats_the_last_verdict() {
        let left = SenderLookup::Known(Some(MembershipState::Leave));
        assert!(!judge_sender(&left, Some(true), JoinCheck::Strict), "a kicked member is out at once");
        let joined = SenderLookup::Known(Some(MembershipState::Join));
        assert!(judge_sender(&joined, Some(false), JoinCheck::Strict));
        let nobody = SenderLookup::Known(None);
        assert!(!judge_sender(&nobody, Some(true), JoinCheck::Strict));
    }

    #[test]
    fn only_answers_from_the_room_are_remembered() {
        let join = SenderLookup::Known(Some(MembershipState::Join));
        let ban = SenderLookup::Known(Some(MembershipState::Ban));
        assert_eq!(lasting_verdict(&join, JoinCheck::Strict), Some(true));
        assert_eq!(lasting_verdict(&ban, JoinCheck::Cached), Some(false));
        assert_eq!(lasting_verdict(&SenderLookup::Known(None), JoinCheck::Strict), Some(false));
        assert_eq!(lasting_verdict(&SenderLookup::Known(None), JoinCheck::Cached), None, "the store just does not know");
        assert_eq!(lasting_verdict(&SenderLookup::Unknown, JoinCheck::Strict), None);
    }

    #[test]
    fn the_verdict_cache_is_per_room_and_bounded() {
        let mut c = VerdictCache::default();
        c.set("!r1:x", "@a:x", true);
        c.set("!r2:x", "@a:x", false);
        assert_eq!(c.get("!r1:x", "@a:x"), Some(true));
        assert_eq!(c.get("!r2:x", "@a:x"), Some(false));
        assert_eq!(c.get("!r1:x", "@b:x"), None);
        for i in 0..(VerdictCache::CAP * 2) {
            c.set("!r:x", &format!("@u{i}:x"), true);
        }
        assert!(c.map.len() <= VerdictCache::CAP);
    }

    #[test]
    fn a_kicked_users_live_membership_is_dropped() {
        let mut all = one(content("D1", "multi_sfu", "https://s", None), "k1", "@kicked:b.org", 1);
        all.extend(one(content("D2", "multi_sfu", "https://s", None), "k2", "@stays:b.org", 2));
        all.extend(one(content("D3", "multi_sfu", "https://s", None), "k3", "@stays:b.org", 3));
        let keep: HashSet<String> = ["@stays:b.org".to_owned()].into();
        let left = retain_senders(all, &keep);
        assert_eq!(left.len(), 2);
        assert!(left.iter().all(|m| m.user_id == "@stays:b.org"));
        assert!(retain_senders(left, &HashSet::new()).is_empty());
    }

    // ── Session tag ────────────────────────────────────────────────────────────

    #[test]
    fn a_legacy_session_is_told_apart_by_its_creation_time() {
        let first = one(content("D1", "multi_sfu", "https://s", Some(ts(10))), "same", "@b:b.org", 20);
        let again = one(content("D1", "multi_sfu", "https://s", Some(ts(10))), "same", "@b:b.org", 99);
        let restarted = one(content("D1", "multi_sfu", "https://s", Some(ts(500))), "same", "@b:b.org", 600);
        assert_eq!(first[0].key(), restarted[0].key(), "same user and device");
        assert_eq!(first[0].session_tag(), again[0].session_tag(), "a refresh keeps created_ts");
        assert_ne!(first[0].session_tag(), restarted[0].session_tag());
    }

    #[test]
    fn a_matrixrtc_session_is_its_member_id() {
        let mut m = one(content("D1", "multi_sfu", "https://s", None), "k", "@b:b.org", 1).remove(0);
        m.source = MemberSource::MatrixRtc;
        m.member_id = "mid-1".into();
        assert_eq!(m.session_tag(), "mid-1");
    }
}
