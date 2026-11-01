//! Removal of privacy-sensitive metadata (GPS atoms, tags, comments, XMP)
//! from audio and video uploads: MP4/MOV/M4A/3GP, WebM/Matroska, WAV/AVI,
//! MP3/AAC (ID3, APE, Lyrics3), FLAC and Ogg (Vorbis/Opus/Theora).
//!
//! The container formats whose payload holds absolute offsets (MP4 `stco`,
//! AVI indexes, Matroska seeks) are rewritten size-preserving: the unwanted
//! box/chunk/element is overwritten with a `free`/`JUNK`/`Void` of the same
//! length, so nothing after it moves. Streams without offsets are rebuilt.
//! Media streams are never touched. Anything unrecognised or unparseable is
//! `None`; callers refuse the upload rather than send the original.

use super::strip_meta::{iso_boxes, IsoBox};
use std::borrow::Cow;

/// The stripped bytes (borrowed when there was nothing to remove), or None
/// when the file cannot be confidently stripped. A parser panic is None too.
pub(super) fn strip_av_for_upload(bytes: &[u8]) -> Option<Cow<'_, [u8]>> {
    let out = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| strip_inner(bytes)))
        .ok()
        .flatten()?;
    Some(if out == bytes {
        Cow::Borrowed(bytes)
    } else {
        Cow::Owned(out)
    })
}

fn strip_inner(b: &[u8]) -> Option<Vec<u8>> {
    if b.len() >= 12 && &b[4..8] == b"ftyp" {
        strip_iso(b)
    } else if b.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
        strip_ebml(b)
    } else if b.len() >= 12 && &b[..4] == b"RIFF" {
        strip_riff(b)
    } else if b.starts_with(b"OggS") {
        strip_ogg(b)
    } else {
        let head = id3v2_len(b)?;
        let rest = &b[head..];
        if rest.starts_with(b"fLaC") {
            strip_flac(rest)
        } else {
            strip_mpeg_audio(rest)
        }
    }
}

// ---------------------------------------------------------------------------
// ISO base media (MP4, MOV, M4A, 3GP)
// ---------------------------------------------------------------------------

/// Overwrites a box with a same-sized `free` box.
fn blank_box(out: &mut [u8], bx: &IsoBox) {
    out[bx.start + 4..bx.start + 8].copy_from_slice(b"free");
    out[bx.payload..bx.end].fill(0);
}

/// `udta` (©xyz location, tags), `meta` (iTunes/Apple keys), `uuid` (XMP and
/// vendor data), `XMP_`, and the slack in `free`/`skip` boxes.
fn is_iso_metadata(ty: &[u8; 4]) -> bool {
    matches!(
        ty,
        b"udta" | b"meta" | b"uuid" | b"XMP_" | b"free" | b"skip"
    )
}

fn strip_iso(b: &[u8]) -> Option<Vec<u8>> {
    let mut out = b.to_vec();
    let mut saw_moov = false;
    for bx in iso_boxes(b, 0, b.len())? {
        if &bx.ty == b"moov" {
            saw_moov = true;
            for child in iso_boxes(b, bx.payload, bx.end)? {
                if is_iso_metadata(&child.ty) {
                    blank_box(&mut out, &child);
                } else if &child.ty == b"trak" {
                    for t in iso_boxes(b, child.payload, child.end)? {
                        if is_iso_metadata(&t.ty) {
                            blank_box(&mut out, &t);
                        }
                    }
                }
            }
        } else if is_iso_metadata(&bx.ty) {
            blank_box(&mut out, &bx);
        }
    }
    saw_moov.then_some(out)
}

// ---------------------------------------------------------------------------
// Matroska / WebM (EBML)
// ---------------------------------------------------------------------------

const EBML_SEGMENT: u32 = 0x1853_8067;
const EBML_INFO: u32 = 0x1549_A966;
const EBML_TITLE: u32 = 0x7BA9;
const EBML_VOID: u8 = 0xEC;
/// Tags, Chapters, Attachments (cover art and fonts can carry anything).
const EBML_DROP: [u32; 3] = [0x1254_C367, 0x1043_A770, 0x1941_A469];

