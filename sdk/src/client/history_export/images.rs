//! Bounded-concurrency image download pass for "include images" HTML
//! export. Called once per window — over exactly that window's message
//! images, and separately over that window's not-yet-fetched sender
//! avatars — bounding memory the same way the rest of the windowed walk
//! does. Never interleaved with the pagination calls themselves, so a
//! slow or failing download can't stall or corrupt the text walk.
//!
//! Deliberately uses a plain `tokio::sync::Semaphore` rather than the
//! existing `media_gate_fg`/`media_gate_bulk` `PriorityGate`s: the goal
//! here is just "an export can never occupy every slot those gates offer
//! the rest of the app," which a small dedicated semaphore guarantees
//! without pulling in `PriorityGate`'s AIMD/stall-reclamation machinery,
//! which is tuned for the interactive scroll/thumbnail workload, not a
//! one-shot bulk pass.

use std::collections::HashSet;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use futures_util::{stream, StreamExt};
use matrix_sdk::Client;

/// One image event's download descriptor, collected while converting a
/// window's events.
pub(super) struct ImageDescriptor {
    pub event_id: String,
    /// Whatever `download_media` should be given as `source`: the plain
    /// `mxc://` URI, or (for encrypted media) the JSON-serialized
    /// `MediaSource` blob — i.e. `split_source`'s `encrypted_json` when
    /// non-empty, else its `url`.
    pub source: String,
    pub original_name: String,
}

pub(super) enum ImageOutcome {
    Saved { rel_path: String },
    Skipped,
    TooLarge,
    Failed,
}

/// Downloads every descriptor into `media_dir` (created if absent), at most
/// `max_concurrent` at a time. Returns one outcome per input descriptor, in
/// the same order, so the caller can look up each event's outcome by index.
/// `used_names` is the caller's collision-avoidance set: the per-window
/// message-image pass hands in a fresh one each call (a stale name never
/// matters across windows there), but the avatar pass hands in one that
/// persists across the whole export — two different senders named "Alice"
/// must not collide even though they're fetched in different windows.
/// Gated `#[cfg(not(test))]` because it calls into `client::media`'s
/// network-touching helpers, which are gated the same way — the pure
/// `sanitize_component`/`unique_file_name` helpers below stay un-gated so
/// they remain unit-testable without a live client.
#[cfg(not(test))]
pub(super) async fn download_images(
    client: &Client,
    media_dir: &Path,
    descriptors: Vec<ImageDescriptor>,
    max_concurrent: usize,
    cancel: &Arc<AtomicBool>,
    used_names: &mut HashSet<String>,
) -> Vec<(ImageDescriptor, ImageOutcome)> {
    if descriptors.is_empty() {
        return Vec::new();
    }
    if let Err(e) = tokio::fs::create_dir_all(media_dir).await {
        tracing::warn!("history export: failed to create media dir {media_dir:?}: {e}");
        return descriptors.into_iter().map(|d| (d, ImageOutcome::Failed)).collect();
    }

    // Pre-assign filenames sequentially (cheap, no I/O) so the concurrent
    // downloads below never race on collision-avoidance bookkeeping.
    let planned: Vec<(ImageDescriptor, String)> = descriptors
        .into_iter()
        .map(|d| {
            let name = unique_file_name(&d.original_name, used_names);
            (d, name)
        })
        .collect();

    stream::iter(planned)
        .map(|(desc, file_name)| {
            let client = client.clone();
            let media_dir = media_dir.to_path_buf();
            let cancel = Arc::clone(cancel);
            async move {
                if cancel.load(Ordering::Relaxed) {
                    return (desc, ImageOutcome::Skipped);
                }
                match super::super::media::download_media_outcome(
                    &client,
                    super::super::media::MEDIA_KIND_SOURCE_FULL,
                    &desc.source,
                    0,
                    0,
                    false,
                )
                .await
                {
                    super::super::media::MediaFetchOutcome::Failed => (desc, ImageOutcome::Failed),
                    super::super::media::MediaFetchOutcome::TooLarge => (desc, ImageOutcome::TooLarge),
                    super::super::media::MediaFetchOutcome::Ok(bytes) => {
                        // `download_media_outcome` returns raw bytes, no
                        // content-type — the pre-assigned `file_name` (see
                        // `unique_file_name` above) already carries a real
                        // extension for most message images (their
                        // original upload filename), but an avatar's
                        // `original_name` is a sender display name with
                        // none at all. Sniffing from magic bytes only
                        // fills that gap; it never touches an already-
                        // extensioned name, so the pre-assigned
                        // uniqueness above still holds — each task only
                        // appends to its own unique base name.
                        let final_name = if has_extension(&file_name) {
                            file_name
                        } else {
                            match sniff_image_extension(&bytes) {
                                Some(ext) => format!("{file_name}.{ext}"),
                                None => file_name,
                            }
                        };
                        match tokio::fs::write(media_dir.join(&final_name), &bytes).await {
                            Ok(()) => (desc, ImageOutcome::Saved { rel_path: final_name }),
                            Err(_) => (desc, ImageOutcome::Failed),
                        }
                    }
                }
            }
        })
        .buffered(max_concurrent.max(1))
        .collect()
        .await
}

/// Images are saved under their original filename by default. Only on an
/// actual collision (two images in the room share the same original name)
/// does a disambiguating number get added — appended before the
/// extension, e.g. "photo.jpg" -> "photo-1.jpg", not as some ID-based
/// prefix on every file regardless of whether it's ever needed.
fn unique_file_name(original_name: &str, used: &mut HashSet<String>) -> String {
    let base_name = if original_name.is_empty() {
        "file".to_string()
    } else {
        sanitize_component(original_name)
    };
    if used.insert(base_name.clone()) {
        return base_name;
    }
    let mut n = 1u32;
    loop {
        let alt = insert_disambiguator(&base_name, n);
        if used.insert(alt.clone()) {
            return alt;
        }
        n += 1;
    }
}

