//! QR-code grant login (MSC4108): existing (logged-in) device generates a QR
//! code for a new device to scan. The six bridge methods below drive the full
//! handshake from the existing-device side.
//!
//! State machine (GrantLoginProgress<GeneratedQrProgress>):
//!   Starting
//!   → EstablishingSecureChannel(QrReady(qr_data))   — send bitmap to C++
//!   → EstablishingSecureChannel(QrScanned(sender))  — await check code from C++
//!   → WaitingForAuth { verification_uri }           — send URI to C++
//!   → SyncingSecrets                                — no action needed
//!   → Done                                          — signal completion

use std::future::IntoFuture as _;

use futures_util::StreamExt as _;
use matrix_sdk::authentication::oauth::qrcode::{GeneratedQrProgress, GrantLoginProgress};
use matrix_sdk_base::crypto::types::qr_login::QrCodeData;
use tokio::sync::{oneshot, watch};

use super::{err, ok, ClientFfi};
use crate::ffi::{OpResult, QrGrantAuth, QrGrantBitmap};

// ---------------------------------------------------------------------------
// Public handle — stored in ClientFfi
// ---------------------------------------------------------------------------

/// Channels kept after `qr_grant_start` returns the bitmap to C++.
pub(super) struct QrGrantHandle {
    /// Fires once the new device has scanned the QR code.
    scanned_rx: Option<oneshot::Receiver<()>>,
    /// Sender for the check code entered by the user. `Option` so it can be
    /// taken exactly once by `qr_grant_submit_check_code`.
    check_code_tx: Option<oneshot::Sender<u8>>,
    /// Fires once the flow reaches `WaitingForAuth` (carrying the URI).
    auth_rx: Option<oneshot::Receiver<String>>,
    /// Fires once the flow is fully done (Ok) or has failed (Err).
    done_rx: Option<oneshot::Receiver<Result<(), String>>>,
    /// Send `true` to abort the background task at any point.
    cancel_tx: watch::Sender<bool>,
}

// ---------------------------------------------------------------------------
// Bridge method implementations
// ---------------------------------------------------------------------------

impl ClientFfi {
    pub fn qr_grant_start(&mut self) -> QrGrantBitmap {
        // Cancel any already-running flow.
        if let Some(h) = self.qr_grant.take() {
            let _ = h.cancel_tx.send(true);
        }

        let Some(client) = self.client.as_ref() else {
            return bitmap_err("not logged in");
        };
        let client = client.clone();

        // Channels
        let (bitmap_tx, bitmap_rx) = oneshot::channel::<(Vec<u8>, u32)>();
        let (scanned_tx, scanned_rx) = oneshot::channel::<()>();
        let (check_tx, check_rx) = oneshot::channel::<u8>();
        let (auth_tx, auth_rx) = oneshot::channel::<String>();
        let (done_tx, done_rx) = oneshot::channel::<Result<(), String>>();
        let (cancel_tx, cancel_rx) = watch::channel(false);

        self.rt.spawn(run_grant_flow(
            client,
            bitmap_tx,
            scanned_tx,
            check_rx,
            auth_tx,
            done_tx,
            cancel_rx,
        ));

        // Block until the QR bitmap is ready (or the flow fails immediately).
        match self.rt.block_on(bitmap_rx) {
            Ok((pixels, side)) => {
                self.qr_grant = Some(QrGrantHandle {
                    scanned_rx: Some(scanned_rx),
                    check_code_tx: Some(check_tx),
                    auth_rx: Some(auth_rx),
                    done_rx: Some(done_rx),
                    cancel_tx,
                });
                QrGrantBitmap { ok: true, message: String::new(), pixels, side }
            }
            Err(_) => bitmap_err("QR grant flow failed before producing a bitmap"),
        }
    }

