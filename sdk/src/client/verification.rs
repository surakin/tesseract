//! SAS device verification (request, accept, start, confirm, cancel, get
//! emojis) plus the background watchers that observe verification request
//! and SAS state streams.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{lock_or_recover, SendHandler};

#[cfg(not(test))]
use crate::ffi::VerificationEmoji;

#[cfg(not(test))]
use std::collections::HashMap;

#[cfg(not(test))]
use std::sync::{Arc, Mutex};

#[cfg(not(test))]
use matrix_sdk::encryption::verification::SasVerification;

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
    emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
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
                    if let Ok(guard) = handler.lock() {
                        guard.on_verification_request(&flow_id, &user_id, &device_id, false);
                    }
                }
            VerificationRequestState::Transitioned { verification } => {
                if let Verification::SasV1(sas) = verification {
                    let h2 = Arc::clone(&handler);
                    let flow_id2 = flow_id.clone();
                    let emoji_cache2 = Arc::clone(&emoji_cache);
                    let handle = tokio::spawn(watch_sas(sas, flow_id2, h2, emoji_cache2));
                    lock_or_recover(&tasks).push(handle.abort_handle());
                }
                break;
            }
            VerificationRequestState::Done => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            VerificationRequestState::Cancelled(info) => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_cancelled(&flow_id, info.reason());
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

/// Watch a `SasVerification`'s state stream. Fires `on_sas_ready` when the
/// 7 emoji are available, `on_verification_done` on success, and
/// `on_verification_cancelled` on mismatch or cancel.
#[cfg(not(test))]
pub(super) async fn watch_sas(
    sas: SasVerification,
    flow_id: String,
    handler: Arc<Mutex<SendHandler>>,
    emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
) {
    use futures_util::StreamExt;
    use matrix_sdk::encryption::verification::SasState;

    let mut changes = sas.changes();

    while let Some(state) = changes.next().await {
        match state {
            SasState::KeysExchanged { emojis, .. } => {
                if let Some(emojis) = emojis {
                    let pairs: Vec<(String, String)> = emojis
                        .emojis
                        .iter()
                        .map(|e| (e.symbol.to_owned(), e.description.to_owned()))
                        .collect();
                    lock_or_recover(&emoji_cache).insert(flow_id.clone(), pairs.clone());
                    let ve: Vec<VerificationEmoji> = pairs
                        .into_iter()
                        .map(|(sym, desc)| VerificationEmoji {
                            symbol: sym,
                            description: desc,
                        })
                        .collect();
                    if let Ok(guard) = handler.lock() {
                        guard.on_sas_ready(&flow_id, &ve);
                    }
                }
            }
            SasState::Done { .. } => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&emoji_cache).remove(&flow_id);
                break;
            }
            SasState::Cancelled(info) => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_cancelled(&flow_id, &info.reason().to_string());
                }
                lock_or_recover(&emoji_cache).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

// ---------------------------------------------------------------------------
// FFI impls
// ---------------------------------------------------------------------------

impl ClientFfi {
    /// Initiate an `m.key.verification.request` to every other device of the
    /// current user. `on_verification_request(incoming=false)` fires when one
    /// device accepts; the UI should then call `start_sas(flow_id)`.
    #[cfg(not(test))]
    pub fn request_self_verification(&mut self) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("not syncing");
        };
        let flow_users = Arc::clone(&self.verification_flow_users);
        let emoji_cache = Arc::clone(&self.sas_emoji_cache);
        let tasks = Arc::clone(&self.verification_tasks);

        match self.rt.block_on(async move {
            let user_id = client
                .user_id()
                .ok_or_else(|| anyhow::anyhow!("not logged in"))?
                .to_owned();
            // Use the user identity (not the own device) so the request is
            // broadcast to all other E2EE sessions — not looped back to this
            // device, which would show an unwanted incoming-request banner.
            let identity = client
                .encryption()
                .get_user_identity(&user_id)
                .await?
                .ok_or_else(|| anyhow::anyhow!("own identity not found"))?;
            let req = identity.request_verification().await?;

            let flow_id = req.flow_id().to_owned();
            let user_id = req.own_user_id().as_str().to_owned();
            lock_or_recover(&flow_users).insert(flow_id.clone(), user_id);

            let tasks_for_register = Arc::clone(&tasks);
            let handle = tokio::spawn(watch_verification_request(
                req,
                flow_id,
                handler,
                flow_users,
                emoji_cache,
                tasks,
            ));
            lock_or_recover(&tasks_for_register).push(handle.abort_handle());
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn request_self_verification(&mut self) -> OpResult {
        err("not logged in")
    }

    /// Accept an incoming verification request. Call after receiving
    /// `on_verification_request(incoming=true)`, then call `start_sas`.
    #[cfg(not(test))]
    pub fn accept_verification(&mut self, flow_id: &str) -> OpResult {
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
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = client
                .encryption()
                .get_verification_request(uid, &flow_id)
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
    pub fn accept_verification(&mut self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Start the SAS key exchange. `on_sas_ready` fires when the 7 emoji are
    /// computed. Call after `accept_verification` (incoming) or after
    /// `on_verification_request(incoming=false)` (outgoing).
    #[cfg(not(test))]
    pub fn start_sas(&mut self, flow_id: &str) -> OpResult {
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
        let emoji_cache = Arc::clone(&self.sas_emoji_cache);
        let tasks = Arc::clone(&self.verification_tasks);

        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = client
                .encryption()
                .get_verification_request(uid, &flow_id_str)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification request not found"))?;
            let sas = req
                .start_sas()
                .await?
                .ok_or_else(|| anyhow::anyhow!("SAS not supported"))?;
            let handle = tokio::spawn(watch_sas(sas, flow_id_str, handler, emoji_cache));
            lock_or_recover(&tasks).push(handle.abort_handle());
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn start_sas(&mut self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Confirm that the SAS emoji match. Fires `on_verification_done` when
    /// both sides confirm. Call from the "They Match" button handler.
    #[cfg(not(test))]
    pub fn confirm_sas(&mut self, flow_id: &str) -> OpResult {
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
        match self.rt.block_on(async move {
            use matrix_sdk::encryption::verification::Verification;
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let verif = client
                .encryption()
                .get_verification(uid, &flow_id)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification not found"))?;
            if let Verification::SasV1(sas) = verif {
                sas.confirm().await?;
            }
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn confirm_sas(&mut self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Cancel or decline a verification flow (mismatch or user dismiss).
    #[cfg(not(test))]
    pub fn cancel_verification(&mut self, flow_id: &str) -> OpResult {
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
    pub fn cancel_verification(&mut self, _flow_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Return the 7 SAS emoji for `flow_id` after `on_sas_ready` has fired.
    #[cfg(not(test))]
    pub fn get_sas_emojis(&self, flow_id: &str) -> Vec<VerificationEmoji> {
        lock_or_recover(&self.sas_emoji_cache)
            .get(flow_id)
            .map(|pairs| {
                pairs
                    .iter()
                    .map(|(sym, desc)| VerificationEmoji {
                        symbol: sym.clone(),
                        description: desc.clone(),
                    })
                    .collect()
            })
            .unwrap_or_default()
    }
}
