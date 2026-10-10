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
///
/// All receive-side channels are wrapped in `Mutex<Option<…>>` so the five
/// "await / submit / cancel" bridge methods can take `&self` (→ SH_FFI on the
/// C++ side) instead of `&mut self` (→ MUT_FFI).  Holding SH_FFI while
/// blocking on a channel recv allows other SH_FFI callers (UI-thread reads) to
/// proceed concurrently — avoiding the deadlock where a long-running grant wait
/// starves `can_pin_in_room` and similar read calls.
pub(super) struct QrGrantHandle {
    /// Fires once the new device has scanned the QR code.
    scanned_rx: std::sync::Mutex<Option<oneshot::Receiver<()>>>,
    /// Sender for the check code entered by the user.
    check_code_tx: std::sync::Mutex<Option<oneshot::Sender<u8>>>,
    /// Fires once the flow reaches `WaitingForAuth` (carrying the URI).
    auth_rx: std::sync::Mutex<Option<oneshot::Receiver<String>>>,
    /// Fires once the flow is fully done (Ok) or has failed (Err).
    done_rx: std::sync::Mutex<Option<oneshot::Receiver<Result<(), String>>>>,
    /// Send `true` to abort the background task at any point.
    /// `watch::Sender` is `Sync`, so no Mutex needed.
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
            client, bitmap_tx, scanned_tx, check_rx, auth_tx, done_tx, cancel_rx,
        ));

        // Block until the QR bitmap is ready (or the flow fails immediately).
        match self.rt.block_on(bitmap_rx) {
            Ok((pixels, side)) => {
                self.qr_grant = Some(QrGrantHandle {
                    scanned_rx: std::sync::Mutex::new(Some(scanned_rx)),
                    check_code_tx: std::sync::Mutex::new(Some(check_tx)),
                    auth_rx: std::sync::Mutex::new(Some(auth_rx)),
                    done_rx: std::sync::Mutex::new(Some(done_rx)),
                    cancel_tx,
                });
                QrGrantBitmap {
                    ok: true,
                    message: String::new(),
                    pixels,
                    side,
                }
            }
            Err(_) => bitmap_err("QR grant flow failed before producing a bitmap"),
        }
    }

    // The five methods below take `&self` so the C++ side uses SH_FFI (shared
    // read lock) rather than MUT_FFI (exclusive write lock).  Multiple shared
    // lock holders can coexist, so a blocking recv here does not prevent the UI
    // thread from taking SH_FFI for unrelated read calls.

    pub fn qr_grant_await_scanned(&self) -> OpResult {
        let Some(h) = self.qr_grant.as_ref() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let rx = match h.scanned_rx.lock().unwrap().take() {
            Some(rx) => rx,
            None => return err("qr_grant_await_scanned already called"),
        };
        match self.rt.block_on(rx) {
            Ok(()) => ok(""),
            Err(_) => err("QR grant flow ended before the QR code was scanned"),
        }
    }

    pub fn qr_grant_submit_check_code(&self, code: u8) -> OpResult {
        let Some(h) = self.qr_grant.as_ref() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let tx = match h.check_code_tx.lock().unwrap().take() {
            Some(tx) => tx,
            None => return err("check code already submitted"),
        };
        match tx.send(code) {
            Ok(()) => ok(""),
            Err(_) => err("QR grant flow has already ended; check code could not be sent"),
        }
    }

    pub fn qr_grant_await_auth(&self) -> QrGrantAuth {
        let Some(h) = self.qr_grant.as_ref() else {
            return auth_err("no QR grant flow in progress; call qr_grant_start first");
        };
        let rx = match h.auth_rx.lock().unwrap().take() {
            Some(rx) => rx,
            None => return auth_err("qr_grant_await_auth already called"),
        };
        match self.rt.block_on(rx) {
            Ok(uri) => QrGrantAuth {
                ok: true,
                message: String::new(),
                verification_uri: uri,
            },
            Err(_) => auth_err("QR grant flow ended before reaching WaitingForAuth"),
        }
    }

    pub fn qr_grant_await_complete(&self) -> OpResult {
        let Some(h) = self.qr_grant.as_ref() else {
            return err("no QR grant flow in progress; call qr_grant_start first");
        };
        let rx = match h.done_rx.lock().unwrap().take() {
            Some(rx) => rx,
            None => return err("qr_grant_await_complete already called"),
        };
        match self.rt.block_on(rx) {
            Ok(Ok(())) => ok(""),
            Ok(Err(msg)) => err(msg),
            Err(_) => err("QR grant flow task panicked or was cancelled"),
        }
    }

    pub fn qr_grant_cancel(&self) {
        if let Some(h) = self.qr_grant.as_ref() {
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

                    Some(GrantLoginProgress::WaitingForAuth {
                        verification_uri,
                        continuation_sender,
                    }) => {
                        if let Some(tx) = auth_tx.take() {
                            let _ = tx.send(verification_uri.to_string());
                        }
                        // The SDK now blocks here until we confirm the app is
                        // ready to proceed (added so apps that suspend/navigate
                        // away while the browser is open can resume later).
                        // Tesseract has no such suspend-and-resume UI for this
                        // flow, so confirm immediately to match the previous
                        // (implicit) auto-continue behavior.
                        let _ = continuation_sender.confirm().await;
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
                    let base = (((row + quiet) * scale + dy) * side + (col + quiet) * scale + dx)
                        as usize
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
    QrGrantBitmap {
        ok: false,
        message: msg.into(),
        pixels: vec![],
        side: 0,
    }
}

fn auth_err(msg: &str) -> QrGrantAuth {
    QrGrantAuth {
        ok: false,
        message: msg.into(),
        verification_uri: String::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk_base::crypto::{
        types::qr_login::Msc4108IntentData, vodozemac::Curve25519PublicKey,
    };

    fn sample_qr_data() -> QrCodeData {
        QrCodeData::new_msc4108(
            Curve25519PublicKey::from_bytes([7u8; 32]),
            url::Url::parse("https://rendezvous.example.org/abc").unwrap(),
            Msc4108IntentData::Reciprocate {
                server_name: "example.org".to_owned(),
            },
        )
    }

    fn pixel(pixels: &[u8], side: u32, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * side + x) * 4) as usize;
        [pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]]
    }

    #[test]
    fn render_qr_dimensions_follow_module_count_scale_and_quiet_zone() {
        let data = sample_qr_data();
        let (pixels, side) = render_qr(&data).expect("renders");
        let modules = qrcode::QrCode::new(data.to_bytes()).unwrap().width() as u32;
        assert_eq!(side, (modules + 2 * 4) * 4);
        assert_eq!(pixels.len(), (side * side * 4) as usize);
    }

    #[test]
    fn render_qr_is_fully_opaque_black_on_white() {
        let (pixels, _) = render_qr(&sample_qr_data()).unwrap();
        assert!(pixels.chunks(4).all(|p| p[3] == 255));
        assert!(pixels
            .chunks(4)
            .all(|p| p[..3] == [0, 0, 0] || p[..3] == [255, 255, 255]));
        assert!(pixels.chunks(4).any(|p| p[..3] == [0, 0, 0]));
        assert!(pixels.chunks(4).any(|p| p[..3] == [255, 255, 255]));
    }

    #[test]
    fn render_qr_quiet_zone_is_white() {
        let (pixels, side) = render_qr(&sample_qr_data()).unwrap();
        let border = 4 * 4; // quiet modules * scale
        for i in 0..side {
            for edge in 0..border {
                assert_eq!(pixel(&pixels, side, i, edge), [255; 4]);
                assert_eq!(pixel(&pixels, side, i, side - 1 - edge), [255; 4]);
                assert_eq!(pixel(&pixels, side, edge, i), [255; 4]);
                assert_eq!(pixel(&pixels, side, side - 1 - edge, i), [255; 4]);
            }
        }
    }

    #[test]
    fn render_qr_pixels_match_the_qr_modules() {
        let data = sample_qr_data();
        let (pixels, side) = render_qr(&data).unwrap();
        let code = qrcode::QrCode::new(data.to_bytes()).unwrap();
        let modules = code.width() as u32;
        let colors = code.to_colors();
        for row in 0..modules {
            for col in 0..modules {
                let dark = matches!(colors[(row * modules + col) as usize], qrcode::Color::Dark);
                let want = if dark { [0, 0, 0, 255] } else { [255; 4] };
                // Every pixel of the 4x4 block must carry the module colour.
                for dy in 0..4 {
                    for dx in 0..4 {
                        let x = (col + 4) * 4 + dx;
                        let y = (row + 4) * 4 + dy;
                        assert_eq!(pixel(&pixels, side, x, y), want, "module ({row},{col})");
                    }
                }
            }
        }
        // The top-left finder pattern always starts with a dark module.
        assert_eq!(pixel(&pixels, side, 16, 16), [0, 0, 0, 255]);
    }

    #[test]
    fn render_qr_differs_for_different_payloads() {
        let other = QrCodeData::new_msc4108(
            Curve25519PublicKey::from_bytes([9u8; 32]),
            url::Url::parse("https://rendezvous.example.org/abc").unwrap(),
            Msc4108IntentData::Login,
        );
        let a = render_qr(&sample_qr_data()).unwrap();
        let b = render_qr(&other).unwrap();
        assert_ne!(a.0, b.0);
    }

    #[test]
    fn error_constructors_carry_message_and_empty_payload() {
        let b = bitmap_err("nope");
        assert!(!b.ok);
        assert_eq!(b.message, "nope");
        assert!(b.pixels.is_empty());
        assert_eq!(b.side, 0);
        let a = auth_err("nope");
        assert!(!a.ok);
        assert_eq!(a.message, "nope");
        assert!(a.verification_uri.is_empty());
    }

    #[test]
    fn methods_report_no_flow_before_start() {
        let c = ClientFfi::new();
        for r in [
            c.qr_grant_await_scanned(),
            c.qr_grant_submit_check_code(1),
            c.qr_grant_await_complete(),
        ] {
            assert!(!r.ok);
            assert!(r.message.contains("qr_grant_start"), "{}", r.message);
        }
        let a = c.qr_grant_await_auth();
        assert!(!a.ok && a.verification_uri.is_empty());
        assert!(a.message.contains("qr_grant_start"), "{}", a.message);
        c.qr_grant_cancel(); // no-op, must not panic
    }

    #[test]
    fn start_requires_a_logged_in_client() {
        let mut c = ClientFfi::new();
        let b = c.qr_grant_start();
        assert!(!b.ok);
        assert_eq!(b.message, "not logged in");
        assert!(b.pixels.is_empty());
        assert!(c.qr_grant.is_none());
    }

    struct Ends {
        scanned_tx: oneshot::Sender<()>,
        check_rx: oneshot::Receiver<u8>,
        auth_tx: oneshot::Sender<String>,
        done_tx: oneshot::Sender<Result<(), String>>,
        cancel_rx: watch::Receiver<bool>,
    }

    fn install_handle(c: &mut ClientFfi) -> Ends {
        let (scanned_tx, scanned_rx) = oneshot::channel();
        let (check_tx, check_rx) = oneshot::channel();
        let (auth_tx, auth_rx) = oneshot::channel();
        let (done_tx, done_rx) = oneshot::channel();
        let (cancel_tx, cancel_rx) = watch::channel(false);
        c.qr_grant = Some(QrGrantHandle {
            scanned_rx: std::sync::Mutex::new(Some(scanned_rx)),
            check_code_tx: std::sync::Mutex::new(Some(check_tx)),
            auth_rx: std::sync::Mutex::new(Some(auth_rx)),
            done_rx: std::sync::Mutex::new(Some(done_rx)),
            cancel_tx,
        });
        Ends { scanned_tx, check_rx, auth_tx, done_tx, cancel_rx }
    }

    #[test]
    fn await_scanned_resolves_once_and_rejects_reuse() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        ends.scanned_tx.send(()).unwrap();
        assert!(c.qr_grant_await_scanned().ok);
        let again = c.qr_grant_await_scanned();
        assert!(!again.ok);
        assert!(again.message.contains("already called"), "{}", again.message);
    }

    #[test]
    fn await_scanned_reports_flow_ending_early() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        drop(ends.scanned_tx);
        let r = c.qr_grant_await_scanned();
        assert!(!r.ok);
        assert!(r.message.contains("before the QR code was scanned"), "{}", r.message);
    }

    #[test]
    fn submit_check_code_delivers_value_once() {
        let mut c = ClientFfi::new();
        let mut ends = install_handle(&mut c);
        assert!(c.qr_grant_submit_check_code(42).ok);
        assert_eq!(ends.check_rx.try_recv().unwrap(), 42);
        let again = c.qr_grant_submit_check_code(43);
        assert!(!again.ok);
        assert!(again.message.contains("already submitted"), "{}", again.message);
    }

    #[test]
    fn submit_check_code_reports_ended_flow() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        drop(ends.check_rx);
        let r = c.qr_grant_submit_check_code(1);
        assert!(!r.ok);
        assert!(r.message.contains("already ended"), "{}", r.message);
    }

    #[test]
    fn await_auth_returns_the_verification_uri() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        ends.auth_tx.send("https://auth.example.org/device".to_owned()).unwrap();
        let a = c.qr_grant_await_auth();
        assert!(a.ok);
        assert!(a.message.is_empty());
        assert_eq!(a.verification_uri, "https://auth.example.org/device");
        let again = c.qr_grant_await_auth();
        assert!(!again.ok && again.message.contains("already called"));
    }

    #[test]
    fn await_auth_reports_flow_ending_before_auth() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        drop(ends.auth_tx);
        let a = c.qr_grant_await_auth();
        assert!(!a.ok);
        assert!(a.verification_uri.is_empty());
        assert!(a.message.contains("WaitingForAuth"), "{}", a.message);
    }

    #[test]
    fn await_complete_maps_success_failure_and_dropped_task() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        ends.done_tx.send(Ok(())).unwrap();
        assert!(c.qr_grant_await_complete().ok);
        assert!(c.qr_grant_await_complete().message.contains("already called"));

        let ends = install_handle(&mut c);
        ends.done_tx.send(Err("homeserver said no".to_owned())).unwrap();
        let r = c.qr_grant_await_complete();
        assert!(!r.ok);
        assert_eq!(r.message, "homeserver said no");

        let ends = install_handle(&mut c);
        drop(ends.done_tx);
        let r = c.qr_grant_await_complete();
        assert!(!r.ok);
        assert!(r.message.contains("panicked or was cancelled"), "{}", r.message);
    }

    #[test]
    fn cancel_signals_the_background_task() {
        let mut c = ClientFfi::new();
        let ends = install_handle(&mut c);
        assert!(!*ends.cancel_rx.borrow());
        c.qr_grant_cancel();
        assert!(*ends.cancel_rx.borrow());
    }
}
