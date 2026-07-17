//! `ClientFfi` surface for the maps-link-to-location feature: resolving a
//! maps shortlink (blocking, background-thread-only) and sending an
//! `m.location` event. URL classification/resolution logic itself lives in
//! `crate::maps_link` — this module is just the FFI-facing glue.

use crate::ffi::{MapsLinkResult, OpResult};

#[cfg(not(test))]
use super::ClientFfi;
#[cfg(test)]
use super::{err, ClientFfi};

#[cfg(not(test))]
use crate::maps_link::{resolve_shortlink_with, ReqwestShortlinkFetcher};

#[cfg(not(test))]
use matrix_sdk::ruma::events::room::message::{
    LocationMessageEventContent, MessageType, RoomMessageEventContent,
};

fn no_match() -> MapsLinkResult {
    MapsLinkResult {
        matched: false,
        needs_resolve: false,
        lat: 0.0,
        lon: 0.0,
        shortlink_url: String::new(),
    }
}

impl ClientFfi {
    /// Follow `url`'s HTTP redirects (best-effort, `timeout_ms` budget) and
    /// classify the resolved target. Blocking — call only from a background
    /// thread. Returns `matched: false` on any failure, non-coordinate
    /// target, or timeout; callers should fall back to a plain-text send.
    #[cfg(not(test))]
    pub fn resolve_maps_shortlink(&self, url: &str, timeout_ms: u64) -> MapsLinkResult {
        let timeout = std::time::Duration::from_millis(timeout_ms.max(1));
        let fetcher = ReqwestShortlinkFetcher;
        self.rt.block_on(async move {
            match resolve_shortlink_with(&fetcher, url, timeout).await {
                Some((lat, lon)) => MapsLinkResult {
                    matched: true,
                    needs_resolve: false,
                    lat,
                    lon,
                    shortlink_url: String::new(),
                },
                None => no_match(),
            }
        })
    }

    #[cfg(test)]
    pub fn resolve_maps_shortlink(&self, _url: &str, _timeout_ms: u64) -> MapsLinkResult {
        no_match()
    }

    /// Send `body` as an `m.location` event (MSC3488) at the given
    /// coordinates. `body` is the plain-text fallback shown by clients that
    /// don't render locations.
    #[cfg(not(test))]
    pub fn send_location(&self, room_id: &str, lat: f64, lon: f64, body: &str) -> OpResult {
        let geo_uri = format!("geo:{lat:.6},{lon:.6}");
        let content = RoomMessageEventContent::new(MessageType::Location(
            LocationMessageEventContent::new(body.to_owned(), geo_uri),
        ));
        self.dispatch_room_msg_(room_id, content)
    }

    #[cfg(test)]
    pub fn send_location(&self, _room_id: &str, _lat: f64, _lon: f64, _body: &str) -> OpResult {
        err("not logged in")
    }
}