/// An element ID (marker bits kept) and its encoded length.
fn ebml_id(b: &[u8], pos: usize) -> Option<(u32, usize)> {
    let first = *b.get(pos)?;
    let len = first.leading_zeros() as usize + 1;
    if len > 4 {
        return None;
    }
    let mut v = 0u32;
    for i in 0..len {
        v = (v << 8) | *b.get(pos + i)? as u32;
    }
    Some((v, len))
}

/// An element size (None = unknown) and its encoded length.
fn ebml_size(b: &[u8], pos: usize) -> Option<(Option<u64>, usize)> {
    let first = *b.get(pos)?;
    let len = first.leading_zeros() as usize + 1;
    if len > 8 {
        return None;
    }
    let mut v = (first as u64) & ((1u64 << (8 - len)) - 1);
    let mut all_ones = v == (1u64 << (8 - len)) - 1;
    for i in 1..len {
        let byte = *b.get(pos + i)?;
        all_ones &= byte == 0xFF;
        v = (v << 8) | byte as u64;
    }
    Some((if all_ones { None } else { Some(v) }, len))
}

/// Overwrites `[start, end)` with one Void element.
fn ebml_void(out: &mut [u8], start: usize, end: usize) -> Option<()> {
    let total = end - start;
    if total < 2 {
        return None;
    }
    out[start] = EBML_VOID;
    if total - 2 <= 126 {
        out[start + 1] = 0x80 | (total - 2) as u8;
        out[start + 2..end].fill(0);
    } else if total - 3 <= 16382 {
        let n = (total - 3) as u16 | 0x4000;
        out[start + 1..start + 3].copy_from_slice(&n.to_be_bytes());
        out[start + 3..end].fill(0);
    } else {
        let n = (total - 9) as u64;
        out[start + 1] = 0x01;
        out[start + 2..start + 9].copy_from_slice(&n.to_be_bytes()[1..]);
        out[start + 9..end].fill(0);
    }
    Some(())
}

/// Walks the children of `[start, end)`, voiding what `drop` selects and
/// calling `descend` for elements to look inside. Stops quietly at an
/// unknown-size element (live-stream clusters), which holds only media.
fn ebml_walk(
    out: &mut [u8],
    start: usize,
    end: usize,
    drop: &dyn Fn(u32) -> bool,
    descend: &dyn Fn(&mut [u8], u32, usize, usize) -> Option<()>,
) -> Option<()> {
    let mut pos = start;
    while pos < end {
        let (id, il) = ebml_id(out, pos)?;
        let (size, sl) = ebml_size(out, pos + il)?;
        let Some(size) = size else { return Some(()) };
        let payload = pos + il + sl;
        let elem_end = payload.checked_add(usize::try_from(size).ok()?)?;
        if elem_end > end {
            return None;
        }
        if drop(id) {
            ebml_void(out, pos, elem_end)?;
        } else {
            descend(out, id, payload, elem_end)?;
        }
        pos = elem_end;
    }
    Some(())
}

fn strip_ebml(b: &[u8]) -> Option<Vec<u8>> {
    let mut out = b.to_vec();
    let len = out.len();
    let mut saw_segment = false;
    let in_info = |out: &mut [u8], _id: u32, s: usize, e: usize| {
        ebml_walk(out, s, e, &|id| id == EBML_TITLE, &|_, _, _, _| Some(()))
    };
    let in_segment = |out: &mut [u8], id: u32, s: usize, e: usize| {
        if id == EBML_INFO {
            in_info(out, id, s, e)
        } else {
            Some(())
        }
    };
    let top = |out: &mut [u8], id: u32, s: usize, e: usize| {
        if id == EBML_SEGMENT {
            ebml_walk(out, s, e, &|id| EBML_DROP.contains(&id), &in_segment)
        } else {
            Some(())
        }
    };
    // A Segment of unknown size runs to the end of the file.
    let mut pos = 0;
    while pos < len {
        let (id, il) = ebml_id(&out, pos)?;
        let (size, sl) = ebml_size(&out, pos + il)?;
        let payload = pos + il + sl;
        let elem_end = match size {
            Some(n) => payload.checked_add(usize::try_from(n).ok()?)?,
            None if id == EBML_SEGMENT => len,
            None => return None,
        };
        if elem_end > len {
            return None;
        }
        if id == EBML_SEGMENT {
            saw_segment = true;
        }
        top(&mut out, id, payload, elem_end)?;
        pos = elem_end;
    }
    saw_segment.then_some(out)
}

