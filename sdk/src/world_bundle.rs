//! World source bundles: the zip a user imports into a room to publish a 3D world
//! (glTF models, textures, Lua scripts and a `world.json` manifest at the root).
//!
//! The zip comes from other users, so this module is the hostile-input boundary:
//! every entry name and header is validated before anything is written, bytes are
//! counted while copying (declared sizes are never trusted), the archive is
//! extracted into a temporary sibling directory, and that directory is renamed
//! into place only after the manifest has passed too. On any error nothing is left
//! behind.
//!
//! Rejected: absolute, `..`, `.`, empty, backslash, drive (`:`), control-character
//! and Windows-invalid names, symlinks and other non-regular entries, encrypted
//! entries, names that collide case-insensitively (or file vs directory), too
//! deep paths, and archives over the [`Limits`].
//!
//! Default [`Limits`]: 64 MiB total, 2000 entries, 32 MiB per file, depth 16.
//!
//! [`pack_dir`] is deterministic: entries sorted by name, fixed timestamp, fixed
//! deflate level, fixed permissions, no extra fields. The same directory contents
//! always give the same bytes (and so the same [`sha256_hex`]).

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, DateTime, ZipArchive, ZipWriter};

/// The manifest format this build understands.
pub const FORMAT: u32 = 1;
/// The manifest's name inside the bundle (always at the root).
pub const MANIFEST_NAME: &str = "world.json";
/// The manifest is small; this bounds what is parsed (declared and counted).
const MAX_MANIFEST: u64 = 256 * 1024;

#[derive(Clone, Debug)]
pub struct Limits {
    pub max_total: u64,
    pub max_files: u32,
    pub max_file: u64,
    pub max_path_depth: u32,
    /// Longest entry path in bytes (the C++ side refuses longer ones).
    pub max_path_bytes: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_total: 64 * 1024 * 1024,
            max_files: 2000,
            max_file: 32 * 1024 * 1024,
            max_path_depth: 16,
            max_path_bytes: 255,
        }
    }
}

/// Mirrors `EngineScene::ScriptSpec`: `attach` is its `behavior`.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ManifestScript {
    pub file: String,
    pub attach: String,
    #[serde(default)]
    pub on_camera: bool,
    /// Numbers or strings only.
    #[serde(default)]
    pub params: serde_json::Map<String, serde_json::Value>,
}

/// Mirrors the world-describing part of `EngineScene::Options`.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub format: u32,
    pub name: String,
    pub field_asset: String,
    pub user_asset: String,
    #[serde(default)]
    pub scripts: Vec<ManifestScript>,
    pub user_script: ManifestScript,
    #[serde(default)]
    pub events_out: Vec<String>,
}

/// The files the manifest needs, relative to the bundle root, without duplicates:
/// `world.json`, the field and robot assets, every script file.
pub fn referenced_files(m: &Manifest) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut add = |f: &str| {
        if !out.iter().any(|o| o == f) {
            out.push(f.to_string());
        }
    };
    add(MANIFEST_NAME);
    add(&m.field_asset);
    add(&m.user_asset);
    for s in &m.scripts {
        add(&s.file);
    }
    add(&m.user_script.file);
    out
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    crate::client::hash::sha256(bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// Characters that hide or fake parts of a name or text: the bidi controls plus
/// zero-width and blank "filler" characters. Stricter than the media filename
/// sanitiser, which must keep joiners that real names use.
fn is_hidden_char(c: char) -> bool {
    crate::media_type_check::is_bidi_control(c)
        || matches!(
            c,
            '\u{200B}'..='\u{200D}'
                | '\u{2060}'..='\u{2064}'
                | '\u{FEFF}'
                | '\u{180E}'
                | '\u{00AD}'
                | '\u{FFA0}'
                | '\u{3164}'
                | '\u{115F}'
                | '\u{1160}'
                | '\u{2800}'
        )
}

/// Bundle-controlled text made safe to show: control, bidi and zero-width
/// characters dropped, at most `max` characters (an ellipsis marks a cut).
pub(crate) fn clean_text(s: &str, max: usize) -> String {
    let mut out: String = s
        .chars()
        .filter(|c| !c.is_control() && !is_hidden_char(*c))
        .take(max + 1)
        .collect();
    if out.chars().count() > max {
        out = out.chars().take(max).collect();
        out.push('\u{2026}');
    }
    out
}

/// A bundle-controlled name quoted for an error message.
fn quote(s: &str) -> String {
    format!("\"{}\"", clean_text(s, 64))
}

/// Checks one entry name (without a trailing `/`) and returns its components.
fn check_path<'a>(name: &'a str, limits: &Limits) -> Result<Vec<&'a str>, String> {
    let bad = |why: &str| Err(format!("unsafe path {}: {why}", quote(name)));
    if name.is_empty() {
        return bad("empty");
    }
    if name.len() > limits.max_path_bytes {
        return bad("too long");
    }
    if name.starts_with('/') {
        return bad("absolute");
    }
    let parts: Vec<&str> = name.split('/').collect();
    if parts.len() as u32 > limits.max_path_depth {
        return bad("too deep");
    }
    for p in &parts {
        match *p {
            "" => return bad("empty component"),
            "." | ".." => return bad("relative component"),
            _ => {}
        }
        if crate::media_type_check::is_reserved_device_name(p) {
            return bad("reserved device name");
        }
        if p.ends_with('.') || p.ends_with(' ') {
            return bad("trailing dot or space");
        }
        if let Some(c) = p.chars().find(|c| {
            c.is_control()
                || is_hidden_char(*c)
                || matches!(c, '\\' | ':' | '<' | '>' | '"' | '|' | '?' | '*')
        }) {
            return bad(&format!("character {c:?}"));
        }
    }
    Ok(parts)
}

