//! Packages a finished export (the document plus any downloaded media) into
//! a single `.zip`, for the "zip output" option. Kept as thin, pure
//! file-to-archive plumbing — the caller decides *what* goes in, this only
//! handles *how* it's written, so it stays testable with plain temp files
//! and no export-specific state.

use std::fs::File;
use std::io::{self, BufReader, Read, Write};
use std::path::{Path, PathBuf};

use zip::write::SimpleFileOptions;
use zip::ZipWriter;

fn zip_err(e: zip::result::ZipError) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e.to_string())
}

/// One file to add to the archive: `source` is where it currently lives on
/// disk, `archive_path` is its path *inside* the zip (forward-slash
/// separated, per the zip spec — callers should not pass OS-native
/// separators on Windows).
pub(super) struct ZipEntry {
    pub source: PathBuf,
    pub archive_path: String,
}

/// Streams every entry's bytes into a new `.zip` at `out_path`, in the
/// order given. Entries are copied via a bounded buffer rather than reading
/// each source file fully into memory first — the document can be tens of
/// MB and media directories considerably more. `on_progress` is called with
/// the 1-based count of entries written so far after each one, so the
/// caller can report real assembly progress.
pub(super) fn write_zip(
    out_path: &Path,
    entries: &[ZipEntry],
    mut on_progress: impl FnMut(usize),
) -> io::Result<()> {
    if let Some(parent) = out_path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let file = File::create(out_path)?;
    let mut writer = ZipWriter::new(file);
    let options = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);

    let mut buf = [0u8; 64 * 1024];
    for (i, entry) in entries.iter().enumerate() {
        writer.start_file(&entry.archive_path, options).map_err(zip_err)?;
        let mut src = BufReader::new(File::open(&entry.source)?);
        loop {
            let n = src.read(&mut buf)?;
            if n == 0 {
                break;
            }
            writer.write_all(&buf[..n])?;
        }
        on_progress(i + 1);
    }
    writer.finish().map_err(zip_err)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read as _;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn unique_test_dir(name: &str) -> PathBuf {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir()
            .join(format!("tesseract_history_export_ziptest_{}_{}_{}", std::process::id(), name, n));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn writes_and_reads_back_entries() {
        let dir = unique_test_dir("basic");
        let doc = dir.join("history.html");
        std::fs::write(&doc, b"<html>hello</html>").unwrap();
        let img = dir.join("photo.jpg");
        std::fs::write(&img, b"not really a jpeg").unwrap();

        let out = dir.join("export.zip");
        write_zip(
            &out,
            &[
                ZipEntry { source: doc.clone(), archive_path: "history.html".to_string() },
                ZipEntry { source: img.clone(), archive_path: "media/photo.jpg".to_string() },
            ],
            |_| {},
        )
        .unwrap();

        let mut archive = zip::ZipArchive::new(File::open(&out).unwrap()).unwrap();
        assert_eq!(archive.len(), 2);

        let mut doc_out = String::new();
        archive.by_name("history.html").unwrap().read_to_string(&mut doc_out).unwrap();
        assert_eq!(doc_out, "<html>hello</html>");

        let mut img_out = Vec::new();
        archive.by_name("media/photo.jpg").unwrap().read_to_end(&mut img_out).unwrap();
        assert_eq!(img_out, b"not really a jpeg");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn empty_entry_list_produces_valid_empty_archive() {
        let dir = unique_test_dir("empty");
        let out = dir.join("empty.zip");
        write_zip(&out, &[], |_| {}).unwrap();
        let archive = zip::ZipArchive::new(File::open(&out).unwrap()).unwrap();
        assert_eq!(archive.len(), 0);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn reports_progress_per_entry() {
        let dir = unique_test_dir("progress");
        let mut sources = Vec::new();
        for i in 0..3 {
            let p = dir.join(format!("f{i}.bin"));
            std::fs::write(&p, [i as u8]).unwrap();
            sources.push(p);
        }
        let entries: Vec<ZipEntry> = sources
            .iter()
            .enumerate()
            .map(|(i, p)| ZipEntry { source: p.clone(), archive_path: format!("f{i}.bin") })
            .collect();

        let out = dir.join("progress.zip");
        let mut seen = Vec::new();
        write_zip(&out, &entries, |done| seen.push(done)).unwrap();
        assert_eq!(seen, vec![1, 2, 3]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_source_file_returns_error_not_panic() {
        let dir = unique_test_dir("missing");
        let out = dir.join("out.zip");
        let result = write_zip(
            &out,
            &[ZipEntry { source: dir.join("does_not_exist.bin"), archive_path: "x.bin".to_string() }],
            |_| {},
        );
        assert!(result.is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