// ---------------------------------------------------------------------------
// RIFF (WAV, AVI)
// ---------------------------------------------------------------------------

fn strip_riff(b: &[u8]) -> Option<Vec<u8>> {
    let is_wave = &b[8..12] == b"WAVE";
    if !is_wave && &b[8..12] != b"AVI " {
        return None; // WebP and other RIFF forms are not audio/video
    }
    let riff_end = (u32::from_le_bytes(b[4..8].try_into().ok()?) as usize).checked_add(8)?;
    if riff_end > b.len() || riff_end < 12 {
        return None;
    }
    // Data after the declared end is dropped, like appended image data.
    let mut out = b[..riff_end].to_vec();
    let mut pos = 12;
    while pos + 8 <= riff_end {
        let id: [u8; 4] = b[pos..pos + 4].try_into().ok()?;
        let size = u32::from_le_bytes(b[pos + 4..pos + 8].try_into().ok()?) as usize;
        let payload = pos + 8;
        let end = payload.checked_add(size)?;
        if end > riff_end {
            return None;
        }
        let drop = if is_wave {
            !matches!(&id, b"fmt " | b"data" | b"fact")
        } else {
            match &id {
                b"LIST" => b.get(payload..payload + 4) == Some(b"INFO"),
                b"JUNK" | b"id3 " | b"ID3 " | b"iXML" | b"_PMX" | b"XMP_" => true,
                _ => false,
            }
        };
        if drop {
            out[pos..pos + 4].copy_from_slice(b"JUNK");
            out[payload..end].fill(0);
        }
        pos = end + (size & 1);
    }
    Some(out)
}

// ---------------------------------------------------------------------------
// MP3 / AAC, FLAC
// ---------------------------------------------------------------------------

/// Length of the ID3v2 tag(s) at the start of `b` (0 when there is none).
fn id3v2_len(b: &[u8]) -> Option<usize> {
    let mut pos = 0;
    while b[pos..].starts_with(b"ID3") {
        let h = b.get(pos..pos + 10)?;
        if h[6..10].iter().any(|v| v & 0x80 != 0) {
            return None;
        }
        let size = h[6..10].iter().fold(0usize, |a, v| (a << 7) | *v as usize);
        let footer = if h[5] & 0x10 != 0 { 10 } else { 0 };
        pos = pos.checked_add(10 + size + footer)?;
        if pos > b.len() {
            return None;
        }
    }
    Some(pos)
}

fn strip_mpeg_audio(b: &[u8]) -> Option<Vec<u8>> {
    // Padding between the tag and the first frame.
    let start = b.iter().position(|v| *v != 0)?;
    let mut end = b.len();
    if !(b[start] == 0xFF && b.get(start + 1)? & 0xE0 == 0xE0) {
        return None;
    }
    loop {
        let t = &b[start..end];
        if t.ends_with(b"LYRICSEND") {
            return None; // Lyrics3 v1 has no length to cut by
        } else if t.len() >= 128 && &t[t.len() - 128..t.len() - 125] == b"TAG" {
            end -= 128;
        } else if t.len() >= 21 && t.ends_with(b"LYRICS200") {
            let digits = t.get(t.len() - 21..t.len() - 15)?;
            let n: usize = std::str::from_utf8(digits).ok()?.parse().ok()?;
            end = end.checked_sub(n + 15)?;
        } else if t.len() >= 32 && &t[t.len() - 32..t.len() - 24] == b"APETAGEX" {
            let f = &t[t.len() - 32..];
            let size = u32::from_le_bytes(f[12..16].try_into().ok()?) as usize;
            let has_header = u32::from_le_bytes(f[20..24].try_into().ok()?) & 0x8000_0000 != 0;
            end = end.checked_sub(size + if has_header { 32 } else { 0 })?;
        } else {
            break;
        }
        if end <= start {
            return None;
        }
    }
    Some(b[start..end].to_vec())
}