/// Pass 1: validates every entry's name and header, with no data read.
/// Returns the exact names of the files (not directories).
fn validate_entries<R: Read + std::io::Seek>(
    archive: &mut ZipArchive<R>,
    limits: &Limits,
) -> Result<HashSet<String>, String> {
    if archive.has_duplicate_names() {
        return Err("duplicate entry names".to_string());
    }
    archive
        .validate_header_consistency()
        .map_err(|e| format!("inconsistent archive: {e}"))?;
    if archive.len() as u64 > limits.max_files as u64 {
        return Err(format!(
            "too many entries ({} > {})",
            archive.len(),
            limits.max_files
        ));
    }
    let mut files: HashSet<String> = HashSet::new();
    let mut dirs: HashSet<String> = HashSet::new();
    // lowercase prefix -> spelling first seen, so "A/x" and "a/y" collide.
    let mut spelling: HashMap<String, String> = HashMap::new();
    let mut total: u64 = 0;
    let mut names: HashSet<String> = HashSet::new();
    for i in 0..archive.len() {
        let f = archive.by_index_raw(i).map_err(|e| e.to_string())?;
        let raw = f.name().map_err(|e| e.to_string())?.into_owned();
        if f.encrypted() {
            return Err(format!("encrypted entry {raw:?}"));
        }
        let is_dir = raw.ends_with('/');
        if is_dir != f.is_dir() {
            return Err(format!("inconsistent entry {raw:?}"));
        }
        if let Some(mode) = f.unix_mode() {
            let kind = mode & 0o170000;
            if kind != 0 && kind != (if is_dir { 0o040000 } else { 0o100000 }) {
                return Err(format!("entry {raw:?} is a link or special file"));
            }
        }
        let path = raw.strip_suffix('/').unwrap_or(&raw);
        let parts = check_path(path, limits)?;
        if !is_dir {
            if raw == MANIFEST_NAME && f.size() > MAX_MANIFEST {
                return Err(format!("{MANIFEST_NAME} is too large"));
            }
            if f.size() > limits.max_file {
                return Err(format!("{raw:?} is too large ({} bytes)", f.size()));
            }
            total = total.saturating_add(f.size());
            if total > limits.max_total {
                return Err("bundle is too large".to_string());
            }
        }
        // Every prefix: directories; the full path: a file unless is_dir.
        let mut prefix = String::new();
        for (n, p) in parts.iter().enumerate() {
            if n > 0 {
                prefix.push('/');
            }
            prefix.push_str(p);
            let key = prefix.to_lowercase();
            match spelling.get(&key) {
                Some(s) if *s != prefix => {
                    return Err(format!("names collide: {s:?} and {prefix:?}"))
                }
                None => {
                    spelling.insert(key.clone(), prefix.clone());
                }
                _ => {}
            }
            let last = n + 1 == parts.len();
            if last && !is_dir {
                if files.contains(&key) || dirs.contains(&key) {
                    return Err(format!("duplicate or colliding name {raw:?}"));
                }
                files.insert(key);
            } else {
                if files.contains(&key) {
                    return Err(format!("{prefix:?} is both a file and a directory"));
                }
                dirs.insert(key);
            }
        }
        if !is_dir {
            names.insert(raw);
        }
    }
    if !names.contains(MANIFEST_NAME) {
        return Err(format!("{MANIFEST_NAME} is missing from the bundle root"));
    }
    Ok(names)
}

