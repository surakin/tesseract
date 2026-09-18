//! Process-wide registry of background jobs, read by the Activity Monitor.
//!
//! Jobs register lazily on first `begin()`; a static keeps call sites free of
//! `ClientFfi` plumbing (media gate and RTC code have no handle to it).

use std::collections::HashMap;
use std::sync::OnceLock;
use std::time::{SystemTime, UNIX_EPOCH};

use parking_lot::Mutex;

#[cfg(not(test))]
use super::ClientFfi;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum JobKind {
    /// Runs for the whole session (watchers, supervisors).
    Loop,
    /// Wakes on an interval.
    Periodic,
    /// Runs on demand and finishes.
    OneShot,
}

impl JobKind {
    fn as_str(self) -> &'static str {
        match self {
            JobKind::Loop => "loop",
            JobKind::Periodic => "periodic",
            JobKind::OneShot => "one-shot",
        }
    }
}

#[derive(Default)]
struct Job {
    group: &'static str,
    kind: Option<JobKind>,
    active: u32,
    run_count: u64,
    last_started_ms: i64,
    last_finished_ms: i64,
    last_error: Option<String>,
    detail: String,
}

fn registry() -> &'static Mutex<HashMap<&'static str, Job>> {
    static REG: OnceLock<Mutex<HashMap<&'static str, Job>>> = OnceLock::new();
    REG.get_or_init(|| Mutex::new(HashMap::new()))
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// RAII marker: the job is Running while this is alive. Concurrent guards for
/// the same name stack (e.g. parallel media fetches).
pub(crate) struct JobGuard {
    name: &'static str,
}

impl Drop for JobGuard {
    fn drop(&mut self) {
        let mut reg = registry().lock();
        if let Some(job) = reg.get_mut(self.name) {
            job.active = job.active.saturating_sub(1);
            job.last_finished_ms = now_ms();
        }
    }
}

pub(crate) fn begin(name: &'static str, group: &'static str, kind: JobKind) -> JobGuard {
    let mut reg = registry().lock();
    let job = reg.entry(name).or_default();
    job.group = group;
    job.kind = Some(kind);
    job.active += 1;
    job.run_count += 1;
    job.last_started_ms = now_ms();
    job.last_error = None;
    drop(reg);
    JobGuard { name }
}

pub(crate) fn set_detail(name: &'static str, detail: impl Into<String>) {
    if let Some(job) = registry().lock().get_mut(name) {
        job.detail = detail.into();
    }
}

/// Set or clear the sticky error shown until the next `begin()` or clear.
pub(crate) fn set_error(name: &'static str, error: Option<String>) {
    if let Some(job) = registry().lock().get_mut(name) {
        job.last_error = error;
    }
}

/// Spawn a future that is Running for its whole lifetime.
pub(crate) fn track<F>(
    name: &'static str,
    group: &'static str,
    kind: JobKind,
    fut: F,
) -> impl std::future::Future<Output = F::Output>
where
    F: std::future::Future,
{
    async move {
        let _g = begin(name, group, kind);
        fut.await
    }
}

pub(crate) struct Snapshot {
    pub name: String,
    pub group: String,
    pub kind: String,
    /// 0 idle, 1 running, 2 error.
    pub state: u8,
    pub run_count: u64,
    pub last_started_ms: i64,
    pub last_finished_ms: i64,
    pub detail: String,
}

pub(crate) fn snapshot() -> Vec<Snapshot> {
    let reg = registry().lock();
    let mut out: Vec<Snapshot> = reg
        .iter()
        .map(|(name, j)| {
            let state = if j.active > 0 {
                1
            } else if j.last_error.is_some() {
                2
            } else {
                0
            };
            let detail = match (&j.last_error, j.detail.is_empty()) {
                (Some(e), _) => e.clone(),
                (None, false) => j.detail.clone(),
                (None, true) => String::new(),
            };
            Snapshot {
                name: (*name).to_string(),
                group: j.group.to_string(),
                kind: j.kind.map(JobKind::as_str).unwrap_or("").to_string(),
                state,
                run_count: j.run_count,
                last_started_ms: j.last_started_ms,
                last_finished_ms: j.last_finished_ms,
                detail,
            }
        })
        .collect();
    out.sort_by(|a, b| a.group.cmp(&b.group).then(a.name.cmp(&b.name)));
    out
}

#[cfg(not(test))]
impl ClientFfi {
    pub fn activity_snapshot(&self) -> Vec<crate::ffi::ActivityEntry> {
        snapshot()
            .into_iter()
            .map(|s| crate::ffi::ActivityEntry {
                name: s.name,
                group: s.group,
                kind: s.kind,
                state: s.state,
                run_count: s.run_count,
                last_started_ms: s.last_started_ms,
                last_finished_ms: s.last_finished_ms,
                detail: s.detail,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn find(name: &str) -> Snapshot {
        snapshot().into_iter().find(|s| s.name == name).unwrap()
    }

    #[test]
    fn guard_marks_running_then_idle() {
        let g = begin("t-running", "Test", JobKind::Periodic);
        assert_eq!(find("t-running").state, 1);
        drop(g);
        let s = find("t-running");
        assert_eq!(s.state, 0);
        assert_eq!(s.run_count, 1);
        assert_eq!(s.kind, "periodic");
    }

    #[test]
    fn concurrent_guards_stack() {
        let a = begin("t-stack", "Test", JobKind::OneShot);
        let b = begin("t-stack", "Test", JobKind::OneShot);
        drop(a);
        assert_eq!(find("t-stack").state, 1);
        drop(b);
        assert_eq!(find("t-stack").state, 0);
        assert_eq!(find("t-stack").run_count, 2);
    }

    #[test]
    fn error_sticks_until_next_begin_or_clear() {
        drop(begin("t-fail", "Test", JobKind::OneShot));
        set_error("t-fail", Some("boom".into()));
        let s = find("t-fail");
        assert_eq!(s.state, 2);
        assert_eq!(s.detail, "boom");
        drop(begin("t-fail", "Test", JobKind::OneShot));
        assert_eq!(find("t-fail").state, 0);
        set_error("t-fail", Some("again".into()));
        set_error("t-fail", None);
        assert_eq!(find("t-fail").state, 0);
    }

    #[test]
    fn detail_is_reported_when_idle() {
        let g = begin("t-detail", "Test", JobKind::Loop);
        set_detail("t-detail", "12 rooms");
        drop(g);
        assert_eq!(find("t-detail").detail, "12 rooms");
    }
}
