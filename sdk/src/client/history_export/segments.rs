//! Per-window segment files for the history-export walk, and their final
//! reverse-order concatenation into one document.
//!
//! The walk proceeds backward in time, window by window (newest window
//! first). Within a window, events are written oldest-first, so each
//! segment file already reads correctly top-to-bottom on its own — but
//! across windows the processing order is newest-to-oldest, the opposite of
//! how the final document should read. Concatenating segments in the
//! *reverse* of their processing order restores chronological order without
//! ever holding more than one window's events in memory at a time.

use std::fs::{self, File};
use std::io::{self, BufWriter, Read, Write};
use std::path::{Path, PathBuf};

fn segment_path(dir: &Path, index: u32) -> PathBuf {
    dir.join(format!("segment_{index:06}.part"))
}

/// Accumulates one export run's segment files under `dir` (a private
/// subdirectory the caller creates alongside the final output path).
pub(super) struct SegmentWriter {
    dir: PathBuf,
    /// Paths of already-finished segments, oldest-processed first (i.e. in
    /// the order `finish_segment` was called) — NOT final chronological
    /// order; see `concatenate_reverse`.
    finished: Vec<PathBuf>,
    current: Option<BufWriter<File>>,
    next_index: u32,
}

impl SegmentWriter {
    /// Starts a fresh segment sequence in a new directory.
    pub(super) fn create(dir: &Path) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        Ok(Self { dir: dir.to_path_buf(), finished: Vec::new(), current: None, next_index: 0 })
    }

    /// Resumes into an existing segment directory, preserving
    /// `existing_segment_count` previously finished segments (`segment_0`
    /// through `segment_{n-1}`) so a resumed export's final concatenation
    /// still includes them. Missing files are skipped with a warning rather
    /// than failing the resume outright — losing one window's worth of
    /// already-fetched history is recoverable (the window gets re-walked),
    /// refusing to resume at all is not friendlier to the user.
    pub(super) fn resume(dir: &Path, existing_segment_count: u32) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        let mut finished = Vec::new();
        for i in 0..existing_segment_count {
            let p = segment_path(dir, i);
            if p.is_file() {
                finished.push(p);
            } else {
                tracing::warn!("history export resume: missing segment {i} in {dir:?}, skipping");
            }
        }
        let next_index = existing_segment_count;
        Ok(Self { dir: dir.to_path_buf(), finished, current: None, next_index })
    }

    pub(super) fn begin_segment(&mut self) -> io::Result<()> {
        debug_assert!(self.current.is_none(), "finish the previous segment first");
        let path = segment_path(&self.dir, self.next_index);
        self.current = Some(BufWriter::new(File::create(path)?));
        Ok(())
    }

    pub(super) fn write(&mut self, s: &str) -> io::Result<()> {
        let Some(w) = self.current.as_mut() else {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "no segment open"));
        };
        w.write_all(s.as_bytes())
    }

    /// Flushes and closes the current segment, recording it as finished.
    /// Returns the number of finished segments so far (feeds the export
    /// checkpoint's `segments_written`).
    pub(super) fn finish_segment(&mut self) -> io::Result<u32> {
        let Some(mut w) = self.current.take() else {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "no segment open"));
        };
        w.flush()?;
        self.finished.push(segment_path(&self.dir, self.next_index));
        self.next_index += 1;
        Ok(self.next_index)
    }

    pub(super) fn segments_written(&self) -> u32 {
        self.next_index
    }

    /// Writes `header`, then every finished segment's bytes in *reverse*
    /// processing order, then `footer`, to `out_path`. `out_path` should be
    /// the caller's `.part` staging file — this function does not rename
    /// it into place, so a crash mid-concatenation never leaves a
    /// half-written file at the real output path. `on_progress` is called
    /// with the 1-based count of segments written so far after each one,
    /// so the caller can report real assembly progress instead of a single
    /// opaque "finalizing" tick.
    pub(super) fn concatenate_reverse(
        &self,
        out_path: &Path,
        header: &str,
        footer: &str,
        mut on_progress: impl FnMut(usize),
    ) -> io::Result<()> {
        let mut out = BufWriter::new(File::create(out_path)?);
        out.write_all(header.as_bytes())?;
        let mut buf = Vec::new();
        for (i, seg) in self.finished.iter().rev().enumerate() {
            buf.clear();
            File::open(seg)?.read_to_end(&mut buf)?;
            out.write_all(&buf)?;
            on_progress(i + 1);
        }
        out.write_all(footer.as_bytes())?;
        out.flush()
    }

    /// Removes the whole segment directory (temporary working files only —
    /// never the final assembled output, which lives elsewhere).
    pub(super) fn cleanup(&self) -> io::Result<()> {
        fs::remove_dir_all(&self.dir)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn unique_test_dir(name: &str) -> PathBuf {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir()
            .join(format!("tesseract_history_export_test_{}_{}_{}", std::process::id(), name, n));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn single_segment_round_trip() {
        let dir = unique_test_dir("single");
        let mut w = SegmentWriter::create(&dir).unwrap();
        w.begin_segment().unwrap();
        w.write("line one\n").unwrap();
        w.write("line two\n").unwrap();
        assert_eq!(w.finish_segment().unwrap(), 1);

        let out = dir.join("out.part");
        w.concatenate_reverse(&out, "HEADER\n", "FOOTER\n", |_| {}).unwrap();
        let content = fs::read_to_string(&out).unwrap();
        assert_eq!(content, "HEADER\nline one\nline two\nFOOTER\n");
        w.cleanup().unwrap();
    }

    #[test]
    fn multiple_segments_are_reversed() {
        // Simulates the real walk: window 1 (newest) processed first,
        // window 2 (older) processed second — the final document must read
        // window 2's content before window 1's.
        let dir = unique_test_dir("multi");
        let mut w = SegmentWriter::create(&dir).unwrap();

        w.begin_segment().unwrap();
        w.write("newest-window content\n").unwrap();
        w.finish_segment().unwrap();

        w.begin_segment().unwrap();
        w.write("oldest-window content\n").unwrap();
        w.finish_segment().unwrap();

        let out = dir.join("out.part");
        w.concatenate_reverse(&out, "", "", |_| {}).unwrap();
        let content = fs::read_to_string(&out).unwrap();
        assert_eq!(content, "oldest-window content\nnewest-window content\n");
        w.cleanup().unwrap();
    }

    #[test]
    fn resume_preserves_previously_finished_segments() {
        let dir = unique_test_dir("resume");
        {
            let mut w = SegmentWriter::create(&dir).unwrap();
            w.begin_segment().unwrap();
            w.write("first run, window A\n").unwrap();
            w.finish_segment().unwrap();
        }

        let mut resumed = SegmentWriter::resume(&dir, 1).unwrap();
        assert_eq!(resumed.segments_written(), 1);
        resumed.begin_segment().unwrap();
        resumed.write("second run, window B\n").unwrap();
        resumed.finish_segment().unwrap();

        let out = dir.join("out.part");
        resumed.concatenate_reverse(&out, "", "", |_| {}).unwrap();
        let content = fs::read_to_string(&out).unwrap();
        assert_eq!(content, "second run, window B\nfirst run, window A\n");
        resumed.cleanup().unwrap();
    }

    #[test]
    fn concatenate_reverse_reports_progress_per_segment() {
        let dir = unique_test_dir("progress");
        let mut w = SegmentWriter::create(&dir).unwrap();
        for i in 0..3 {
            w.begin_segment().unwrap();
            w.write(&format!("segment {i}\n")).unwrap();
            w.finish_segment().unwrap();
        }

        let out = dir.join("out.part");
        let mut seen = Vec::new();
        w.concatenate_reverse(&out, "", "", |done| seen.push(done)).unwrap();
        assert_eq!(seen, vec![1, 2, 3]);
        w.cleanup().unwrap();
    }

    #[test]
    fn resume_skips_missing_segment_files_without_failing() {
        let dir = unique_test_dir("resume_missing");
        fs::create_dir_all(&dir).unwrap();
        // Claim 2 prior segments exist, but write neither to disk.
        let resumed = SegmentWriter::resume(&dir, 2).unwrap();
        assert_eq!(resumed.finished.len(), 0);
        assert_eq!(resumed.segments_written(), 2);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn cleanup_removes_segment_directory() {
        let dir = unique_test_dir("cleanup");
        let mut w = SegmentWriter::create(&dir).unwrap();
        w.begin_segment().unwrap();
        w.write("x\n").unwrap();
        w.finish_segment().unwrap();
        assert!(dir.exists());
        w.cleanup().unwrap();
        assert!(!dir.exists());
    }
}