fn parse_manifest(bytes: &[u8]) -> Result<Manifest, String> {
    // Read the format first so a newer bundle gets a useful message rather than a
    // complaint about fields this build does not know.
    let v: serde_json::Value =
        serde_json::from_slice(bytes).map_err(|e| format!("{MANIFEST_NAME}: {e}"))?;
    let format = v.get("format").and_then(|f| f.as_u64()).unwrap_or(0);
    if format > FORMAT as u64 {
        return Err(format!(
            "this world was made with a newer format ({format}); update Tesseract to use it"
        ));
    }
    if format == 0 {
        return Err(format!("{MANIFEST_NAME}: missing or invalid format"));
    }
    serde_json::from_value(v).map_err(|e| format!("{MANIFEST_NAME}: {e}"))
}

/// Checks the manifest's own values and that every file it names is one of the
/// bundle's entries, by exact name (so the verdict does not depend on the OS).
fn validate_manifest(m: &Manifest, names: &HashSet<String>, limits: &Limits) -> Result<(), String> {
    if m.name.trim().is_empty() {
        return Err("manifest: empty name".to_string());
    }
    for s in m.scripts.iter().chain(std::iter::once(&m.user_script)) {
        // The engine keeps 63 bytes of a behaviour name and cuts it at a NUL, so a longer or
        // NUL-bearing name would attach a different behaviour than the one the manifest shows.
        if s.attach.is_empty() || s.attach.len() > 63 || s.attach.chars().any(|c| c.is_control()) {
            return Err(format!(
                "manifest: script {:?} has an invalid attach name",
                s.file
            ));
        }
        for (k, v) in &s.params {
            if !(v.is_number() || v.is_string()) {
                return Err(format!(
                    "manifest: param {k:?} must be a number or a string"
                ));
            }
        }
    }
    if m.events_out
        .iter()
        .any(|e| e.is_empty() || e.len() > 63 || e.chars().any(|c| c.is_control()))
    {
        return Err("manifest: invalid event name".to_string());
    }
    for f in referenced_files(m) {
        check_path(&f, limits).map_err(|e| format!("manifest: {e}"))?;
        if !names.contains(&f) {
            return Err(format!("manifest names {f:?}, which is not in the bundle"));
        }
    }
    Ok(())
}

fn temp_sibling(dest: &Path) -> Result<PathBuf, String> {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    let parent = dest
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let name = dest
        .file_name()
        .ok_or_else(|| "destination has no name".to_string())?
        .to_string_lossy();
    Ok(parent.join(format!(
        ".{name}.tmp-{}-{}",
        std::process::id(),
        N.fetch_add(1, Ordering::Relaxed)
    )))
}

fn extract_into(
    archive: &mut ZipArchive<Cursor<&[u8]>>,
    names: &HashSet<String>,
    tmp: &Path,
    limits: &Limits,
) -> Result<Manifest, String> {
    let io = |e: std::io::Error| e.to_string();
    let mut total: u64 = 0;
    for i in 0..archive.len() {
        let mut f = archive.by_index(i).map_err(|e| e.to_string())?;
        let raw = f.name().map_err(|e| e.to_string())?.into_owned();
        let rel = raw.strip_suffix('/').unwrap_or(&raw).to_string();
        let target = tmp.join(&rel);
        if f.is_dir() {
            std::fs::create_dir_all(&target).map_err(io)?;
            continue;
        }
        if let Some(p) = target.parent() {
            std::fs::create_dir_all(p).map_err(io)?;
        }
        let declared = f.size();
        let mut out = File::options()
            .write(true)
            .create_new(true)
            .open(&target)
            .map_err(io)?;
        // Count what really comes out; one byte past the declared size is a lie.
        let mut buf = [0u8; 64 * 1024];
        let mut got: u64 = 0;
        loop {
            let n = f.read(&mut buf).map_err(io)?;
            if n == 0 {
                break;
            }
            got += n as u64;
            total += n as u64;
            if got > declared {
                return Err(format!("{raw:?} is larger than its header says"));
            }
            if raw == MANIFEST_NAME && got > MAX_MANIFEST {
                return Err(format!("{MANIFEST_NAME} is too large"));
            }
            if got > limits.max_file || total > limits.max_total {
                return Err("bundle is too large".to_string());
            }
            out.write_all(&buf[..n]).map_err(io)?;
        }
        if got != declared {
            return Err(format!("{raw:?} is shorter than its header says"));
        }
    }
    let manifest_bytes = std::fs::read(tmp.join(MANIFEST_NAME)).map_err(io)?;
    let m = parse_manifest(&manifest_bytes)?;
    validate_manifest(&m, names, limits)?;
    Ok(m)
}

