//! SAS device verification (request, accept, start, confirm, cancel, get
//! codes) plus the background watchers that observe verification request
//! and SAS state streams.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::{OpResult, VerificationEmoji, VerificationSas};

#[cfg(not(test))]
use super::{lock_or_recover, SendHandler};

#[cfg(not(test))]
use std::collections::HashMap;

#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::sync::Arc;

#[cfg(not(test))]
use matrix_sdk::encryption::verification::SasVerification;

/// Per-flow SAS codes, filled by `watch_sas` and read by `get_sas`.
#[cfg(not(test))]
pub(super) type SasCache = Arc<Mutex<HashMap<String, VerificationSas>>>;

/// Build the FFI payload from a `KeysExchanged` state. The decimals are
/// always present; `emojis` is `None` when only the `decimal` method was
/// agreed, which leaves the emoji list empty.
fn sas_payload(emojis: Option<&[(&str, &str)]>, decimals: (u16, u16, u16)) -> VerificationSas {
    VerificationSas {
        emojis: emojis
            .unwrap_or_default()
            .iter()
            .map(|(symbol, description)| VerificationEmoji {
                symbol: (*symbol).to_owned(),
                description: (*description).to_owned(),
            })
            .collect(),
        decimals: [decimals.0, decimals.1, decimals.2],
    }
}

#[cfg(not(test))]
const VERIFICATION_LOOKUP_ATTEMPTS: usize = 7;
#[cfg(not(test))]
const VERIFICATION_LOOKUP_INITIAL_DELAY_MS: u64 = 50;

#[cfg(not(test))]
async fn lookup_verification_request_with_retry(
    client: &matrix_sdk::Client,
    user_id: &matrix_sdk::ruma::UserId,
    flow_id: &str,
) -> Option<matrix_sdk::encryption::verification::VerificationRequest> {
    let mut delay_ms = VERIFICATION_LOOKUP_INITIAL_DELAY_MS;
    for attempt in 0..VERIFICATION_LOOKUP_ATTEMPTS {
        if let Some(req) = client
            .encryption()
            .get_verification_request(user_id, flow_id)
            .await
        {
            return Some(req);
        }
        if attempt + 1 < VERIFICATION_LOOKUP_ATTEMPTS {
            tokio::time::sleep(std::time::Duration::from_millis(delay_ms)).await;
            delay_ms *= 2;
        }
    }
    None
}