/// Keeps STREAMINFO and SEEKTABLE, drops comments, pictures, application
/// blocks, cue sheets and padding.
fn strip_flac(b: &[u8]) -> Option<Vec<u8>> {
    let mut kept: Vec<&[u8]> = Vec::new();
    let mut pos = 4;
    loop {
        let h = b.get(pos..pos + 4)?;
        let len = ((h[1] as usize) << 16) | ((h[2] as usize) << 8) | h[3] as usize;
        let end = pos.checked_add(4 + len)?;
        if end > b.len() {
            return None;
        }
        if matches!(h[0] & 0x7F, 0 | 3) {
            kept.push(&b[pos..end]);
        }
        pos = end;
        if h[0] & 0x80 != 0 {
            break;
        }
    }
    if kept.first()?[0] & 0x7F != 0 {
        return None; // STREAMINFO must come first
    }
    let mut out = Vec::with_capacity(b.len());
    out.extend_from_slice(b"fLaC");
    let last = kept.len() - 1;
    for (i, blk) in kept.iter().enumerate() {
        let mut first = blk[0] & 0x7F;
        if i == last {
            first |= 0x80;
        }
        out.push(first);
        out.extend_from_slice(&blk[1..]);
    }
    out.extend_from_slice(&b[pos..]);
    Some(out)
}

// ---------------------------------------------------------------------------
// Ogg (Vorbis, Opus, Theora)
// ---------------------------------------------------------------------------