/// Inserts "-{n}" before the last '.'-delimited extension, or appends it
/// if there's no extension (or the "extension" is the whole name, e.g. a
/// dotfile-style ".jpg" with nothing before the dot).
fn insert_disambiguator(name: &str, n: u32) -> String {
    match name.rfind('.') {
        Some(idx) if idx > 0 => format!("{}-{n}{}", &name[..idx], &name[idx..]),
        _ => format!("{name}-{n}"),
    }
}

/// Same "does this name really have an extension" rule `insert_disambiguator`
/// uses (a leading dot with nothing before it, e.g. ".jpg", doesn't count).
fn has_extension(name: &str) -> bool {
    matches!(name.rfind('.'), Some(idx) if idx > 0)
}

/// Sniffs an image file extension from its magic bytes. Only for the
/// gap `unique_file_name` can't fill on its own: an avatar's
/// `original_name` is a sender display name, which never carries a real
/// extension the way a message image's original upload filename usually
/// does. `download_media_outcome` returns raw bytes with no content-type,
/// so this is the only signal available.
fn sniff_image_extension(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        Some("png")
    } else if bytes.starts_with(b"\xFF\xD8\xFF") {
        Some("jpg")
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some("gif")
    } else if bytes.len() >= 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some("webp")
    } else {
        None
    }
}

/// Strips characters illegal (or awkward) in a filename on Windows, macOS,
/// or Linux, and caps length so a long caption-derived name can't exceed
/// filesystem limits.
fn sanitize_component(s: &str) -> String {
    let cleaned: String = s
        .chars()
        .map(|c| match c {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' | '\0' => '_',
            c if c.is_control() => '_',
            c => c,
        })
        .collect();
    let trimmed = cleaned.trim().trim_matches('.');
    let capped: String = trimmed.chars().take(120).collect();
    if capped.is_empty() {
        "file".to_string()
    } else {
        capped
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn has_extension_true_for_a_real_extension() {
        assert!(has_extension("photo.jpg"));
    }

    #[test]
    fn has_extension_false_with_no_dot() {
        assert!(!has_extension("Alice"));
    }

    #[test]
    fn has_extension_false_for_a_dotfile_style_leading_dot() {
        assert!(!has_extension(".jpg"));
    }

    #[test]
    fn sniff_image_extension_recognizes_png() {
        let mut bytes = vec![0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];
        bytes.extend_from_slice(&[0, 0, 0, 0]);
        assert_eq!(sniff_image_extension(&bytes), Some("png"));
    }

    #[test]
    fn sniff_image_extension_recognizes_jpeg() {
        assert_eq!(sniff_image_extension(&[0xFF, 0xD8, 0xFF, 0xE0]), Some("jpg"));
    }

    #[test]
    fn sniff_image_extension_recognizes_gif() {
        assert_eq!(sniff_image_extension(b"GIF89a...."), Some("gif"));
    }

    #[test]
    fn sniff_image_extension_recognizes_webp() {
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&[0, 0, 0, 0]); // file size, irrelevant here
        bytes.extend_from_slice(b"WEBP");
        assert_eq!(sniff_image_extension(&bytes), Some("webp"));
    }

    #[test]
    fn sniff_image_extension_unknown_bytes_returns_none() {
        assert_eq!(sniff_image_extension(b"not an image"), None);
    }

    #[test]
    fn sanitize_component_strips_illegal_characters() {
        assert_eq!(sanitize_component("a/b\\c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
    }

    #[test]
    fn sanitize_component_caps_length() {
        let long = "x".repeat(500);
        assert_eq!(sanitize_component(&long).chars().count(), 120);
    }

    #[test]
    fn sanitize_component_empty_falls_back_to_file() {
        assert_eq!(sanitize_component(""), "file");
        assert_eq!(sanitize_component("..."), "file");
    }

    #[test]
    fn unique_file_name_uses_original_name_by_default() {
        let mut used = HashSet::new();
        assert_eq!(unique_file_name("photo.jpg", &mut used), "photo.jpg");
    }

    #[test]
    fn unique_file_name_dedupes_on_collision_by_numbering_before_extension() {
        let mut used = HashSet::new();
        let a = unique_file_name("photo.jpg", &mut used);
        let b = unique_file_name("photo.jpg", &mut used);
        let c = unique_file_name("photo.jpg", &mut used);
        assert_eq!(a, "photo.jpg");
        assert_eq!(b, "photo-1.jpg");
        assert_eq!(c, "photo-2.jpg");
    }

    #[test]
    fn unique_file_name_empty_original_name_falls_back_to_file() {
        let mut used = HashSet::new();
        assert_eq!(unique_file_name("", &mut used), "file");
    }

    #[test]
    fn unique_file_name_collision_without_extension_just_appends() {
        let mut used = HashSet::new();
        let a = unique_file_name("attachment", &mut used);
        let b = unique_file_name("attachment", &mut used);
        assert_eq!(a, "attachment");
        assert_eq!(b, "attachment-1");
    }

    #[test]
    fn insert_disambiguator_handles_leading_dot_as_no_extension() {
        // A name like ".jpg" has nothing before the dot to treat as a
        // basename, so it's not a real "extension" to preserve.
        assert_eq!(insert_disambiguator(".jpg", 1), ".jpg-1");
    }
}