    pub fn qr_grant_await_scanned(&mut self) -> OpResult {
        let Some(h) = self.qr_grant.as_mut() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let Some(rx) = h.scanned_rx.take() else {
            return err("qr_grant_await_scanned already called");
        };
        match self.rt.block_on(rx) {
            Ok(()) => ok(""),
            Err(_) => err("QR grant flow ended before the QR code was scanned"),
        }
    }

    pub fn qr_grant_submit_check_code(&mut self, code: u8) -> OpResult {
        let Some(h) = self.qr_grant.as_mut() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let Some(tx) = h.check_code_tx.take() else {
            return err("check code already submitted");
        };
        match tx.send(code) {
            Ok(()) => ok(""),
            Err(_) => err("QR grant flow has already ended; check code could not be sent"),
        }
    }

    pub fn qr_grant_await_auth(&mut self) -> QrGrantAuth {
        let Some(h) = self.qr_grant.as_mut() else {
            return auth_err("no QR grant flow in progress; call qr_grant_start first");
        };
        let Some(rx) = h.auth_rx.take() else {
            return auth_err("qr_grant_await_auth already called");
        };
        match self.rt.block_on(rx) {
            Ok(uri) => QrGrantAuth { ok: true, message: String::new(), verification_uri: uri },
            Err(_) => auth_err("QR grant flow ended before reaching WaitingForAuth"),
        }
    }

    pub fn qr_grant_await_complete(&mut self) -> OpResult {
        let Some(h) = self.qr_grant.as_mut() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let Some(rx) = h.done_rx.take() else {
            return err("qr_grant_await_complete already called");
        };
        match self.rt.block_on(rx) {
            Ok(Ok(())) => ok(""),
            Ok(Err(msg)) => err(msg),
            Err(_) => err("QR grant flow task panicked or was cancelled"),
        }
    }

    pub fn qr_grant_cancel(&mut self) {
        if let Some(h) = self.qr_grant.take() {
            let _ = h.cancel_tx.send(true);
        }
    }
}

// ---------------------------------------------------------------------------
// Async background task
//
// We create both the `OAuth` handle and the `GrantLoginWithGeneratedQrCode`
// future inside the spawned task so all borrows are contained within a
// `'static` async block — no lifetime escapes.
// ---------------------------------------------------------------------------

