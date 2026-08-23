//! Bounded-concurrency image download pass for "include images" HTML
//! export. Run once per window, over exactly that window's image
//! descriptors (bounding memory the same way the rest of the windowed walk
//! does) — never interleaved with the pagination calls themselves, so a
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
    let mut used_names: HashSet<String> = HashSet::new();
    let planned: Vec<(ImageDescriptor, String)> = descriptors
        .into_iter()
        .map(|d| {
            let name = unique_file_name(&d.original_name, &mut used_names);
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
                        match tokio::fs::write(media_dir.join(&file_name), &bytes).await {
                            Ok(()) => (desc, ImageOutcome::Saved { rel_path: file_name }),
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
