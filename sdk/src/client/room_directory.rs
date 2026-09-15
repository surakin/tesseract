//! Public room directory browsing (MSC-free, plain `POST /publicRooms`).
//!
//! Wraps matrix-sdk's `room_directory_search::RoomDirectorySearch`, a
//! ready-made paginated helper over `Client::public_rooms_filtered`. Search
//! state (the `since` token + accumulated results) is stateful across
//! `search()`/`next_page()` calls, so — following this file's usual
//! convention of never handing C++ a second opaque handle — the
//! `RoomDirectorySearch` itself lives inside `ClientFfi::room_directory_searches`,
//! keyed by the caller-chosen `request_id`. Async work is spawned as a
//! detached tokio task and delivers results via `EventHandlerBridge`,
//! mirroring `paginate_back_async` (see `timeline.rs`).

#[cfg(not(test))]
use std::sync::Arc;

#[cfg(not(test))]
use matrix_sdk::room_directory_search::{RoomDescription, RoomDirectorySearch};
#[cfg(not(test))]
use matrix_sdk::ruma::room::JoinRuleKind;
#[cfg(not(test))]
use matrix_sdk::ruma::ServerName;

#[cfg(not(test))]
use super::{ClientFfi, SendHandler};
#[cfg(not(test))]
use parking_lot::Mutex;

/// Bound on how long a single search()/next_page() round trip may take.
/// `server` in the request is relayed by *our own* homeserver via
/// federation (per-spec, `publicRooms`'s `server` param is served by the
/// local server, not the remote one directly) — an unreachable or
/// non-existent target server means our homeserver's own federation
/// attempt hangs, which would otherwise inherit the SDK's 60s default
/// request timeout. This mirrors (in spirit, not by sharing code —
/// `discover_homeserver`'s well-known/client-API probe doesn't apply to a
/// server-to-server federation relay) LoginView's bounded homeserver
/// validation, so a bad "browse via server" fails fast with a friendly
/// message instead of a minute-long hang.
#[cfg(not(test))]
const ROOM_DIRECTORY_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);

/// Batch size requested per page — matches the batch size used by other
/// paginated list fetches in this codebase (room media gallery pages).
#[cfg(not(test))]
const ROOM_DIRECTORY_BATCH_SIZE: u32 = 20;

#[cfg(not(test))]
fn join_rule_str(rule: &JoinRuleKind) -> String {
    match rule {
        JoinRuleKind::Public => "public",
        JoinRuleKind::Knock => "knock",
        JoinRuleKind::Invite => "invite",
        JoinRuleKind::Restricted => "restricted",
        JoinRuleKind::KnockRestricted => "knock_restricted",
        JoinRuleKind::Private => "private",
        _ => "unknown",
    }
    .to_string()
}

#[cfg(not(test))]
fn to_ffi_entry(d: &RoomDescription) -> crate::ffi::RoomDirectoryEntryFfi {
    crate::ffi::RoomDirectoryEntryFfi {
        room_id: d.room_id.to_string(),
        name: d.name.clone().unwrap_or_default(),
        topic: d.topic.clone().unwrap_or_default(),
        alias: d.alias.as_ref().map(|a| a.to_string()).unwrap_or_default(),
        avatar_url: d
            .avatar_url
            .as_ref()
            .map(|a| a.to_string())
            .unwrap_or_default(),
        join_rule: join_rule_str(&d.join_rule),
        is_world_readable: d.is_world_readable,
        joined_members: d.joined_members,
    }
}

/// Snapshot the currently loaded results (already-delivered pages included)
/// and whether the server has more pages.
#[cfg(not(test))]
fn snapshot(search: &RoomDirectorySearch) -> (Vec<crate::ffi::RoomDirectoryEntryFfi>, bool) {
    let (results, _stream) = search.results();
    let entries = results.iter().map(to_ffi_entry).collect();
    (entries, search.is_at_last_page())
}

#[cfg(not(test))]
fn deliver_page(
    handler: &Option<Arc<Mutex<SendHandler>>>,
    request_id: u64,
    entries: &Vec<crate::ffi::RoomDirectoryEntryFfi>,
    reached_end: bool,
) {
    let Some(h) = handler else { return };
    let g = h.lock();
    g.on_room_directory_search_results(request_id, entries, reached_end);
}

#[cfg(not(test))]
fn deliver_failed(handler: &Option<Arc<Mutex<SendHandler>>>, request_id: u64, message: &str) {
    let Some(h) = handler else { return };
    let g = h.lock();
    g.on_room_directory_search_failed(request_id, message);
}

