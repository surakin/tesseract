//! Request timing and frequency log (Cargo feature `request_log`).
//!
//! matrix-sdk wraps every homeserver request in an `info` span named `send`
//! (target `matrix_sdk::http_client`) carrying the method and the path with
//! the query erased. [`RequestLogLayer`] times those spans and, when enabled
//! with `TESSERACT_REQUEST_LOG`, appends one tab-separated line per request
//! to a file plus a summary every [`SUMMARY_EVERY`]: requests per second,
//! in-flight now and at peak, and the endpoints that took the most total
//! time. Paths are normalised (room/event/user ids, media ids and numbers
//! collapse to placeholders) so the same endpoint aggregates.
//!
//! `TESSERACT_REQUEST_LOG=1` writes `request_timings.log` next to the
//! per-account stores; any other value is taken as the file path.

use std::collections::HashMap;
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use parking_lot::Mutex;
use tracing::field::{Field, Visit};
use tracing::span::{Attributes, Id, Record};
use tracing::Subscriber;
use tracing_subscriber::layer::Context;
use tracing_subscriber::registry::LookupSpan;
use tracing_subscriber::Layer;

const SUMMARY_EVERY: Duration = Duration::from_secs(10);
const SPAN_NAME: &str = "send";
/// Where the SDK's request spans come from.
pub(crate) const SPAN_TARGET: &str = "matrix_sdk::http_client";

#[derive(Default, Clone)]
struct EndpointStats {
    count: u64,
    total_ms: f64,
    max_ms: f64,
}

struct Shared {
    start: Instant,
    out: Mutex<std::fs::File>,
    stats: Mutex<HashMap<String, EndpointStats>>,
    in_flight: AtomicU32,
    peak_in_flight: AtomicU32,
    total: std::sync::atomic::AtomicU64,
}

static SHARED: OnceLock<Arc<Shared>> = OnceLock::new();

/// The log file requested through the environment, if any.
fn requested_path(default_dir: &std::path::Path) -> Option<PathBuf> {
    let v = std::env::var("TESSERACT_REQUEST_LOG").ok()?;
    let v = v.trim();
    if v.is_empty() || v == "0" {
        return None;
    }
    if v == "1" || v.eq_ignore_ascii_case("true") {
        return Some(default_dir.join("request_timings.log"));
    }
    Some(PathBuf::from(v))
}

/// The layer to install, or `None` when request logging is off.
pub(crate) fn init(default_dir: &std::path::Path) -> Option<RequestLogLayer> {
    let path = requested_path(default_dir)?;
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .ok()?;
    let _ = writeln!(
        file,
        "# request log started; columns: t_ms method endpoint status dur_ms in_flight req_bytes resp_bytes"
    );
    let shared = Arc::new(Shared {
        start: Instant::now(),
        out: Mutex::new(file),
        stats: Mutex::new(HashMap::new()),
        in_flight: AtomicU32::new(0),
        peak_in_flight: AtomicU32::new(0),
        total: std::sync::atomic::AtomicU64::new(0),
    });
    let _ = SHARED.set(Arc::clone(&shared));
    let summary = Arc::clone(&shared);
    std::thread::Builder::new()
        .name("request-log-summary".into())
        .spawn(move || {
            let mut last_total = 0u64;
            loop {
                std::thread::sleep(SUMMARY_EVERY);
                last_total = write_summary(&summary, last_total);
            }
        })
        .ok()?;
    Some(RequestLogLayer { shared })
}

fn write_summary(s: &Shared, last_total: u64) -> u64 {
    let total = s.total.load(Ordering::Relaxed);
    let window = total - last_total;
    let stats = s.stats.lock().clone();
    let mut rows: Vec<(&String, &EndpointStats)> = stats.iter().collect();
    rows.sort_by(|a, b| b.1.total_ms.total_cmp(&a.1.total_ms));
    let mut out = s.out.lock();
    let _ = writeln!(
        out,
        "# summary t={}s total={} last_{}s={} ({:.1}/s) in_flight={} peak={}",
        s.start.elapsed().as_secs(),
        total,
        SUMMARY_EVERY.as_secs(),
        window,
        window as f64 / SUMMARY_EVERY.as_secs_f64(),
        s.in_flight.load(Ordering::Relaxed),
        s.peak_in_flight.load(Ordering::Relaxed),
    );
    for (ep, st) in rows.into_iter().take(15) {
        let _ = writeln!(
            out,
            "#   {:>6}x {:>9.0}ms total {:>7.0}ms avg {:>7.0}ms max  {ep}",
            st.count,
            st.total_ms,
            st.total_ms / st.count.max(1) as f64,
            st.max_ms
        );
    }
    let _ = out.flush();
    total
}