/// Validates `bytes` and, only if everything passes, creates `dest` with the
/// bundle's files. `dest` must not exist (an empty directory is replaced).
pub fn extract(bytes: &[u8], dest: &Path, limits: &Limits) -> Result<Manifest, String> {
    // Safety net: any error may quote bundle text (serde prints unknown field names verbatim).
    extract_with_tmp(bytes, dest, limits, &temp_sibling(dest)?).map_err(|e| clean_text(&e, 300))
}

fn extract_with_tmp(
    bytes: &[u8],
    dest: &Path,
    limits: &Limits,
    tmp: &Path,
) -> Result<Manifest, String> {
    let mut archive = ZipArchive::new(Cursor::new(bytes)).map_err(|e| format!("not a zip: {e}"))?;
    let names = validate_entries(&mut archive, limits)?;
    // Only a directory this call created is ever removed.
    std::fs::create_dir(tmp).map_err(|e| e.to_string())?;
    let result = extract_into(&mut archive, &names, tmp, limits).and_then(|m| {
        if dest.is_dir() {
            // Only an empty directory may be replaced; remove_dir refuses others.
            std::fs::remove_dir(dest).map_err(|e| format!("destination exists: {e}"))?;
        }
        std::fs::rename(tmp, dest).map_err(|e| e.to_string())?;
        Ok(m)
    });
    if result.is_err() {
        let _ = std::fs::remove_dir_all(tmp);
    }
    result
}

fn collect(
    dir: &Path,
    prefix: &str,
    limits: &Limits,
    total: &mut u64,
    out: &mut Vec<(String, PathBuf)>,
) -> Result<(), String> {
    let rd = std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    for entry in rd {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry
            .file_name()
            .into_string()
            .map_err(|n| format!("non-UTF-8 name {n:?}"))?;
        // .git, .DS_Store, editor swap files: never part of a world.
        if name.starts_with('.') {
            continue;
        }
        let rel = if prefix.is_empty() {
            name
        } else {
            format!("{prefix}/{name}")
        };
        let ft = entry.file_type().map_err(|e| e.to_string())?;
        if ft.is_dir() {
            collect(&entry.path(), &rel, limits, total, out)?;
        } else if ft.is_file() {
            let size = entry.metadata().map_err(|e| e.to_string())?.len();
            if size > limits.max_file {
                return Err(format!("pack: file {rel:?} is too large ({size} bytes)"));
            }
            *total = total.saturating_add(size);
            if *total > limits.max_total {
                return Err("pack: files are too large in total".to_string());
            }
            if out.len() as u64 >= limits.max_files as u64 {
                return Err(format!("pack: too many files (over {})", limits.max_files));
            }
            out.push((rel, entry.path()));
        } else {
            return Err(format!("{rel:?} is not a regular file"));
        }
    }
    Ok(())
}

/// Zips the directory deterministically. The result is checked the way an import
/// would check it, so a bundle that cannot be imported is never produced.
pub fn pack_dir(dir: &Path) -> Result<Vec<u8>, String> {
    pack_dir_with(dir, &Limits::default())
}