/// Friendly message for a search()/next_page() that hit ROOM_DIRECTORY_TIMEOUT.
/// Named after `server_input` when browsing via another homeserver (empty
/// means the account's own homeserver), mirroring `discover_homeserver`'s
/// "{server} took too long to respond" wording.
#[cfg(not(test))]
fn timeout_message(server_input: &str) -> String {
    if server_input.is_empty() {
        "Your homeserver took too long to respond".to_string()
    } else {
        format!("{server_input} took too long to respond")
    }
}

#[cfg(not(test))]
impl ClientFfi {
    /// Start (or restart) a public-room-directory search. Clears any
    /// previous search stored under `request_id`. `filter` is a
    /// server-side search term (empty = no filter, i.e. list everything);
    /// `server` optionally names another homeserver to browse via
    /// federation (empty = the logged-in account's own homeserver).
    /// Delivers the first page via `on_room_directory_search_results`, or
    /// `on_room_directory_search_failed` on error. Non-blocking.
    pub fn room_directory_search_start_async(&self, request_id: u64, filter: &str, server: &str) {
        let Some(client) = self.client.clone() else {
            deliver_failed(&self.handler, request_id, "not logged in");
            return;
        };
        let handler = self.handler.clone();
        let filter = filter.trim();
        let filter = if filter.is_empty() {
            None
        } else {
            Some(filter.to_owned())
        };
        let server_input = server.trim().to_owned();
        let searches = Arc::clone(&self.room_directory_searches);
        let in_flight = Arc::clone(&self.in_flight);
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);

        // Drop any previous search stored under this id up front so a
        // restarted search (fresh id, same slot never reused by the UI) does
        // not leak; the UI is expected to pick a fresh request_id per search
        // and call cancel_room_directory_search when the Browse tab closes.
        searches.lock().remove(&request_id);

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_directory/search".to_string(),
            );

            let via_server = if server_input.is_empty() {
                None
            } else {
                match <&ServerName>::try_from(server_input.as_str()) {
                    Ok(s) => Some(s.to_owned()),
                    Err(e) => {
                        deliver_failed(
                            &handler,
                            request_id,
                            &format!("invalid server name: {e}"),
                        );
                        return;
                    }
                }
            };

            let mut search = RoomDirectorySearch::new(client);
            match tokio::time::timeout(
                ROOM_DIRECTORY_TIMEOUT,
                search.search(filter, ROOM_DIRECTORY_BATCH_SIZE, via_server),
            )
            .await
            {
                Ok(Ok(())) => {
                    let (entries, reached_end) = snapshot(&search);
                    deliver_page(&handler, request_id, &entries, reached_end);
                    searches.lock().insert(request_id, search);
                }
                Ok(Err(e)) => deliver_failed(&handler, request_id, &e.to_string()),
                Err(_) => {
                    deliver_failed(&handler, request_id, &timeout_message(&server_input))
                }
            }
        });
    }

    /// Fetch the next page of a search started by `room_directory_search_start_async`.
    /// Delivers only the newly-appended rows via `on_room_directory_search_results`
    /// (the UI appends them to what it already has). No-op (delivers an empty
    /// page with `reached_end = true`) if `request_id` is unknown or the
    /// search already reached its last page. Non-blocking.
    pub fn room_directory_next_page_async(&self, request_id: u64) {
        let Some(mut search) = self.room_directory_searches.lock().remove(&request_id) else {
            deliver_failed(
                &self.handler,
                request_id,
                "no active room directory search for this request id",
            );
            return;
        };
        let handler = self.handler.clone();
        let searches = Arc::clone(&self.room_directory_searches);
        let in_flight = Arc::clone(&self.in_flight);
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_directory/next_page".to_string(),
            );

            let prev_len = search.results().0.len();
            match tokio::time::timeout(ROOM_DIRECTORY_TIMEOUT, search.next_page()).await {
                Ok(Ok(())) => {
                    let (all_entries, reached_end) = snapshot(&search);
                    let new_entries: Vec<_> = all_entries.into_iter().skip(prev_len).collect();
                    deliver_page(&handler, request_id, &new_entries, reached_end);
                    searches.lock().insert(request_id, search);
                }
                Ok(Err(e)) => {
                    deliver_failed(&handler, request_id, &e.to_string());
                    // Put the search back so the UI can retry the same page.
                    searches.lock().insert(request_id, search);
                }
                Err(_) => {
                    // next_page() doesn't know the server name (RoomDirectorySearch
                    // has no public getter for it) — generic wording.
                    deliver_failed(&handler, request_id, &timeout_message(""));
                    searches.lock().insert(request_id, search);
                }
            }
        });
    }

    /// Drop the search stored under `request_id` (Browse tab closed, or a
    /// fresh search is about to replace it). No callback fires.
    pub fn cancel_room_directory_search(&self, request_id: u64) {
        self.room_directory_searches.lock().remove(&request_id);
    }
}