async fn run_grant_flow(
    client: matrix_sdk::Client,
    bitmap_tx: oneshot::Sender<(Vec<u8>, u32)>,
    scanned_tx: oneshot::Sender<()>,
    check_rx: oneshot::Receiver<u8>,
    auth_tx: oneshot::Sender<String>,
    done_tx: oneshot::Sender<Result<(), String>>,
    mut cancel_rx: watch::Receiver<bool>,
) {
    // Build the grant and the progress stream inside the task so no external
    // lifetime borrow is needed.  `oauth()` returns a temporary; bind it to
    // a named local so it lives long enough for the borrow in `subscribe_to_progress`.
    let oauth = client.oauth();
    let grant = oauth.grant_login_with_qr_code().generate();
    let mut progress = grant.subscribe_to_progress();

    // Wrap senders in Option so they can be consumed exactly once even though
    // the match arms live inside a loop.
    let mut bitmap_tx = Some(bitmap_tx);
    let mut scanned_tx = Some(scanned_tx);
    let mut check_rx = Some(check_rx);
    let mut auth_tx = Some(auth_tx);
    let mut done_tx = Some(done_tx);

    // Drive the grant future to completion while observing progress.
    // `GrantLoginWithGeneratedQrCode` implements `IntoFuture`, not `Future`
    // directly — call `.into_future()` to get the underlying boxed future.
    let grant_fut = grant.into_future();
    tokio::pin!(grant_fut);

    let mut grant_done = false;

    loop {
        tokio::select! {
            biased;

            // Cancellation wins immediately.
            _ = cancel_rx.changed() => {
                // Drop grant_fut to abort the in-flight HTTP connections.
                drop(grant_fut);
                if let Some(tx) = done_tx.take() {
                    let _ = tx.send(Err("cancelled".into()));
                }
                return;
            }

            // Drive the grant future to completion.
            result = &mut grant_fut, if !grant_done => {
                grant_done = true;
                match result {
                    Ok(()) => {
                        if let Some(tx) = done_tx.take() {
                            let _ = tx.send(Ok(()));
                        }
                    }
                    Err(e) => {
                        if let Some(tx) = done_tx.take() {
                            let _ = tx.send(Err(e.to_string()));
                        }
                    }
                }
                // Don't break yet — drain remaining progress events first so
                // the WaitingForAuth signal isn't lost.
            }

            maybe = progress.next() => {
                match maybe {
                    Some(GrantLoginProgress::EstablishingSecureChannel(
                        GeneratedQrProgress::QrReady(qr_data)
                    )) => {
                        match render_qr(&qr_data) {
                            Some((pixels, side)) => {
                                if let Some(tx) = bitmap_tx.take() {
                                    let _ = tx.send((pixels, side));
                                }
                            }
                            None => {
                                if let Some(tx) = done_tx.take() {
                                    let _ = tx.send(Err("failed to render QR code".into()));
                                }
                                return;
                            }
                        }
                    }

                    Some(GrantLoginProgress::EstablishingSecureChannel(
                        GeneratedQrProgress::QrScanned(sender)
                    )) => {
                        if let Some(tx) = scanned_tx.take() {
                            let _ = tx.send(());
                        }
                        // Wait for C++ to supply the check code.
                        if let Some(rx) = check_rx.take() {
                            if let Ok(code) = rx.await {
                                let _ = sender.send(code).await;
                            }
                        }
                    }

                    Some(GrantLoginProgress::WaitingForAuth { verification_uri }) => {
                        if let Some(tx) = auth_tx.take() {
                            let _ = tx.send(verification_uri.to_string());
                        }
                    }

                    Some(GrantLoginProgress::Done) | None => {
                        // Stream ended — if grant_fut hasn't fired yet we'll
                        // catch it on the next iteration; if it already fired
                        // we're truly done.
                        if grant_done {
                            break;
                        }
                    }

                    // Starting, SyncingSecrets — no action needed.
                    _ => {}
                }
            }
        }

    }
}

// ---------------------------------------------------------------------------
// QR pixel rendering
// ---------------------------------------------------------------------------

fn render_qr(data: &QrCodeData) -> Option<(Vec<u8>, u32)> {
    let bytes = data.to_bytes();
    let code = match qrcode::QrCode::new(&bytes) {
        Ok(c) => c,
        Err(e) => {
            tracing::error!("Failed to create QR code: {e}");
            return None;
        }
    };

    let colors = code.to_colors();
    let modules = code.width();
    let scale: u32 = 4;
    let quiet: u32 = 4;
    let side = (modules as u32 + 2 * quiet) * scale;

    // All bytes start as 255 (white, fully opaque RGBA).
    let mut pixels = vec![255u8; (side * side * 4) as usize];

    for (i, color) in colors.iter().enumerate() {
        if matches!(color, qrcode::Color::Dark) {
            let row = i as u32 / modules as u32;
            let col = i as u32 % modules as u32;
            for dy in 0..scale {
                for dx in 0..scale {
                    let base = (((row + quiet) * scale + dy) * side
                        + (col + quiet) * scale
                        + dx) as usize
                        * 4;
                    pixels[base] = 0; // R
                    pixels[base + 1] = 0; // G
                    pixels[base + 2] = 0; // B
                    // base+3 (alpha) stays 255
                }
            }
        }
    }

    Some((pixels, side))
}

// ---------------------------------------------------------------------------
// Error constructors
// ---------------------------------------------------------------------------

fn bitmap_err(msg: &str) -> QrGrantBitmap {
    QrGrantBitmap { ok: false, message: msg.into(), pixels: vec![], side: 0 }
}

fn auth_err(msg: &str) -> QrGrantAuth {
    QrGrantAuth { ok: false, message: msg.into(), verification_uri: String::new() }
}