fn pack_dir_with(dir: &Path, limits: &Limits) -> Result<Vec<u8>, String> {
    let mut files = Vec::new();
    collect(dir, "", limits, &mut 0, &mut files)?;
    files.sort_by(|a, b| a.0.cmp(&b.0));
    let opts = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Deflated)
        .compression_level(Some(6))
        .last_modified_time(DateTime::default())
        .unix_permissions(0o644);
    let mut w = ZipWriter::new(Cursor::new(Vec::new()));
    let mut manifest_bytes = None;
    for (rel, path) in &files {
        w.start_file(rel.as_str(), opts)
            .map_err(|e| e.to_string())?;
        if rel == MANIFEST_NAME {
            // Validate the very bytes that are packed.
            let mut b = Vec::new();
            File::open(path)
                .and_then(|f| f.take(MAX_MANIFEST + 1).read_to_end(&mut b))
                .map_err(|e| format!("{rel}: {e}"))?;
            if b.len() as u64 > MAX_MANIFEST {
                return Err(format!("{MANIFEST_NAME} is too large"));
            }
            w.write_all(&b).map_err(|e| format!("{rel}: {e}"))?;
            manifest_bytes = Some(b);
        } else {
            // The file may have grown since collect() looked at its size.
            let f = File::open(path).map_err(|e| format!("{rel}: {e}"))?;
            let n = std::io::copy(&mut f.take(limits.max_file + 1), &mut w)
                .map_err(|e| format!("{rel}: {e}"))?;
            if n > limits.max_file {
                return Err(format!("pack: file {rel:?} is too large"));
            }
        }
    }
    let bytes = w.finish().map_err(|e| e.to_string())?.into_inner();

    let mut archive = ZipArchive::new(Cursor::new(&bytes[..])).map_err(|e| e.to_string())?;
    let names = validate_entries(&mut archive, limits)?;
    let manifest =
        parse_manifest(&manifest_bytes.ok_or_else(|| format!("{MANIFEST_NAME} is missing"))?)?;
    validate_manifest(&manifest, &names, limits)?;
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::sync::atomic::{AtomicU64, Ordering};
    use zip::write::SimpleFileOptions;
    use zip::ZipWriter;

    fn tmp(name: &str) -> PathBuf {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "tesseract_world_bundle_{}_{}_{}",
            std::process::id(),
            name,
            n
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    const MANIFEST: &str = r#"{
        "format": 1,
        "name": "Test world",
        "field_asset": "field.glb",
        "user_asset": "robot.glb",
        "scripts": [
            {"file": "scripts/world.lua", "attach": "world"},
            {"file": "scripts/day.lua", "attach": "daylight", "params": {"clock": 0.5, "tint": "warm"}},
            {"file": "scripts/player.lua", "attach": "local_player", "on_camera": true}
        ],
        "user_script": {"file": "robot.lua", "attach": "robot"},
        "events_out": ["pose", "send"]
    }"#;

    fn write_world(dir: &Path, manifest: &str) {
        std::fs::create_dir_all(dir.join("scripts")).unwrap();
        std::fs::write(dir.join("world.json"), manifest).unwrap();
        std::fs::write(dir.join("field.glb"), b"field-bytes").unwrap();
        std::fs::write(dir.join("robot.glb"), b"robot-bytes").unwrap();
        std::fs::write(dir.join("robot.lua"), b"-- robot").unwrap();
        for s in ["world", "day", "player"] {
            std::fs::write(dir.join(format!("scripts/{s}.lua")), format!("-- {s}")).unwrap();
        }
    }

    fn valid_dir(name: &str) -> PathBuf {
        let d = tmp(name);
        write_world(&d, MANIFEST);
        d
    }

    /// A zip with the valid world's files plus `extra` (name, bytes) entries.
    fn zip_with(manifest: &str, extra: &[(&str, &[u8])]) -> Vec<u8> {
        let mut w = ZipWriter::new(std::io::Cursor::new(Vec::new()));
        let o = SimpleFileOptions::default();
        let mut put = |n: &str, b: &[u8]| {
            w.start_file(n, o).unwrap();
            w.write_all(b).unwrap();
        };
        put("world.json", manifest.as_bytes());
        put("field.glb", b"f");
        put("robot.glb", b"r");
        put("robot.lua", b"-");
        for s in ["world", "day", "player"] {
            put(&format!("scripts/{s}.lua"), b"-");
        }
        for (n, b) in extra {
            put(n, b);
        }
        w.finish().unwrap().into_inner()
    }

    fn extract_err(bytes: &[u8], limits: &Limits) -> (String, PathBuf) {
        let parent = tmp("dest");
        let dest = parent.join("out");
        let err = extract(bytes, &dest, limits).expect_err("must be rejected");
        assert!(!dest.exists(), "nothing may be left behind");
        let leftovers: Vec<_> = std::fs::read_dir(&parent).unwrap().collect();
        assert!(leftovers.is_empty(), "temp dir left behind");
        (err, dest)
    }

    fn rejected(bytes: &[u8]) -> String {
        extract_err(bytes, &Limits::default()).0
    }

    #[test]
    fn manifest_rejects_unsafe_attach_and_event_names() {
        for bad in ["", &"a".repeat(64), "world\\u0000x", "wor\\nld"] {
            let m = MANIFEST.replace("\"attach\": \"world\"", &format!("\"attach\": \"{bad}\""));
            assert!(
                rejected(&zip_with(&m, &[])).contains("attach"),
                "attach {bad:?}"
            );
        }
        let ok = MANIFEST.replace("\"attach\": \"world\"", "\"attach\": \"go_kart\"");
        let dest = tmp("kart").join("out");
        extract(&zip_with(&ok, &[]), &dest, &Limits::default()).unwrap();
        for bad in ["", "a\\u0000b"] {
            let m = MANIFEST.replace("\"pose\"", &format!("\"{bad}\""));
            assert!(rejected(&zip_with(&m, &[])).contains("event"));
        }
    }

    #[test]
    fn pack_is_deterministic() {
        let d = valid_dir("det");
        let a = pack_dir(&d).unwrap();
        let b = pack_dir(&d).unwrap();
        assert_eq!(a, b);
        // Touch mtimes only.
        let f = std::fs::File::options()
            .write(true)
            .open(d.join("field.glb"))
            .unwrap();
        f.set_modified(
            std::time::SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(86400 * 400),
        )
        .unwrap();
        assert_eq!(a, pack_dir(&d).unwrap());
        assert_eq!(sha256_hex(&a), sha256_hex(&b));
        assert_eq!(sha256_hex(b"abc").len(), 64);
    }

    #[test]
    fn roundtrip_extracts_identical_files() {
        let d = valid_dir("rt");
        let bytes = pack_dir(&d).unwrap();
        let parent = tmp("rt_dest");
        let dest = parent.join("world");
        let m = extract(&bytes, &dest, &Limits::default()).unwrap();
        assert_eq!(m.name, "Test world");
        assert_eq!(m.scripts.len(), 3);
        assert_eq!(m.scripts[1].params.get("tint").unwrap(), "warm");
        assert!(m.scripts[2].on_camera);
        assert_eq!(m.events_out, vec!["pose", "send"]);
        for f in referenced_files(&m) {
            assert_eq!(
                std::fs::read(d.join(&f)).unwrap(),
                std::fs::read(dest.join(&f)).unwrap(),
                "{f}"
            );
        }
        let refs = referenced_files(&m);
        assert!(refs.contains(&"world.json".to_string()));
        assert!(refs.contains(&"scripts/day.lua".to_string()));
        assert!(refs.contains(&"robot.lua".to_string()));
        assert!(refs.contains(&"field.glb".to_string()));
    }

    #[test]
    fn rejects_parent_dir_entry() {
        for n in ["../evil.txt", "a/../../evil.txt", "a/./b.txt"] {
            let e = rejected(&zip_with(MANIFEST, &[(n, b"x")]));
            assert!(e.contains("unsafe path") && e.contains("relative"), "{e}");
        }
    }

    #[test]
    fn rejects_absolute_and_backslash_and_drive_paths() {
        for n in [
            "/etc/passwd",
            "a\\b.txt",
            "..\\b.txt",
            "C:/b.txt",
            "C:b.txt",
            "a//b.txt",
        ] {
            let e = rejected(&zip_with(MANIFEST, &[(n, b"x")]));
            assert!(e.contains("unsafe path"), "{n}: {e}");
        }
    }

    #[test]
    fn rejects_symlink_entry() {
        let mut w = ZipWriter::new(std::io::Cursor::new(Vec::new()));
        let o = SimpleFileOptions::default();
        w.start_file("world.json", o).unwrap();
        w.write_all(MANIFEST.as_bytes()).unwrap();
        w.add_symlink("data.bin", "/etc/passwd", o).unwrap();
        let bytes = w.finish().unwrap().into_inner();
        let e = rejected(&bytes);
        assert!(
            e.contains("data.bin") && e.contains("is a link or special file"),
            "{e}"
        );
    }

    #[test]
    fn rejects_case_colliding_names() {
        for extra in [
            vec![("Tex.png", &b"x"[..]), ("tex.PNG", b"y")],
            vec![("scripts", b"x")],
            vec![("SCRIPTS/x", b"x")],
        ] {
            let e = rejected(&zip_with(MANIFEST, &extra));
            assert!(e.contains("collid") || e.contains("both a file"), "{e}");
        }
    }

    #[test]
    fn rejects_too_many_files() {
        let l = Limits {
            max_files: 3,
            ..Limits::default()
        };
        let (e, _) = extract_err(&zip_with(MANIFEST, &[]), &l);
        assert!(e.contains("too many entries"), "{e}");
    }

    #[test]
    fn rejects_declared_size_over_limit() {
        let big = vec![7u8; 1000];
        let bytes = zip_with(MANIFEST, &[("big.bin", &big)]);
        let l = Limits {
            max_file: 800,
            ..Limits::default()
        };
        let (e, _) = extract_err(&bytes, &l);
        assert!(e.contains("big.bin") && e.contains("is too large"), "{e}");
        let l = Limits {
            max_total: 500,
            ..Limits::default()
        };
        let (e, _) = extract_err(&bytes, &l);
        assert!(e.contains("bundle is too large"), "{e}");
    }

    #[test]
    fn rejects_actual_size_over_declared() {
        let mut w = ZipWriter::new(std::io::Cursor::new(Vec::new()));
        let o = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        w.start_file("world.json", o).unwrap();
        w.write_all(MANIFEST.as_bytes()).unwrap();
        w.start_file("bomb.bin", o).unwrap();
        w.write_all(&vec![0u8; 200_000]).unwrap();
        let mut bytes = w.finish().unwrap().into_inner();
        // Lie: patch the uncompressed size of bomb.bin to 10 in both headers.
        let mut patched = 0;
        for i in 0..bytes.len().saturating_sub(46) {
            let sig = &bytes[i..i + 4];
            let off = if sig == b"PK\x03\x04" {
                22
            } else if sig == b"PK\x01\x02" {
                24
            } else {
                continue;
            };
            let name_off = if off == 22 { 30 } else { 46 };
            if bytes[i + name_off..].starts_with(b"bomb.bin") {
                bytes[i + off..i + off + 4].copy_from_slice(&10u32.to_le_bytes());
                patched += 1;
            }
        }
        assert_eq!(patched, 2);
        let (err, _) = extract_err(&bytes, &Limits::default());
        assert!(err.contains("larger than"), "{err}");
    }

    #[test]
    fn rejects_missing_manifest() {
        let mut w = ZipWriter::new(std::io::Cursor::new(Vec::new()));
        w.start_file("field.glb", SimpleFileOptions::default())
            .unwrap();
        w.write_all(b"x").unwrap();
        let bytes = w.finish().unwrap().into_inner();
        assert!(rejected(&bytes).contains("world.json"));
    }

    #[test]
    fn rejects_manifest_naming_missing_file() {
        let m = MANIFEST.replace("robot.lua", "nope.lua");
        let bytes = zip_with(&m, &[]);
        assert!(rejected(&bytes).contains("nope.lua"));
    }

    #[test]
    fn rejects_script_path_escaping_bundle() {
        for bad in ["../outside.lua", "/abs.lua", "scripts\\\\x.lua"] {
            let m = MANIFEST.replace("scripts/world.lua", bad);
            let e = rejected(&zip_with(&m, &[]));
            assert!(
                e.contains("manifest") && e.contains("unsafe path"),
                "{bad}: {e}"
            );
        }
    }

    #[test]
    fn rejects_newer_format() {
        let m = MANIFEST.replace("\"format\": 1", "\"format\": 99");
        assert!(rejected(&zip_with(&m, &[])).contains("newer"));
    }

    #[test]
    fn rejects_duplicate_entry_names() {
        let mut bytes = zip_with(MANIFEST, &[("field.gld", b"g")]);
        let from = b"field.gld";
        let mut i = 0;
        while i + from.len() <= bytes.len() {
            if &bytes[i..i + from.len()] == from {
                bytes[i..i + from.len()].copy_from_slice(b"field.glb");
                i += from.len();
            } else {
                i += 1;
            }
        }
        let e = rejected(&bytes);
        assert!(e.contains("duplicate"), "{e}");
    }

    #[test]
    fn rejects_oversized_manifest() {
        let mut m = String::from("{\"x\":[0");
        while m.len() < 1024 * 1024 {
            m.push_str(",0");
        }
        m.push_str("]}");
        let e = rejected(&zip_with(&m, &[]));
        assert!(e.contains("world.json") && e.contains("too large"), "{e}");
    }

    #[test]
    fn manifest_references_are_exact_names() {
        let m = MANIFEST.replace("\"field.glb\"", "\"Field.glb\"");
        let e = rejected(&zip_with(&m, &[]));
        assert!(e.contains("Field.glb"), "{e}");
    }

    #[test]
    fn rejects_windows_reserved_names() {
        for n in ["scripts/aux.lua", "nul.glb", "COM1.txt", "lpt9", "Con"] {
            let e = rejected(&zip_with(MANIFEST, &[(n, b"x")]));
            assert!(e.contains("reserved"), "{n}: {e}");
        }
    }

    #[test]
    fn a_stale_temp_dir_is_never_deleted() {
        let parent = tmp("stale");
        let stale = parent.join(".out.tmp-1-0");
        std::fs::create_dir(&stale).unwrap();
        std::fs::write(stale.join("keep"), b"k").unwrap();
        // Structurally valid, so it gets past pass 1; the manifest then fails.
        let bytes = zip_with(&MANIFEST.replace("robot.lua", "nope.lua"), &[]);
        assert!(extract_with_tmp(&bytes, &parent.join("out"), &Limits::default(), &stale).is_err());
        assert!(stale.is_dir() && stale.join("keep").exists());
        assert!(!parent.join("out").exists());
    }

    #[test]
    fn pack_skips_dot_entries() {
        let d = valid_dir("dots");
        std::fs::create_dir_all(d.join(".git")).unwrap();
        std::fs::write(d.join(".git/config"), b"x").unwrap();
        std::fs::write(d.join(".DS_Store"), b"x").unwrap();
        std::fs::write(d.join("scripts/.x.swp"), b"x").unwrap();
        let bytes = pack_dir(&d).unwrap();
        let a = ZipArchive::new(Cursor::new(&bytes[..])).unwrap();
        assert!(a
            .file_names()
            .all(|n| !n.unwrap().split('/').any(|c| c.starts_with('.'))));
        assert_eq!(a.len(), 7);
    }

    #[test]
    fn pack_checks_limits_before_reading() {
        let d = valid_dir("packlim");
        let l = Limits {
            max_file: 5,
            ..Limits::default()
        };
        assert!(pack_dir_with(&d, &l).unwrap_err().contains("pack: file"));
        let l = Limits {
            max_files: 2,
            ..Limits::default()
        };
        assert!(pack_dir_with(&d, &l)
            .unwrap_err()
            .contains("pack: too many files"));
    }

    #[test]
    fn rejects_bidi_and_invisible_format_characters_in_entry_names() {
        for c in [
            '\u{202E}', '\u{200B}', '\u{200F}', '\u{2060}', '\u{2066}', '\u{FEFF}', '\u{180E}',
            '\u{00AD}', '\u{FFA0}', '\u{3164}', '\u{115F}', '\u{1160}', '\u{2800}',
        ] {
            let n = format!("notes{c}txt.exe");
            let e = rejected(&zip_with(MANIFEST, &[(n.as_str(), b"x")]));
            assert!(e.contains("unsafe path"), "{c:?}: {e}");
        }
    }

    #[test]
    fn rejects_a_path_over_255_bytes() {
        let n = format!("{}.txt", "a".repeat(252));
        assert_eq!(n.len(), 256);
        let e = rejected(&zip_with(MANIFEST, &[(n.as_str(), b"x")]));
        assert!(e.contains("unsafe path") && e.contains("too long"), "{e}");
        let ok = format!("{}.txt", "a".repeat(251));
        assert_eq!(ok.len(), 255);
        let dest = tmp("p255").join("out");
        extract(
            &zip_with(MANIFEST, &[(ok.as_str(), b"x")]),
            &dest,
            &Limits::default(),
        )
        .unwrap();
    }

    #[test]
    fn errors_never_carry_bundle_controlled_bidi_or_unbounded_text() {
        // serde prints the unknown field's name verbatim.
        let field = format!("\u{202E}{}choose Always run\u{200B}", "x".repeat(500));
        let m = MANIFEST.replacen(
            "\"format\": 1,",
            &format!("\"format\": 1, \"{field}\": 1,"),
            1,
        );
        let e = rejected(&zip_with(&m, &[]));
        assert!(e.contains("unknown field"), "{e}");
        assert!(
            !e.chars().any(|c| c.is_control() || is_hidden_char(c)),
            "{e:?}"
        );
        assert!(e.chars().count() <= 400, "{}", e.chars().count());
        // An entry name is quoted clamped too.
        let n = format!("{}\u{202E}.exe", "b".repeat(300));
        let e = rejected(&zip_with(MANIFEST, &[(n.as_str(), b"x")]));
        assert!(e.chars().count() <= 400, "{}", e.chars().count());
        assert!(!e.contains('\u{202E}'), "{e:?}");
    }
}