fn ogg_crc(data: &[u8]) -> u32 {
    let mut crc = 0u32;
    for &byte in data {
        crc ^= (byte as u32) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 {
                (crc << 1) ^ 0x04C1_1DB7
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// An empty comment header of the codec `pkt` starts as; None for codecs
/// without a known layout (Speex, FLAC-in-Ogg).
fn empty_comment_packet(pkt: &[u8]) -> Option<Vec<u8>> {
    let (magic, framing): (&[u8], bool) = if pkt.starts_with(b"OpusTags") {
        (b"OpusTags", false)
    } else if pkt.starts_with(b"\x03vorbis") {
        (b"\x03vorbis", true)
    } else if pkt.starts_with(b"\x81theora") {
        (b"\x81theora", false)
    } else {
        return None;
    };
    let mut v = magic.to_vec();
    v.extend_from_slice(&[0; 8]); // empty vendor string, zero comments
    if framing {
        v.push(1);
    }
    Some(v)
}

/// Replaces the comment header (the second packet of the stream) with an
/// empty one. The packet must lie wholly inside one page, which is how every
/// common muxer writes it; the page's lacing table and checksum are rebuilt.
fn strip_ogg(b: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(b.len());
    let mut pos = 0;
    let mut packet = 0usize; // index of the packet in progress
    let mut serial: Option<[u8; 4]> = None;
    let mut done = false;
    while pos < b.len() {
        let h = b.get(pos..pos + 27)?;
        if &h[..4] != b"OggS" || h[4] != 0 {
            return None;
        }
        let nseg = h[26] as usize;
        let table = b.get(pos + 27..pos + 27 + nseg)?;
        let data_start = pos + 27 + nseg;
        let data_len: usize = table.iter().map(|v| *v as usize).sum();
        let page_end = data_start.checked_add(data_len)?;
        if page_end > b.len() {
            return None;
        }
        let this_serial: [u8; 4] = h[14..18].try_into().ok()?;
        if *serial.get_or_insert(this_serial) != this_serial {
            return None; // multiplexed or chained streams
        }
        let continued = h[5] & 1 != 0;

        // Locate the comment packet in this page.
        let mut target = None; // (first segment, last segment) of packet 1
        let mut seg_start = 0;
        let mut cur = packet;
        for (i, lace) in table.iter().enumerate() {
            if *lace < 255 {
                if cur == 1 && !done {
                    // A continued page's first packet began on the previous page.
                    if seg_start == 0 && continued {
                        return None;
                    }
                    target = Some((seg_start, i));
                }
                cur += 1;
                seg_start = i + 1;
            }
        }
        if cur == 1 && !done && seg_start < nseg {
            return None; // the comment packet runs onto the next page
        }
        packet = cur;

        let Some((s, e)) = target else {
            out.extend_from_slice(&b[pos..page_end]);
            pos = page_end;
            continue;
        };
        done = true;
        let before: usize = table[..s].iter().map(|v| *v as usize).sum();
        let old: usize = table[s..=e].iter().map(|v| *v as usize).sum();
        let new_pkt = empty_comment_packet(&b[data_start + before..data_start + before + old])?;
        let mut lacing: Vec<u8> = table[..s].to_vec();
        lacing.extend(std::iter::repeat_n(255u8, new_pkt.len() / 255));
        lacing.push((new_pkt.len() % 255) as u8);
        lacing.extend_from_slice(&table[e + 1..]);
        if lacing.len() > 255 {
            return None;
        }
        let page_start = out.len();
        out.extend_from_slice(&h[..22]);
        out.extend_from_slice(&[0; 4]); // checksum, filled below
        out.push(lacing.len() as u8);
        out.extend_from_slice(&lacing);
        out.extend_from_slice(&b[data_start..data_start + before]);
        out.extend_from_slice(&new_pkt);
        out.extend_from_slice(&b[data_start + before + old..page_end]);
        let crc = ogg_crc(&out[page_start..]);
        out[page_start + 22..page_start + 26].copy_from_slice(&crc.to_le_bytes());
        pos = page_end;
    }
    done.then_some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn iso_box(ty: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut v = ((payload.len() + 8) as u32).to_be_bytes().to_vec();
        v.extend_from_slice(ty);
        v.extend_from_slice(payload);
        v
    }

    fn has(hay: &[u8], needle: &[u8]) -> bool {
        hay.windows(needle.len()).any(|w| w == needle)
    }

    #[test]
    fn mp4_location_atoms_are_blanked_without_moving_anything() {
        let udta = iso_box(b"udta", &iso_box(b"\xA9xyz", b"+48.8584+002.2945/"));
        let trak = iso_box(
            b"trak",
            &[iso_box(b"tkhd", &[1; 8]), iso_box(b"udta", b"SECRET")].concat(),
        );
        let moov = iso_box(b"moov", &[iso_box(b"mvhd", &[2; 20]), udta, trak].concat());
        let mdat = iso_box(b"mdat", b"MEDIAMEDIA");
        let file = [iso_box(b"ftyp", b"isom\0\0\0\0isom"), moov, mdat].concat();
        let out = strip_av_for_upload(&file).unwrap();
        assert_eq!(out.len(), file.len());
        assert!(!has(&out, b"+48.8584"));
        assert!(!has(&out, b"SECRET"));
        assert!(has(&out, b"MEDIAMEDIA"));
        assert!(has(&out, &[2; 20]));
    }

    #[test]
    fn mp4_without_moov_is_refused() {
        let file = [
            iso_box(b"ftyp", b"isom\0\0\0\0isom"),
            iso_box(b"mdat", b"x"),
        ]
        .concat();
        assert!(strip_av_for_upload(&file).is_none());
    }

    #[test]
    fn truncated_mp4_is_refused() {
        let mut file = [
            iso_box(b"ftyp", b"isom\0\0\0\0isom"),
            iso_box(b"moov", &[0; 16]),
        ]
        .concat();
        file.truncate(file.len() - 3);
        assert!(strip_av_for_upload(&file).is_none());
    }

    fn ebml_elem(id: &[u8], payload: &[u8]) -> Vec<u8> {
        let mut v = id.to_vec();
        v.push(0x80 | payload.len() as u8);
        v.extend_from_slice(payload);
        v
    }

    #[test]
    fn webm_tags_and_title_are_voided() {
        let info = ebml_elem(
            &[0x15, 0x49, 0xA9, 0x66],
            &ebml_elem(&[0x7B, 0xA9], b"MyTitle"),
        );
        let tags = ebml_elem(&[0x12, 0x54, 0xC3, 0x67], b"GPSTAG");
        let cluster = ebml_elem(&[0x1F, 0x43, 0xB6, 0x75], b"FRAMES");
        let segment = ebml_elem(&[0x18, 0x53, 0x80, 0x67], &[info, tags, cluster].concat());
        let file = [ebml_elem(&[0x1A, 0x45, 0xDF, 0xA3], b"webm"), segment].concat();
        let out = strip_av_for_upload(&file).unwrap();
        assert_eq!(out.len(), file.len());
        assert!(!has(&out, b"MyTitle"));
        assert!(!has(&out, b"GPSTAG"));
        assert!(has(&out, b"FRAMES"));
    }

    #[test]
    fn wav_extra_chunks_become_junk() {
        let chunk = |id: &[u8; 4], d: &[u8]| {
            let mut v = id.to_vec();
            v.extend_from_slice(&(d.len() as u32).to_le_bytes());
            v.extend_from_slice(d);
            if d.len() % 2 == 1 {
                v.push(0);
            }
            v
        };
        let body = [
            b"WAVE".to_vec(),
            chunk(b"fmt ", &[1; 16]),
            chunk(b"LIST", b"INFOIART\x04\0\0\0Bob\0"),
            chunk(b"data", b"PCMPCM"),
        ]
        .concat();
        let mut file = b"RIFF".to_vec();
        file.extend_from_slice(&(body.len() as u32).to_le_bytes());
        file.extend_from_slice(&body);
        let out = strip_av_for_upload(&file).unwrap();
        assert_eq!(out.len(), file.len());
        assert!(!has(&out, b"IART"));
        assert!(has(&out, b"PCMPCM"));
    }

    #[test]
    fn mp3_tags_are_cut_from_both_ends() {
        let mut file = b"ID3\x03\0\0\0\0\0\x0A".to_vec();
        file.extend_from_slice(b"SECRETSECR");
        file.extend_from_slice(&[0xFF, 0xFB, 0x90, 0x00, 1, 2, 3, 4]);
        let mut v1 = vec![0u8; 128];
        v1[..3].copy_from_slice(b"TAG");
        v1[3..9].copy_from_slice(b"SECRET");
        file.extend_from_slice(&v1);
        let out = strip_av_for_upload(&file).unwrap();
        assert_eq!(&*out, &[0xFF, 0xFB, 0x90, 0x00, 1, 2, 3, 4]);
    }

    #[test]
    fn unknown_audio_is_refused() {
        assert!(strip_av_for_upload(b"just some bytes").is_none());
        assert!(strip_av_for_upload(&[]).is_none());
    }

    #[test]
    fn flac_comment_and_picture_blocks_are_dropped() {
        let block = |ty: u8, last: bool, d: &[u8]| {
            let mut v = vec![ty | if last { 0x80 } else { 0 }, 0, 0, d.len() as u8];
            v.extend_from_slice(d);
            v
        };
        let file = [
            b"fLaC".to_vec(),
            block(0, false, &[7; 34]),
            block(4, false, b"ARTIST=Bob"),
            block(6, true, b"COVERART"),
            b"FRAMES".to_vec(),
        ]
        .concat();
        let out = strip_av_for_upload(&file).unwrap();
        assert!(!has(&out, b"ARTIST"));
        assert!(!has(&out, b"COVERART"));
        assert_eq!(out[4], 0x80, "STREAMINFO becomes the last block");
        assert!(out.ends_with(b"FRAMES"));
    }

    fn ogg_page_raw(seq: u32, flags: u8, table: &[u8], data: &[u8]) -> Vec<u8> {
        let mut v = b"OggS\0".to_vec();
        v.push(flags);
        v.extend_from_slice(&0u64.to_le_bytes());
        v.extend_from_slice(&[9, 9, 9, 9]);
        v.extend_from_slice(&seq.to_le_bytes());
        v.extend_from_slice(&[0; 4]);
        v.push(table.len() as u8);
        v.extend_from_slice(table);
        v.extend_from_slice(data);
        let crc = ogg_crc(&v);
        v[22..26].copy_from_slice(&crc.to_le_bytes());
        v
    }

    fn ogg_page(seq: u32, flags: u8, packets: &[&[u8]]) -> Vec<u8> {
        let mut table = Vec::new();
        let mut data = Vec::new();
        for p in packets {
            table.extend(std::iter::repeat_n(255u8, p.len() / 255));
            table.push((p.len() % 255) as u8);
            data.extend_from_slice(p);
        }
        ogg_page_raw(seq, flags, &table, &data)
    }

    #[test]
    fn opus_tags_are_replaced_and_pages_stay_valid() {
        let mut tags = b"OpusTags".to_vec();
        tags.extend_from_slice(&5u32.to_le_bytes());
        tags.extend_from_slice(b"SECRE");
        tags.extend_from_slice(&0u32.to_le_bytes());
        let file = [
            ogg_page(0, 2, &[b"OpusHead0000000000"]),
            ogg_page(1, 0, &[&tags]),
            ogg_page(2, 0, &[b"AUDIOAUDIO"]),
        ]
        .concat();
        let out = strip_av_for_upload(&file).unwrap();
        assert!(!has(&out, b"SECRE"));
        assert!(has(&out, b"AUDIOAUDIO"));
        // Every page's stored checksum matches its content.
        let mut pos = 0;
        while pos < out.len() {
            let nseg = out[pos + 26] as usize;
            let len: usize = out[pos + 27..pos + 27 + nseg]
                .iter()
                .map(|v| *v as usize)
                .sum();
            let end = pos + 27 + nseg + len;
            let mut page = out[pos..end].to_vec();
            let stored = u32::from_le_bytes(page[22..26].try_into().unwrap());
            page[22..26].fill(0);
            assert_eq!(ogg_crc(&page), stored);
            pos = end;
        }
    }

    #[test]
    fn vorbis_comment_sharing_a_page_with_setup_is_replaced() {
        let mut comment = b"\x03vorbis".to_vec();
        comment.extend_from_slice(&6u32.to_le_bytes());
        comment.extend_from_slice(b"SECRET");
        comment.extend_from_slice(&0u32.to_le_bytes());
        comment.push(1);
        let file = [
            ogg_page(0, 2, &[b"\x01vorbis-ident"]),
            ogg_page(1, 0, &[&comment, b"\x05vorbis-setup"]),
        ]
        .concat();
        let out = strip_av_for_upload(&file).unwrap();
        assert!(!has(&out, b"SECRET"));
        assert!(has(&out, b"\x05vorbis-setup"));
    }

    #[test]
    fn ogg_with_comment_spanning_pages_is_refused() {
        let mut tags = b"OpusTags".to_vec();
        tags.extend_from_slice(&[b'x'; 300]);
        let file = [
            ogg_page(0, 2, &[b"OpusHead"]),
            ogg_page_raw(1, 0, &[255], &tags[..255]),
            ogg_page(2, 1, &[&tags[255..]]),
        ]
        .concat();
        assert!(strip_av_for_upload(&file).is_none());
    }
}
