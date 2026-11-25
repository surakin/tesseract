//! A per-layer log filter that lets one event per interval through for a noisy target.

use std::{
    sync::Mutex,
    time::{Duration, Instant},
};

use tracing::{field::Visit, Event, Metadata, Subscriber};
use tracing_subscriber::{
    filter::FilterExt,
    layer::{Context, Filter},
    registry::LookupSpan,
    Registry,
};

/// The LiveKit data-track pipeline logs an error per undecryptable frame, which at the 20 Hz
/// world stream floods the log until a peer's key arrives.
const LIMITED_TARGET: &str = "livekit_datatrack::remote::pipeline";
const INTERVAL: Duration = Duration::from_secs(10);

/// Allows one event per `interval`.
pub struct RateLimit {
    interval: Duration,
    last: Mutex<Option<Instant>>,
}

impl RateLimit {
    pub fn new(interval: Duration) -> Self {
        Self {
            interval,
            last: Mutex::new(None),
        }
    }

    /// Whether an event at `now` passes; a passing event starts a new interval.
    pub fn allow(&self, now: Instant) -> bool {
        let mut last = self.last.lock().unwrap_or_else(|e| e.into_inner());
        match *last {
            Some(t) if now.saturating_duration_since(t) < self.interval => false,
            _ => {
                *last = Some(now);
                true
            }
        }
    }
}

/// Reads the original target of an event that came through the `log` crate.
struct LogTarget(Option<String>);

impl Visit for LogTarget {
    fn record_str(&mut self, field: &tracing::field::Field, value: &str) {
        if field.name() == "log.target" {
            self.0 = Some(value.to_owned());
        }
    }
    fn record_debug(&mut self, _: &tracing::field::Field, _: &dyn std::fmt::Debug) {}
}

fn is_limited(event: &Event<'_>) -> bool {
    let meta = event.metadata();
    if meta.target() == LIMITED_TARGET {
        return true;
    }
    // `log` records are re-emitted with target "log" and the real one in a field.
    if meta.target() == "log" {
        let mut v = LogTarget(None);
        event.record(&mut v);
        return v.0.as_deref() == Some(LIMITED_TARGET);
    }
    false
}

/// Per-layer filter: everything passes except the limited target, which is rate-limited
/// whatever the level filter string says.
pub struct NoisyTargetLimit(RateLimit);

impl NoisyTargetLimit {
    pub fn new() -> Self {
        Self(RateLimit::new(INTERVAL))
    }
}

impl<S: Subscriber + for<'a> LookupSpan<'a>> Filter<S> for NoisyTargetLimit {
    fn enabled(&self, _: &Metadata<'_>, _: &Context<'_, S>) -> bool {
        true
    }

    fn event_enabled(&self, event: &Event<'_>, _: &Context<'_, S>) -> bool {
        !is_limited(event) || self.0.allow(Instant::now())
    }
}

/// `env` with the noisy-target limit on top.
pub fn limited<F: Filter<Registry>>(env: F) -> impl Filter<Registry> {
    FilterExt::<Registry>::and(env, NoisyTargetLimit::new())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_event_per_interval() {
        let l = RateLimit::new(Duration::from_secs(10));
        let t0 = Instant::now();
        assert!(l.allow(t0));
        assert!(!l.allow(t0 + Duration::from_secs(1)));
        assert!(!l.allow(t0 + Duration::from_secs(9)));
        assert!(l.allow(t0 + Duration::from_secs(10)));
        assert!(!l.allow(t0 + Duration::from_secs(11)));
    }

    #[test]
    fn filter_limits_only_the_noisy_target() {
        use std::sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        };
        use tracing_subscriber::prelude::*;

        #[derive(Clone, Default)]
        struct Count(Arc<AtomicUsize>);
        impl<S: Subscriber> tracing_subscriber::Layer<S> for Count {
            fn on_event(&self, _: &Event<'_>, _: Context<'_, S>) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        let count = Count::default();
        let sub =
            tracing_subscriber::registry().with(count.clone().with_filter(NoisyTargetLimit::new()));
        tracing::subscriber::with_default(sub, || {
            for _ in 0..5 {
                tracing::error!(target: "livekit_datatrack::remote::pipeline", "bad frame");
                tracing::error!(target: "other", "kept");
            }
        });
        // One limited event plus all five others.
        assert_eq!(count.0.load(Ordering::SeqCst), 6);
    }
}