#[cfg(not(test))]
async fn lookup_sas_with_retry(
    client: &matrix_sdk::Client,
    user_id: &matrix_sdk::ruma::UserId,
    flow_id: &str,
) -> Option<SasVerification> {
    use matrix_sdk::encryption::verification::Verification;

    let mut delay_ms = VERIFICATION_LOOKUP_INITIAL_DELAY_MS;
    for attempt in 0..VERIFICATION_LOOKUP_ATTEMPTS {
        if let Some(Verification::SasV1(sas)) =
            client.encryption().get_verification(user_id, flow_id).await
        {
            return Some(sas);
        }
        if attempt + 1 < VERIFICATION_LOOKUP_ATTEMPTS {
            tokio::time::sleep(std::time::Duration::from_millis(delay_ms)).await;
            delay_ms *= 2;
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Background watchers
// ---------------------------------------------------------------------------

/// Watch a verification request's state stream. Fires `on_verification_request`
/// (incoming=false) when the request is accepted (Ready state), then spawns a
/// SAS watcher when the flow transitions to `Transitioned { SasV1 }`.
/// Also surfaces top-level Done / Cancelled transitions before SAS starts.
#[cfg(not(test))]
pub(super) async fn watch_verification_request(
    req: matrix_sdk::encryption::verification::VerificationRequest,
    flow_id: String,
    handler: Arc<Mutex<SendHandler>>,
    flow_users: Arc<Mutex<HashMap<String, String>>>,
    sas_cache: SasCache,
    tasks: Arc<Mutex<Vec<tokio::task::AbortHandle>>>,
) {
    use futures_util::StreamExt;
    use matrix_sdk::encryption::verification::{Verification, VerificationRequestState};

    let user_id = req.other_user_id().as_str().to_owned();
    let we_started = req.we_started();

    let mut changes = req.changes();

    while let Some(state) = changes.next().await {
        match state {
            VerificationRequestState::Ready {
                ref other_device_data,
                ..
            }
                // The other side accepted our outgoing request. Signal the UI
                // to transition from Waiting and call start_sas.
                if we_started => {
                    let device_id = other_device_data.device_id().as_str().to_owned();
                    {
                        let guard = handler.lock();
                        guard.on_verification_request(&flow_id, &user_id, &device_id, false);
                    }
                }
            VerificationRequestState::Transitioned { verification } => {
                if let Verification::SasV1(sas) = verification {
                    let h2 = Arc::clone(&handler);
                    let flow_id2 = flow_id.clone();
                    let sas_cache2 = Arc::clone(&sas_cache);
                    let handle = tokio::spawn(watch_sas(sas, flow_id2, h2, sas_cache2));
                    track_task(&tasks, handle.abort_handle());
                }
                break;
            }
            VerificationRequestState::Done => {
                {
                    let guard = handler.lock();
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            VerificationRequestState::Cancelled(info) => {
                {
                    let guard = handler.lock();
                    guard.on_verification_cancelled(&flow_id, info.cancel_code().as_str());
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

/// Watch a `SasVerification`'s state stream. Fires `on_sas_ready` when the
/// SAS codes are available (decimals always, emoji when negotiated), `on_verification_done` on success, and
/// `on_verification_cancelled` on mismatch or cancel.
#[cfg(not(test))]
pub(super) async fn watch_sas(
    sas: SasVerification,
    flow_id: String,
    handler: Arc<Mutex<SendHandler>>,
    sas_cache: SasCache,
) {
    use futures_util::StreamExt;
    use matrix_sdk::encryption::verification::SasState;

    let mut changes = sas.changes();

    while let Some(state) = changes.next().await {
        match state {
            SasState::KeysExchanged { emojis, decimals } => {
                // A peer that only offers `decimal` (MSC4405) leaves `emojis`
                // unset; the decimals are always there.
                let pairs: Option<Vec<(&str, &str)>> =
                    emojis.map(|e| e.emojis.iter().map(|e| (e.symbol, e.description)).collect());
                let payload = sas_payload(pairs.as_deref(), decimals);
                lock_or_recover(&sas_cache).insert(flow_id.clone(), payload.clone());
                {
                    let guard = handler.lock();
                    guard.on_sas_ready(&flow_id, &payload);
                }
            }
            SasState::Done { .. } => {
                {
                    let guard = handler.lock();
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&sas_cache).remove(&flow_id);
                break;
            }
            SasState::Cancelled(info) => {
                {
                    let guard = handler.lock();
                    guard.on_verification_cancelled(&flow_id, info.cancel_code().as_str());
                }
                lock_or_recover(&sas_cache).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// FFI impls
// ---------------------------------------------------------------------------

/// Remember a watcher so sign-out can abort it, forgetting the finished ones
/// first: a peer that spams requests must not grow the list without bound.
#[cfg(not(test))]
fn track_task(tasks: &Mutex<Vec<tokio::task::AbortHandle>>, handle: tokio::task::AbortHandle) {
    let mut tasks = lock_or_recover(tasks);
    tasks.retain(|t| !t.is_finished());
    tasks.push(handle);
}

impl ClientFfi {
    /// Initiate an `m.key.verification.request` to every other device of the
    /// current user. On success the result's message is the new flow id, so
    /// the UI can track (and cancel, or hear a decline for) the request before
    /// any device answers. `on_verification_request(incoming=false)` fires
    /// when one device accepts; the UI should then call `start_sas(flow_id)`.
    #[cfg(not(test))]
    pub fn request_self_verification(&self) -> OpResult {
        self.request_verification_of(None)
    }

    /// Request verification of another user's cross-signing identity.
    /// matrix-sdk sends the request in the DM shared with them, creating the
    /// DM when there is none. On success the result's message is the flow id
    /// (the request event's id); `on_verification_request(incoming=false)`
    /// fires once they accept.
    #[cfg(not(test))]
    pub fn request_user_verification(&self, user_id: &str) -> OpResult {
        if user_id.is_empty() {
            return err("empty user id");
        }
        self.request_verification_of(Some(user_id))
    }

    #[cfg(test)]
    pub fn request_user_verification(&self, _user_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Shared body of the two request functions: `target` is the other user,
    /// or `None` for our own identity (every other device of ours).
    #[cfg(not(test))]
    fn request_verification_of(&self, target: Option<&str>) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("not syncing");
        };
        let flow_users = Arc::clone(&self.verification_flow_users);
        let sas_cache = Arc::clone(&self.sas_cache);
        let tasks = Arc::clone(&self.verification_tasks);
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "verification/start".to_string(),
        );

        let target = target.map(str::to_owned);
        match self.rt.block_on(async move {
            let user_id = match target {
                Some(t) => matrix_sdk::ruma::OwnedUserId::try_from(t.as_str())?,
                None => client
                    .user_id()
                    .ok_or_else(|| anyhow::anyhow!("not logged in"))?
                    .to_owned(),
            };
            // Use the user identity (not a device) so a self-request is
            // broadcast to all other E2EE sessions — not looped back to this
            // device, which would show an unwanted incoming-request banner —
            // and another user's goes to the DM we share with them.
            let identity = client
                .encryption()
                .get_user_identity(&user_id)
                .await?
                .ok_or_else(|| anyhow::anyhow!("no cross-signing identity for this user"))?;
            let req = identity.request_verification().await?;

            let flow_id = req.flow_id().to_owned();
            let user_id = req.other_user_id().as_str().to_owned();
            lock_or_recover(&flow_users).insert(flow_id.clone(), user_id);
            let reported_flow_id = flow_id.clone();

            let tasks_for_register = Arc::clone(&tasks);
            let handle = tokio::spawn(watch_verification_request(
                req, flow_id, handler, flow_users, sas_cache, tasks,
            ));
            lock_or_recover(&tasks_for_register).push(handle.abort_handle());
            Ok::<String, anyhow::Error>(reported_flow_id)
        }) {
            Ok(flow_id) => ok(&flow_id),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn request_self_verification(&self) -> OpResult {
        err("not logged in")
    }

    /// Accept an incoming verification request. Call after receiving
    /// `on_verification_request(incoming=true)`, then call `start_sas`.
    #[cfg(not(test))]
    pub fn accept_verification(&self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let user_id = match lock_or_recover(&self.verification_flow_users)
            .get(flow_id)
            .cloned()
        {
            Some(u) => u,
            None => return err("no pending verification request for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "verification/accept".to_string(),
        );
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = lookup_verification_request_with_retry(&client, uid, &flow_id)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification request not found"))?;
            req.accept().await?;
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn accept_verification(&self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Start the SAS key exchange. `on_sas_ready` fires when the SAS codes
    /// are computed. Call after `accept_verification` (incoming) or after
    /// `on_verification_request(incoming=false)` (outgoing).
    #[cfg(not(test))]
    pub fn start_sas(&self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let user_id = match lock_or_recover(&self.verification_flow_users)
            .get(flow_id)
            .cloned()
        {
            Some(u) => u,
            None => return err("no pending verification request for this flow_id"),
        };
        let flow_id_str = flow_id.to_owned();
        let Some(handler) = self.handler.clone() else {
            return err("not syncing");
        };
        let sas_cache = Arc::clone(&self.sas_cache);
        let tasks = Arc::clone(&self.verification_tasks);
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "verification/start_sas".to_string(),
        );

        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = lookup_verification_request_with_retry(&client, uid, &flow_id_str)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification request not found"))?;
            let sas = match req.start_sas().await {
                Ok(Some(sas)) => sas,
                Ok(None) => lookup_sas_with_retry(&client, uid, &flow_id_str)
                    .await
                    .ok_or_else(|| anyhow::anyhow!("SAS not supported"))?,
                Err(e) => {
                    if let Some(sas) = lookup_sas_with_retry(&client, uid, &flow_id_str).await {
                        sas
                    } else {
                        return Err(e.into());
                    }
                }
            };
            let handle = tokio::spawn(watch_sas(sas, flow_id_str, handler, sas_cache));
            track_task(&tasks, handle.abort_handle());
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn start_sas(&self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Confirm that the SAS codes match. Fires `on_verification_done` when
    /// both sides confirm. Call from the "They Match" button handler.
    #[cfg(not(test))]
    pub fn confirm_sas(&self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let user_id = match lock_or_recover(&self.verification_flow_users)
            .get(flow_id)
            .cloned()
        {
            Some(u) => u,
            None => return err("no active SAS for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "verification/confirm".to_string(),
        );
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let sas = lookup_sas_with_retry(&client, uid, &flow_id)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification not found"))?;
            sas.confirm().await?;
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn confirm_sas(&self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Cancel or decline a verification flow (mismatch or user dismiss).
    #[cfg(not(test))]
    pub fn cancel_verification(&self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let user_id = match lock_or_recover(&self.verification_flow_users)
            .get(flow_id)
            .cloned()
        {
            Some(u) => u,
            None => return err("no active verification for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "verification/cancel".to_string(),
        );
        match self.rt.block_on(async move {
            use matrix_sdk::encryption::verification::Verification;
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            // Try the SAS object first; fall back to the request.
            if let Some(Verification::SasV1(sas)) =
                client.encryption().get_verification(uid, &flow_id).await
            {
                sas.cancel().await?;
            } else if let Some(req) = client
                .encryption()
                .get_verification_request(uid, &flow_id)
                .await
            {
                req.cancel().await?;
            }
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn cancel_verification(&self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Return the SAS codes for `flow_id` after `on_sas_ready` has fired.
    #[cfg(not(test))]
    pub fn get_sas(&self, flow_id: &str) -> VerificationSas {
        lock_or_recover(&self.sas_cache)
            .get(flow_id)
            .cloned()
            .unwrap_or_else(|| sas_payload(None, (0, 0, 0)))
    }
}

#[cfg(test)]
mod tests {
    use super::sas_payload;

    #[test]
    fn sas_payload_decimal_only_has_no_emoji() {
        let sas = sas_payload(None, (1234, 5678, 9191));
        assert!(sas.emojis.is_empty());
        assert_eq!(sas.decimals, [1234, 5678, 9191]);
    }

    #[test]
    fn sas_payload_keeps_emoji_order_and_decimals() {
        let emojis = [("🐶", "Dog"), ("🐱", "Cat")];
        let sas = sas_payload(Some(&emojis), (1000, 2000, 3000));
        assert_eq!(sas.decimals, [1000, 2000, 3000]);
        let got: Vec<(&str, &str)> = sas
            .emojis
            .iter()
            .map(|e| (e.symbol.as_str(), e.description.as_str()))
            .collect();
        assert_eq!(got, vec![("🐶", "Dog"), ("🐱", "Cat")]);
    }
}

/// matrix-sdk ignores verification requests older than this (spec: 10 min).
const VERIFICATION_REQUEST_MAX_AGE_SECS: u64 = 10 * 60;

/// Whether an in-room m.key.verification.request should be surfaced: sent
/// to us by someone else, and recent enough to still be answerable (sync
/// also replays old requests from room history).
pub(super) fn is_live_in_room_request_for(
    me: &matrix_sdk::ruma::UserId,
    sender: &matrix_sdk::ruma::UserId,
    to: &matrix_sdk::ruma::UserId,
    sent_secs: u64,
    now_secs: u64,
) -> bool {
    to == me
        && sender != me
        && now_secs.saturating_sub(sent_secs) < VERIFICATION_REQUEST_MAX_AGE_SECS
}

#[cfg(test)]
mod in_room_request_tests {
    use super::is_live_in_room_request_for;
    use matrix_sdk::ruma::user_id;

    #[test]
    fn accepts_recent_request_addressed_to_us() {
        let me = user_id!("@me:x.org");
        assert!(is_live_in_room_request_for(
            me,
            user_id!("@a:x.org"),
            me,
            1_000,
            1_060
        ));
    }

    #[test]
    fn rejects_request_for_someone_else_or_from_us() {
        let me = user_id!("@me:x.org");
        let other = user_id!("@a:x.org");
        assert!(!is_live_in_room_request_for(
            me,
            other,
            user_id!("@b:x.org"),
            1_000,
            1_000
        ));
        assert!(!is_live_in_room_request_for(me, me, me, 1_000, 1_000));
    }

    #[test]
    fn rejects_expired_request() {
        let me = user_id!("@me:x.org");
        assert!(!is_live_in_room_request_for(
            me,
            user_id!("@a:x.org"),
            me,
            0,
            601
        ));
    }
}