/// Appends a `# note` line to the request log (no-op when logging is off), so
/// background jobs can leave their own counts next to the request timings.
pub(crate) fn note(msg: impl AsRef<str>) {
    let Some(s) = SHARED.get() else { return };
    let mut out = s.out.lock();
    let _ = writeln!(
        out,
        "# note t={}ms {}",
        s.start.elapsed().as_millis(),
        msg.as_ref()
    );
}

/// Collapses ids so the same endpoint aggregates: room, event, user and alias
/// ids (plain or percent-encoded), numbers, media ids and long opaque tokens
/// become placeholders.
pub(crate) fn normalize_endpoint(path: &str) -> String {
    let path = path.split('?').next().unwrap_or(path);
    let segs: Vec<&str> = path.split('/').collect();
    let mut out: Vec<String> = Vec::with_capacity(segs.len());
    let mut after_media = 0usize; // segments left to replace after a media marker
    for (i, seg) in segs.iter().enumerate() {
        if seg.is_empty() {
            if i != 0 {
                continue;
            }
            out.push(String::new());
            continue;
        }
        if after_media > 0 {
            after_media -= 1;
            out.push(if after_media == 1 { "{server}" } else { "{media}" }.into());
            continue;
        }
        if matches!(*seg, "download" | "thumbnail") && out.iter().any(|s| s == "media") {
            out.push((*seg).to_owned());
            after_media = 2;
            continue;
        }
        let lower = seg.to_ascii_lowercase();
        let id_like = seg.starts_with(['!', '$', '@', '#'])
            || lower.starts_with("%21")
            || lower.starts_with("%24")
            || lower.starts_with("%40")
            || lower.starts_with("%23");
        let numeric = seg.chars().all(|c| c.is_ascii_digit());
        let opaque = seg.len() >= 20
            && seg
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '='));
        out.push(if id_like {
            "{id}".into()
        } else if numeric {
            "{n}".into()
        } else if opaque {
            "{token}".into()
        } else {
            (*seg).to_owned()
        });
    }
    out.join("/")
}

#[derive(Default)]
struct SpanData {
    start: Option<Instant>,
    method: String,
    uri: String,
    status: String,
    req_bytes: u64,
    resp_bytes: u64,
}

struct FieldGrabber<'a>(&'a mut SpanData);

impl Visit for FieldGrabber<'_> {
    fn record_str(&mut self, f: &Field, v: &str) {
        match f.name() {
            "uri" => self.0.uri = v.to_owned(),
            "method" => self.0.method = v.to_owned(),
            "status" => self.0.status = v.to_owned(),
            _ => {}
        }
    }
    fn record_u64(&mut self, f: &Field, v: u64) {
        match f.name() {
            "request_size" => self.0.req_bytes = v,
            "response_size" => self.0.resp_bytes = v,
            "status" => self.0.status = v.to_string(),
            _ => {}
        }
    }
    fn record_debug(&mut self, f: &Field, v: &dyn std::fmt::Debug) {
        let s = format!("{v:?}");
        match f.name() {
            "method" => self.0.method = s,
            "status" => self.0.status = s,
            "request_size" => self.0.req_bytes = s.parse().unwrap_or(0),
            "response_size" => self.0.resp_bytes = s.parse().unwrap_or(0),
            _ => {}
        }
    }
}

pub(crate) struct RequestLogLayer {
    shared: Arc<Shared>,
}

impl<S> Layer<S> for RequestLogLayer
where
    S: Subscriber + for<'a> LookupSpan<'a>,
{
    fn on_new_span(&self, attrs: &Attributes<'_>, id: &Id, ctx: Context<'_, S>) {
        if attrs.metadata().name() != SPAN_NAME {
            return;
        }
        let Some(span) = ctx.span(id) else { return };
        let mut data = SpanData {
            start: Some(Instant::now()),
            ..SpanData::default()
        };
        attrs.record(&mut FieldGrabber(&mut data));
        span.extensions_mut().insert(data);
        let now = self.shared.in_flight.fetch_add(1, Ordering::Relaxed) + 1;
        self.shared.peak_in_flight.fetch_max(now, Ordering::Relaxed);
    }

    fn on_record(&self, id: &Id, values: &Record<'_>, ctx: Context<'_, S>) {
        let Some(span) = ctx.span(id) else { return };
        let mut ext = span.extensions_mut();
        if let Some(data) = ext.get_mut::<SpanData>() {
            values.record(&mut FieldGrabber(data));
        }
    }

    fn on_close(&self, id: Id, ctx: Context<'_, S>) {
        let Some(span) = ctx.span(&id) else { return };
        let Some(data) = span.extensions_mut().remove::<SpanData>() else {
            return;
        };
        self.shared.in_flight.fetch_sub(1, Ordering::Relaxed);
        let dur_ms = data.start.map(|t| t.elapsed().as_secs_f64() * 1000.0).unwrap_or(0.0);
        let endpoint = normalize_endpoint(
            data.uri
                .split_once("://")
                .and_then(|(_, rest)| rest.find('/').map(|i| &rest[i..]))
                .unwrap_or(&data.uri),
        );
        self.shared.total.fetch_add(1, Ordering::Relaxed);
        {
            let mut stats = self.shared.stats.lock();
            let e = stats.entry(format!("{} {endpoint}", data.method)).or_default();
            e.count += 1;
            e.total_ms += dur_ms;
            e.max_ms = e.max_ms.max(dur_ms);
        }
        let t_ms = self.shared.start.elapsed().as_millis();
        let mut out = self.shared.out.lock();
        let _ = writeln!(
            out,
            "{t_ms}\t{}\t{endpoint}\t{}\t{dur_ms:.0}\t{}\t{}\t{}",
            data.method,
            if data.status.is_empty() { "-" } else { &data.status },
            self.shared.in_flight.load(Ordering::Relaxed),
            data.req_bytes,
            data.resp_bytes,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::normalize_endpoint;

    #[test]
    fn ids_and_numbers_collapse() {
        assert_eq!(
            normalize_endpoint("/_matrix/client/v3/rooms/%21abc%3Aserver.org/messages"),
            "/_matrix/client/v3/rooms/{id}/messages"
        );
        assert_eq!(
            normalize_endpoint("/_matrix/client/v3/rooms/!abc:s/send/m.room.message/17"),
            "/_matrix/client/v3/rooms/{id}/send/m.room.message/{n}"
        );
        assert_eq!(
            normalize_endpoint("/_matrix/client/v3/presence/@u:s/status"),
            "/_matrix/client/v3/presence/{id}/status"
        );
    }

    #[test]
    fn media_paths_hide_server_and_media_id() {
        assert_eq!(
            normalize_endpoint("/_matrix/client/v1/media/download/example.org/AbCdEfGh"),
            "/_matrix/client/v1/media/download/{server}/{media}"
        );
        assert_eq!(
            normalize_endpoint("/_matrix/client/v1/media/thumbnail/example.org/AbCdEfGh?width=1"),
            "/_matrix/client/v1/media/thumbnail/{server}/{media}"
        );
    }

    #[test]
    fn plain_endpoints_are_unchanged() {
        assert_eq!(
            normalize_endpoint("/_matrix/client/unstable/org.matrix.simplified_msc3575/sync"),
            "/_matrix/client/unstable/org.matrix.simplified_msc3575/sync"
        );
    }

    #[test]
    fn long_opaque_tokens_collapse() {
        assert_eq!(
            normalize_endpoint("/_matrix/client/v3/sendToDevice/m.x/AAAAAAAAAAAAAAAAAAAAAA"),
            "/_matrix/client/v3/sendToDevice/m.x/{token}"
        );
    }

    #[test]
    fn a_sdk_send_span_becomes_a_log_line() {
        use tracing_subscriber::filter::{LevelFilter, Targets};
        use tracing_subscriber::prelude::*;

        let path = std::env::temp_dir().join(format!("tesseract-reqlog-{}.log", std::process::id()));
        let _ = std::fs::remove_file(&path);
        std::env::set_var("TESSERACT_REQUEST_LOG", &path);
        let layer = super::init(std::path::Path::new("/nonexistent")).expect("layer is installed");
        let subscriber = tracing_subscriber::registry()
            .with(layer.with_filter(Targets::new().with_target(super::SPAN_TARGET, LevelFilter::INFO)));
        tracing::subscriber::with_default(subscriber, || {
            let span = tracing::info_span!(
                target: "matrix_sdk::http_client",
                "send",
                uri = tracing::field::Empty,
                method = tracing::field::Empty,
                status = tracing::field::Empty,
            );
            span.record("method", tracing::field::debug(&"GET"))
                .record("uri", "https://hs.example/_matrix/client/v3/rooms/%21r%3Ahs.example/messages")
                .record("status", 200u64);
            drop(span);
        });
        std::env::remove_var("TESSERACT_REQUEST_LOG");
        let log = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let line = log.lines().find(|l| !l.starts_with('#')).expect("a request line");
        let cols: Vec<&str> = line.split('\t').collect();
        assert_eq!(cols[2], "/_matrix/client/v3/rooms/{id}/messages");
        assert_eq!(cols[3], "200");
    }
}
