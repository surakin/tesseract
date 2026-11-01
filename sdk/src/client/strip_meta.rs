//! Lossless removal of privacy-sensitive metadata (EXIF/GPS, XMP, IPTC,
//! text comments) from JPEG, PNG, WebP, GIF, TIFF, JPEG 2000, SVG, JPEG XL and
//! HEIF/AVIF/HEIC uploads (BMP has no metadata fields). Pixel
//! data and ICC profiles are untouched; only the EXIF Orientation tag is kept
//! (JPEG/PNG/WebP) so photos still display upright. The format is recognised
//! from the bytes first, so a wrong or generic declared mime cannot skip the
//! strip. Anything unparseable is returned unchanged.

use img_parts::jpeg::{markers, Jpeg, JpegSegment};
use img_parts::png::Png;
use img_parts::riff::{RiffChunk, RiffContent};
use img_parts::webp::WebP;
use img_parts::{Bytes, ImageEXIF};
use little_exif::exif_tag::ExifTag;
use little_exif::filetype::FileExtension;
use little_exif::metadata::Metadata;
use std::borrow::Cow;

const ICC_PREFIX: &[u8] = b"ICC_PROFILE\0";
/// PNG chunks that describe the pixels. Everything else (text, time, EXIF/XMP,
/// C2PA `caBX`, vendor chunks) is dropped; the orientation is rewritten as a
/// minimal `eXIf` chunk afterwards.
const PNG_KEEP_CHUNKS: [[u8; 4]; 19] = [
    *b"IHDR", *b"PLTE", *b"IDAT", *b"IEND", *b"tRNS", *b"cHRM", *b"gAMA", *b"iCCP", *b"sBIT",
    *b"sRGB", *b"cICP", *b"mDCV", *b"cLLI", *b"pHYs", *b"bKGD", *b"hIST", *b"acTL", *b"fcTL",
    *b"fdAT",
];
/// WebP chunks that describe the pixels (EXIF, XMP and anything unknown go).
/// VP8X flag bits for the EXIF (0x08) and XMP (0x04) chunks.
const WEBP_VP8X_EXIF_XMP: u8 = 0x0C;
const WEBP_KEEP_CHUNKS: [[u8; 4]; 7] = [
    *b"VP8 ", *b"VP8L", *b"VP8X", *b"ALPH", *b"ANIM", *b"ANMF", *b"ICCP",
];

/// What the user chose to send, which decides how much of the file may be
/// rewritten. `Image` is a picture the user meant to share as such: every
/// recognised format is stripped. `File` is an arbitrary attachment: only the
/// photo containers cameras and phones produce are stripped (so a geotagged
/// JPEG sent as a file doesn't leak its GPS); TIFF/RAW, SVG, BMP, JPEG 2000 and
/// everything else are sent byte-identical, because rewriting them can destroy
/// data the recipient needs.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum StripScope {
    Image,
    File,
}

/// The single entry point every upload path uses.
pub(super) fn strip_for_upload<'a>(
    bytes: &'a [u8],
    mime: &str,
    scope: StripScope,
) -> Cow<'a, [u8]> {
    if scope == StripScope::File && !is_photo_container(bytes) {
        return Cow::Borrowed(bytes);
    }
    strip_image_metadata(bytes, mime)
}

pub(super) fn strip_image_metadata<'a>(bytes: &'a [u8], mime: &str) -> Cow<'a, [u8]> {
    strip_checked(bytes, mime).unwrap_or(Cow::Borrowed(bytes))
}

/// JPEG, PNG, WebP, GIF, HEIF/AVIF/HEIC and JPEG XL, recognised from the
/// bytes alone (never from the declared mime).
fn is_photo_container(b: &[u8]) -> bool {
    b.starts_with(&[0xFF, 0xD8, 0xFF])
        || b.starts_with(b"\x89PNG\r\n\x1a\n")
        || (b.len() >= 12 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP")
        || b.starts_with(b"GIF8")
        || is_heif(b)
        || is_jxl_container(b)
        || b.starts_with(&[0xFF, 0x0A])
}

/// True when the metadata of `bytes` can be (and, via `strip_image_metadata`,
/// is) removed with confidence: the format is recognised and parsed, or is
/// known to carry no metadata at all. False for formats without a stripper and
/// for files that fail to parse; callers then must not upload the original.
pub fn is_strippable(bytes: &[u8], mime: &str) -> bool {
    strip_checked(bytes, mime).is_some()
}

/// The stripped bytes (borrowed when there was nothing to remove), or None
/// when the file cannot be confidently stripped. A parser panic is None too:
/// it must not abort the process across the FFI boundary, and the fuzz-style
/// tests below keep that path from being reached in practice.
fn strip_checked<'a>(bytes: &'a [u8], mime: &str) -> Option<Cow<'a, [u8]>> {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| strip_inner(bytes, mime)))
        .ok()
        .flatten()
}

/// Formats with no metadata fields, as the length of their meaningful bytes
/// (anything after it is dropped, like data appended after an image's end):
/// BMP (its declared file size) and a bare JPEG XL codestream (its orientation
/// is in the pixel header; it has no end marker, so the file is taken as is).
fn metadata_free_len(b: &[u8]) -> Option<usize> {
    if b.starts_with(&[0xFF, 0x0A]) {
        return Some(b.len());
    }
    let bmp = b.len() >= 26
        && b.starts_with(b"BM")
        && matches!(
            u32::from_le_bytes(b[14..18].try_into().unwrap_or([0; 4])),
            12 | 40 | 52 | 56 | 64 | 108 | 124
        );
    if !bmp {
        return None;
    }
    match u32::from_le_bytes(b[2..6].try_into().unwrap_or([0; 4])) as usize {
        0 => Some(b.len()),
        n if n >= 26 => Some(n.min(b.len())),
        _ => None, // nonsense declared size
    }
}

fn strip_inner<'a>(bytes: &'a [u8], mime: &str) -> Option<Cow<'a, [u8]>> {
    // Dispatch on the bytes. A recognised format that fails to parse is None;
    // it does not fall through to the generic JPEG/PNG/WebP handling.
    let parsed = if bytes.starts_with(b"GIF8") {
        Some(strip_gif(bytes))
    } else if is_jxl_container(bytes) {
        Some(strip_jxl(bytes))
    } else if is_jp2(bytes) {
        Some(strip_jp2(bytes))
    } else if is_heif(bytes) {
        Some(strip_heif(bytes))
    } else if is_tiff(bytes) {
        Some(strip_tiff(bytes))
    } else if is_svg(bytes) {
        Some(strip_svg(bytes))
    } else {
        None
    };
    if let Some(result) = parsed {
        return result.map(Cow::Owned);
    }
    if let Some(n) = metadata_free_len(bytes) {
        return Some(Cow::Borrowed(&bytes[..n]));
    }
    // Contents first, declared mime only as a fallback.
    let ext = if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        FileExtension::JPEG
    } else if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        FileExtension::PNG {
            as_zTXt_chunk: false,
        }
    } else if bytes.len() >= 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        FileExtension::WEBP
    } else {
        match mime {
            "image/jpeg" | "image/jpg" => FileExtension::JPEG,
            "image/png" => FileExtension::PNG {
                as_zTXt_chunk: false,
            },
            "image/webp" => FileExtension::WEBP,
            _ => return None,
        }
    };
    strip(bytes, &ext).map(Cow::Owned)
}

fn strip(bytes: &[u8], ext: &FileExtension) -> Option<Vec<u8>> {
    // PNG keeps its orientation itself (strip_png writes a standard `eXIf`
    // chunk); little_exif could only write it as a non-standard zTXt block.
    let orientation = match ext {
        FileExtension::PNG { .. } => None,
        _ => read_orientation(bytes, ext),
    };
    let mut out = match ext {
        FileExtension::JPEG => strip_jpeg(bytes)?,
        FileExtension::PNG { .. } => strip_png(bytes)?,
        _ => strip_webp(bytes)?,
    };
    if let Some(o) = orientation {
        let mut md = Metadata::new();
        md.set_tag(ExifTag::Orientation(vec![o]));
        md.write_to_vec(&mut out, ext.clone()).ok()?;
    }
    Some(out)
}

/// Orientation tag (2..=8); None when absent, upright (1) or unreadable.
fn read_orientation(bytes: &[u8], ext: &FileExtension) -> Option<u16> {
    let md = Metadata::new_from_vec(&bytes.to_vec(), ext.clone()).ok()?;
    md.get_tag(&ExifTag::Orientation(Vec::new()))
        .find_map(|t| match t {
            ExifTag::Orientation(v) => v.first().copied(),
            _ => None,
        })
        .filter(|o| (2..=8).contains(o))
}

fn strip_jpeg(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut jpeg = Jpeg::from_bytes(Bytes::copy_from_slice(bytes)).ok()?;
    jpeg.segments_mut().retain(|s| match s.marker() {
        // JFXX carries an extension thumbnail: a preview of the uncropped original.
        markers::APP0 => !s.contents().starts_with(b"JFXX\0"),
        markers::APP1 | markers::APP3..=markers::APP13 | markers::APP15 | markers::COM => false,
        markers::APP2 => s.contents().starts_with(ICC_PREFIX),
        _ => true,
    });
    // Anything after the end-of-image marker (appended originals, motion-photo
    // video, secondary pictures) is not part of the picture: drop it.
    if let Some(eoi) = jpeg
        .segments()
        .iter()
        .position(|s| s.marker() == markers::EOI)
    {
        jpeg.segments_mut().truncate(eoi + 1);
    }
    trim_after_eoi(&mut jpeg);
    // Keep JFIF (it sets the pixel density) but drop its embedded thumbnail.
    for seg in jpeg.segments_mut().iter_mut() {
        if seg.marker() == markers::APP0 {
            if let Some(clean) = jfif_without_thumbnail(seg.contents()) {
                *seg = JpegSegment::new_with_contents(markers::APP0, Bytes::from(clean));
            }
        }
    }
    Some(jpeg.encoder().bytes().to_vec())
}

/// img_parts keeps everything after a scan header as that segment's entropy
/// data, so the EOI marker and any appended bytes live inside it. In entropy
/// data `FF 00` is stuffing and `FF D0..D7` are restarts, so the first `FF D9`
/// is the real end of image: cut there and drop any later segments.
fn trim_after_eoi(jpeg: &mut Jpeg) {
    let segs = jpeg.segments_mut();
    for i in 0..segs.len() {
        if !segs[i].has_entropy() {
            continue;
        }
        let enc = segs[i].clone().encoder().bytes().to_vec();
        let start = 4 + segs[i].contents().len(); // marker(2) + length(2) + header
        let Some(rel) = enc
            .get(start..)
            .and_then(|e| e.windows(2).position(|w| w == [0xFF, 0xD9]))
        else {
            continue;
        };
        let entropy = Bytes::copy_from_slice(&enc[start..start + rel + 2]);
        let keep = (segs[i].marker(), Bytes::copy_from_slice(segs[i].contents()));
        segs[i] = JpegSegment::new_with_entropy(keep.0, keep.1, entropy);
        segs.truncate(i + 1);
        return;
    }
}

/// `JFIF\0` + version(2) units(1) density(4) + thumbnail w/h(2) [+ RGB data]:
/// the 14-byte header with zero thumbnail dimensions, or None when there is no
/// thumbnail to remove (or this is not a JFIF segment).
fn jfif_without_thumbnail(c: &[u8]) -> Option<Vec<u8>> {
    if !c.starts_with(b"JFIF\0") || c.len() < 14 || (c.len() == 14 && c[12] == 0 && c[13] == 0) {
        return None;
    }
    let mut v = c[..14].to_vec();
    v[12] = 0;
    v[13] = 0;
    Some(v)
}

fn strip_png(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut png = Png::from_bytes(Bytes::copy_from_slice(bytes)).ok()?;
    // Keep only the orientation, rewritten as a minimal standard `eXIf` chunk.
    let orientation = png.exif().and_then(|e| tiff_orientation(&e));
    png.chunks_mut()
        .retain(|c| PNG_KEEP_CHUNKS.contains(&c.kind()));
    png.set_exif(None);
    if let Some(o) = orientation {
        png.set_exif(Some(Bytes::from(minimal_exif_orientation(o))));
    }
    Some(png.encoder().bytes().to_vec())
}

/// Orientation (2..=8) from raw EXIF/TIFF bytes (no `Exif\0\0` prefix).
fn tiff_orientation(e: &[u8]) -> Option<u16> {
    let le = match e.get(..4)? {
        b"II*\0" => true,
        b"MM\0*" => false,
        _ => return None,
    };
    let u16_at = |at: usize| -> Option<u16> {
        let v: [u8; 2] = e.get(at..at + 2)?.try_into().ok()?;
        Some(if le {
            u16::from_le_bytes(v)
        } else {
            u16::from_be_bytes(v)
        })
    };
    let ifd_bytes: [u8; 4] = e.get(4..8)?.try_into().ok()?;
    let ifd = if le {
        u32::from_le_bytes(ifd_bytes)
    } else {
        u32::from_be_bytes(ifd_bytes)
    } as usize;
    (0..u16_at(ifd)? as usize)
        .map(|i| ifd + 2 + 12 * i)
        .find(|&at| u16_at(at) == Some(274))
        .and_then(|at| u16_at(at + 8))
        .filter(|o| (2..=8).contains(o))
}

/// A one-entry big-endian EXIF block holding only the Orientation tag.
fn minimal_exif_orientation(o: u16) -> Vec<u8> {
    let mut v = b"MM\0*\0\0\0\x08".to_vec(); // header, IFD0 at 8
    v.extend_from_slice(&[0, 1]); // one entry
    v.extend_from_slice(&[0x01, 0x12, 0, 3, 0, 0, 0, 1]); // tag 274, SHORT, count 1
    v.extend_from_slice(&o.to_be_bytes());
    v.extend_from_slice(&[0, 0]); // value padding
    v.extend_from_slice(&[0, 0, 0, 0]); // no next IFD
    v
}

fn strip_webp(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut webp = WebP::from_bytes(Bytes::copy_from_slice(bytes)).ok()?;
    // Keeps only the chunks that describe the pixels, which drops EXIF, XMP
    // and anything private.
    webp.chunks_mut()
        .retain(|c| WEBP_KEEP_CHUNKS.contains(&c.id()));
    // The VP8X header says which of those exist (ICC, alpha, animation, EXIF,
    // XMP). Clear only the EXIF and XMP bits. img-parts' own `set_exif(None)`
    // is not usable here: it re-infers the container kind and, when there is
    // no ICC/EXIF chunk, deletes VP8X outright, turning every animated or
    // alpha WebP into an invalid file.
    for chunk in webp.chunks_mut().iter_mut() {
        if chunk.id() != *b"VP8X" {
            continue;
        }
        let data = chunk.content().data().filter(|d| d.len() >= 10)?;
        let mut flags = data.to_vec();
        flags[0] &= !WEBP_VP8X_EXIF_XMP;
        *chunk = RiffChunk::new(*b"VP8X", RiffContent::Data(Bytes::from(flags)));
    }
    Some(webp.encoder().bytes().to_vec())
}

// ---- GIF -------------------------------------------------------------------

/// Drops comment extensions and application extensions other than the
/// animation loop (`NETSCAPE2.0`/`ANIMEXTS1.0`) and ICC (`ICCRGBG1012`) ones,
/// which is where XMP lives. Frames and timing are copied verbatim.
fn strip_gif(b: &[u8]) -> Option<Vec<u8>> {
    if b.len() < 13 {
        return None;
    }
    let mut pos = 13usize;
    if b[10] & 0x80 != 0 {
        pos += 3usize << ((b[10] & 7) + 1);
    }
    let mut out = b.get(..pos)?.to_vec();
    while pos < b.len() {
        match b[pos] {
            0x3B => {
                out.push(0x3B);
                return Some(out);
            }
            0x21 => {
                let label = *b.get(pos + 1)?;
                let end = skip_gif_sub_blocks(b, pos + 2)?;
                let keep = match label {
                    0xFE => false,
                    0xFF => {
                        let id = b.get(pos + 3..pos + 14)?;
                        id == b"NETSCAPE2.0" || id == b"ANIMEXTS1.0" || id == b"ICCRGBG1012"
                    }
                    _ => true,
                };
                if keep {
                    out.extend_from_slice(&b[pos..end]);
                }
                pos = end;
            }
            0x2C => {
                let packed = *b.get(pos + 9)?;
                let mut p = pos + 10;
                if packed & 0x80 != 0 {
                    p += 3usize << ((packed & 7) + 1);
                }
                let end = skip_gif_sub_blocks(b, p + 1)?; // +1: LZW minimum code size
                out.extend_from_slice(b.get(pos..end)?);
                pos = end;
            }
            _ => return None,
        }
    }
    None // no trailer: truncated, leave it alone
}

/// Position just past a chain of GIF data sub-blocks starting at `pos`.
fn skip_gif_sub_blocks(b: &[u8], mut pos: usize) -> Option<usize> {
    loop {
        let len = *b.get(pos)? as usize;
        if len == 0 {
            return Some(pos + 1);
        }
        pos += 1 + len;
    }
}

// ---- ISO base media (JPEG XL container, AVIF) ------------------------------

/// One box: type, start of the box, start of its payload, end of the box.
pub(super) struct IsoBox {
    pub(super) ty: [u8; 4],
    pub(super) start: usize,
    pub(super) payload: usize,
    pub(super) end: usize,
}

fn be16(b: &[u8], at: usize) -> Option<usize> {
    Some(u16::from_be_bytes(b.get(at..at + 2)?.try_into().ok()?) as usize)
}
fn be32(b: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_be_bytes(b.get(at..at + 4)?.try_into().ok()?) as usize)
}
fn be_n(b: &[u8], at: usize, n: usize) -> Option<usize> {
    let mut v = 0usize;
    for i in 0..n {
        v = v.checked_shl(8)? | *b.get(at + i)? as usize;
    }
    Some(v)
}

/// Child boxes of `[start, end)`. None when the layout is malformed.
pub(super) fn iso_boxes(b: &[u8], start: usize, end: usize) -> Option<Vec<IsoBox>> {
    let mut v = Vec::new();
    let mut pos = start;
    while pos + 8 <= end {
        let size = be32(b, pos)?;
        let ty: [u8; 4] = b.get(pos + 4..pos + 8)?.try_into().ok()?;
        let (payload, box_end) = match size {
            0 => (pos + 8, end),
            1 => {
                let big = u64::from_be_bytes(b.get(pos + 8..pos + 16)?.try_into().ok()?);
                (pos + 16, pos.checked_add(usize::try_from(big).ok()?)?)
            }
            n if n >= 8 => (pos + 8, pos.checked_add(n)?),
            _ => return None,
        };
        if box_end > end || payload > box_end {
            return None;
        }
        v.push(IsoBox {
            ty,
            start: pos,
            payload,
            end: box_end,
        });
        pos = box_end;
    }
    (pos == end).then_some(v)
}

fn is_jxl_container(b: &[u8]) -> bool {
    b.starts_with(b"\0\0\0\x0cJXL \r\n\x87\n")
}

/// Rebuilds an ISO-box file without the boxes `drop` selects.
fn drop_iso_boxes(b: &[u8], drop: impl Fn(&IsoBox) -> bool) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(b.len());
    for bx in iso_boxes(b, 0, b.len())? {
        if !drop(&bx) {
            out.extend_from_slice(&b[bx.start..bx.end]);
        }
    }
    Some(out)
}

/// Removes the `Exif`, `xml ` (XMP), `jumb` (JUMBF/C2PA) and `jbrd` (the
/// original JPEG's header data kept by lossless JPEG transcoding) boxes,
/// including Brotli-compressed `brob` wrappers around them. The codestream is untouched;
/// orientation lives in its header, so there is nothing to preserve. A bare
/// codestream carries no metadata and is left alone.
fn strip_jxl(b: &[u8]) -> Option<Vec<u8>> {
    drop_iso_boxes(b, |bx| {
        let is_meta = |t: &[u8]| t == b"Exif" || t == b"xml " || t == b"jumb" || t == b"jbrd";
        let inner = if &bx.ty == b"brob" {
            b.get(bx.payload..bx.payload + 4)
        } else {
            None
        };
        is_meta(&bx.ty) || inner.is_some_and(is_meta)
    })
}

fn is_jp2(b: &[u8]) -> bool {
    b.starts_with(b"\0\0\0\x0cjP  \r\n\x87\n")
}

/// JPEG 2000: drops the `xml ` boxes and every `uuid` box (XMP, EXIF and
/// GeoJP2 all live there) and the comment (`COM`) segments inside the
/// codestream. Colour (`colr`, incl. ICC) and the image data stay.
fn strip_jp2(b: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(b.len());
    for bx in iso_boxes(b, 0, b.len())? {
        if &bx.ty == b"xml " || &bx.ty == b"uuid" {
            continue;
        }
        if &bx.ty != b"jp2c" {
            out.extend_from_slice(&b[bx.start..bx.end]);
            continue;
        }
        let cs = strip_j2k_comments(&b[bx.payload..bx.end])?;
        // Rewrite the box header for the new payload length. A box that runs
        // to the end of the file (size 0) keeps saying so.
        if be32(b, bx.start)? == 0 {
            out.extend_from_slice(&b[bx.start..bx.payload]);
        } else if bx.payload - bx.start == 16 {
            out.extend_from_slice(&1u32.to_be_bytes());
            out.extend_from_slice(b"jp2c");
            out.extend_from_slice(&((16 + cs.len()) as u64).to_be_bytes());
        } else {
            out.extend_from_slice(&u32::try_from(8 + cs.len()).ok()?.to_be_bytes());
            out.extend_from_slice(b"jp2c");
        }
        out.extend_from_slice(&cs);
    }
    Some(out)
}

/// A JPEG 2000 codestream without its `COM` (0xFF64) marker segments. Comments
/// are allowed in the main header and in tile-part headers; the former are cut
/// out, but a tile-part header would need its `Psot` length rewritten, so a
/// comment there makes the file "not confidently clean" (None).
fn strip_j2k_comments(cs: &[u8]) -> Option<Vec<u8>> {
    const SOC: usize = 0xFF4F;
    const SOT: usize = 0xFF90;
    const SOD: usize = 0xFF93;
    const EOC: usize = 0xFFD9;
    const COM: usize = 0xFF64;
    if be16(cs, 0)? != SOC {
        return None;
    }
    let mut out = vec![0xFF, 0x4F];
    let mut pos = 2usize;
    // Main header: marker segments up to the first SOT.
    while be16(cs, pos)? != SOT {
        let marker = be16(cs, pos)?;
        if marker < 0xFF00 {
            return None;
        }
        let len = be16(cs, pos + 2)?;
        let end = pos
            .checked_add(2 + len)
            .filter(|e| len >= 2 && *e <= cs.len())?;
        if marker != COM {
            out.extend_from_slice(&cs[pos..end]);
        }
        pos = end;
    }
    let tiles_start = pos;
    // Tile-parts: only inspected, copied verbatim.
    while be16(cs, pos)? == SOT {
        let psot = be32(cs, pos + 6)?;
        let tile_end = if psot == 0 {
            cs.len()
        } else {
            pos.checked_add(psot)?
        };
        if tile_end > cs.len() || tile_end < pos + 14 {
            return None;
        }
        let mut h = pos + 12;
        while be16(cs, h)? != SOD {
            if be16(cs, h)? == COM {
                return None;
            }
            let len = be16(cs, h + 2)?;
            h = h.checked_add(2 + len).filter(|_| len >= 2)?;
            if h > tile_end {
                return None;
            }
        }
        pos = tile_end;
        if pos >= cs.len() {
            break;
        }
    }
    if pos < cs.len() && be16(cs, pos)? != EOC {
        return None;
    }
    out.extend_from_slice(&cs[tiles_start..]);
    Some(out)
}

/// HEIF family (AVIF, HEIC, HEIX, ...): an `ftyp` whose major or compatible
/// brands include one of the HEIF image brands.
fn is_heif(b: &[u8]) -> bool {
    const BRANDS: [&[u8; 4]; 10] = [
        b"avif", b"avis", b"heic", b"heix", b"hevc", b"hevx", b"heim", b"heis", b"mif1", b"msf1",
    ];
    if b.len() < 16 || &b[4..8] != b"ftyp" {
        return false;
    }
    let end = be32(b, 0).map_or(b.len(), |n| n.clamp(16, b.len()));
    (8..end)
        .step_by(4)
        .filter(|&at| at != 12) // 12..16 is the minor version, not a brand
        .any(|at| {
            b.get(at..at + 4)
                .is_some_and(|x| BRANDS.iter().any(|br| &br[..] == x))
        })
}

/// XMP packet carried in a `uuid` box (ISO/Adobe convention).
const XMP_UUID: [u8; 16] = [
    0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC,
];

/// Neutralises EXIF and XMP in HEIF/AVIF/HEIC files in place (items, XMP `uuid` boxes and `xml ` boxes): their payload bytes are zeroed and
/// the item type is renamed to `free`, so decoders skip them. Offsets and
/// lengths are unchanged, so every other reference in the file stays valid.
fn strip_heif(b: &[u8]) -> Option<Vec<u8>> {
    let top = iso_boxes(b, 0, b.len())?;
    let meta = top.iter().find(|x| &x.ty == b"meta")?;
    let kids = iso_boxes(b, meta.payload + 4, meta.end)?; // +4: full-box version/flags
    let iinf = kids.iter().find(|x| &x.ty == b"iinf")?;
    let idat = kids.iter().find(|x| &x.ty == b"idat");

    // Whole-box metadata: XMP `uuid` boxes at the top level, `xml ` in `meta`.
    let mut out = b.to_vec();
    for bx in top.iter().chain(kids.iter()) {
        let xmp_uuid =
            &bx.ty == b"uuid" && b.get(bx.payload..bx.payload + 16) == Some(&XMP_UUID[..]);
        if xmp_uuid || &bx.ty == b"xml " {
            out[bx.payload..bx.end].fill(0);
            out[bx.start + 4..bx.start + 8].copy_from_slice(b"free");
        }
    }

    // Items to scrub: (item id, absolute position of the item_type fourcc).
    let mut targets: Vec<(usize, usize)> = Vec::new();
    let version = *b.get(iinf.payload)?;
    let count_at = iinf.payload + 4;
    let first = if version == 0 {
        count_at + 2
    } else {
        count_at + 4
    };
    for infe in iso_boxes(b, first, iinf.end)? {
        if &infe.ty != b"infe" {
            continue;
        }
        let v = *b.get(infe.payload)?;
        if v < 2 {
            continue;
        }
        let (id, after_id) = if v == 2 {
            (be16(b, infe.payload + 4)?, infe.payload + 6)
        } else {
            (be32(b, infe.payload + 4)?, infe.payload + 8)
        };
        let ty_at = after_id + 2; // skip protection index
        let ty = b.get(ty_at..ty_at + 4)?;
        let scrub = if ty == b"Exif" {
            true
        } else if ty == b"mime" {
            // item_name\0 content_type\0
            let rest = b.get(ty_at + 4..infe.end)?;
            let name_end = rest.iter().position(|&c| c == 0)?;
            let ct = &rest[name_end + 1..];
            ct.starts_with(b"application/rdf+xml")
        } else {
            false
        };
        if scrub {
            targets.push((id, ty_at));
        }
    }
    if targets.is_empty() {
        return Some(out); // parsed fine; nothing (more) to scrub
    }

    for &(_, ty_at) in &targets {
        out[ty_at..ty_at + 4].copy_from_slice(b"free");
    }

    // Zero each target's extents via iloc.
    let iloc = kids.iter().find(|x| &x.ty == b"iloc")?;
    let v = *b.get(iloc.payload)?;
    let sizes = *b.get(iloc.payload + 4)?;
    let (offset_size, length_size) = ((sizes >> 4) as usize, (sizes & 15) as usize);
    let sizes2 = *b.get(iloc.payload + 5)?;
    let base_size = (sizes2 >> 4) as usize;
    let index_size = if v == 1 || v == 2 {
        (sizes2 & 15) as usize
    } else {
        0
    };
    // Extents that occupy no bytes would let a crafted file make the loops
    // below spin without consuming input; field widths above 8 are not valid.
    if (offset_size == 0 && length_size == 0)
        || offset_size > 8
        || length_size > 8
        || base_size > 8
        || index_size > 8
    {
        return None;
    }
    let (items, mut p) = if v < 2 {
        (be16(b, iloc.payload + 6)?, iloc.payload + 8)
    } else {
        (be32(b, iloc.payload + 6)?, iloc.payload + 10)
    };
    for _ in 0..items {
        let id = if v < 2 { be16(b, p)? } else { be32(b, p)? };
        p += if v < 2 { 2 } else { 4 };
        let method = if v == 1 || v == 2 {
            let m = be16(b, p)? & 0xF;
            p += 2;
            m
        } else {
            0
        };
        p += 2; // data_reference_index
        let base = be_n(b, p, base_size)?;
        p += base_size;
        let extents = be16(b, p)?;
        p += 2;
        for _ in 0..extents {
            p += index_size;
            let off = be_n(b, p, offset_size)?;
            p += offset_size;
            let len = be_n(b, p, length_size)?;
            p += length_size;
            if !targets.iter().any(|&(t, _)| t == id) {
                continue;
            }
            let origin = match method {
                0 => 0,
                1 => idat?.payload, // construction_method 1: offsets are into idat
                _ => continue,
            };
            let from = origin.checked_add(base)?.checked_add(off)?;
            let to = from.checked_add(len)?;
            out.get_mut(from..to)?.fill(0);
        }
    }
    Some(out)
}

// ---- TIFF -------------------------------------------------------------------

fn is_tiff(b: &[u8]) -> bool {
    b.starts_with(b"II*\0") || b.starts_with(b"MM\0*")
}

/// Tags removed from every IFD: descriptive text, author/software/host, dates,
/// copyright, XMP, IPTC, Photoshop resources, the Exif and GPS sub-IFDs and
/// PrintIM. ICC profile (34675), Orientation (274), resolution and everything
/// that describes the pixels stay.
const TIFF_STRIP_TAGS: [u16; 17] = [
    269, 270, 271, 272, 285, 305, 306, 315, 316, 700, 33432, 33723, 34377, 34665, 34853, 37724,
    50341,
];
/// Tags whose value is an offset to another IFD that must be wiped too.
const TIFF_IFD_POINTERS: [u16; 3] = [34665, 34853, 40965];

struct Tiff<'a> {
    b: &'a mut [u8],
    le: bool,
    /// IFD offsets already wiped: a hostile file can point many entries at the
    /// same (or nested) sub-IFDs, which would otherwise fan out exponentially.
    wiped: std::collections::HashSet<usize>,
}

/// Upper bound on sub-IFDs wiped per file; real files have a handful.
const TIFF_MAX_SUB_IFDS: usize = 4096;

impl Tiff<'_> {
    fn u16(&self, at: usize) -> Option<u16> {
        let v: [u8; 2] = self.b.get(at..at + 2)?.try_into().ok()?;
        Some(if self.le {
            u16::from_le_bytes(v)
        } else {
            u16::from_be_bytes(v)
        })
    }
    fn u32(&self, at: usize) -> Option<u32> {
        let v: [u8; 4] = self.b.get(at..at + 4)?.try_into().ok()?;
        Some(if self.le {
            u32::from_le_bytes(v)
        } else {
            u32::from_be_bytes(v)
        })
    }
    fn put_u16(&mut self, at: usize, v: u16) -> Option<()> {
        let bytes = if self.le {
            v.to_le_bytes()
        } else {
            v.to_be_bytes()
        };
        self.b.get_mut(at..at + 2)?.copy_from_slice(&bytes);
        Some(())
    }
    fn zero(&mut self, at: usize, len: usize) -> Option<()> {
        self.b.get_mut(at..at.checked_add(len)?)?.fill(0);
        Some(())
    }

    /// Byte size of an entry's value: type size * count.
    fn value_len(&self, ty: u16, count: u32) -> usize {
        let unit = match ty {
            1 | 2 | 6 | 7 => 1, // BYTE ASCII SBYTE UNDEFINED
            3 | 8 => 2,
            4 | 9 | 11 | 13 => 4,
            5 | 10 | 12 | 16 | 17 | 18 => 8,
            _ => 1,
        };
        unit * count as usize
    }

    /// Wipes an IFD (its entries, their out-of-line values and any nested
    /// IFDs). `depth` bounds recursion on hostile files.
    fn wipe_ifd(&mut self, off: usize, depth: u32) -> Option<()> {
        if depth > 4 || off == 0 || !self.wiped.insert(off) {
            return Some(());
        }
        if self.wiped.len() > TIFF_MAX_SUB_IFDS {
            return None; // hostile fan-out: give up, file is left unchanged
        }
        let n = self.u16(off)? as usize;
        for i in 0..n {
            let e = off + 2 + 12 * i;
            let (tag, ty, count) = (self.u16(e)?, self.u16(e + 2)?, self.u32(e + 4)?);
            let len = self.value_len(ty, count);
            if len > 4 {
                let at = self.u32(e + 8)? as usize;
                self.zero(at, len).or(Some(()))?;
            }
            if TIFF_IFD_POINTERS.contains(&tag) && len <= 4 {
                let sub = self.u32(e + 8)? as usize;
                self.wipe_ifd(sub, depth + 1)?;
            }
        }
        self.zero(off, 2 + 12 * n + 4)
    }

    /// Removes the unwanted entries of the IFD at `off` in place and returns
    /// (next IFD offset, whether anything was removed).
    fn clean_ifd(&mut self, off: usize) -> Option<(usize, bool)> {
        let n = self.u16(off)? as usize;
        let next_at = off + 2 + 12 * n;
        let next = self.u32(next_at)? as usize;
        let mut kept: Vec<[u8; 12]> = Vec::with_capacity(n);
        let mut removed = false;
        for i in 0..n {
            let e = off + 2 + 12 * i;
            let entry: [u8; 12] = self.b.get(e..e + 12)?.try_into().ok()?;
            let (tag, ty, count) = (self.u16(e)?, self.u16(e + 2)?, self.u32(e + 4)?);
            if !TIFF_STRIP_TAGS.contains(&tag) {
                kept.push(entry);
                continue;
            }
            removed = true;
            let len = self.value_len(ty, count);
            if len > 4 {
                let at = self.u32(e + 8)? as usize;
                self.zero(at, len).or(Some(()))?;
            } else if TIFF_IFD_POINTERS.contains(&tag) {
                let sub = self.u32(e + 8)? as usize;
                self.wipe_ifd(sub, 0)?;
            }
        }
        if !removed {
            return Some((next, false));
        }
        let next_bytes: [u8; 4] = self.b.get(next_at..next_at + 4)?.try_into().ok()?;
        self.zero(off, 2 + 12 * n + 4)?;
        self.put_u16(off, kept.len() as u16)?;
        for (i, entry) in kept.iter().enumerate() {
            let e = off + 2 + 12 * i;
            self.b.get_mut(e..e + 12)?.copy_from_slice(entry);
        }
        let new_next = off + 2 + 12 * kept.len();
        self.b
            .get_mut(new_next..new_next + 4)?
            .copy_from_slice(&next_bytes);
        Some((next, true))
    }
}

/// Classic (non-Big) TIFF: removes the descriptive/identifying tags from every
/// IFD in the chain and wipes their data, the Exif and GPS IFDs included. The
/// file length and every pixel offset stay the same, so only the IFD entry
/// tables are rewritten (compacted in place). Orientation is kept. BigTIFF and
/// anything unparseable is left unchanged.
fn strip_tiff(b: &[u8]) -> Option<Vec<u8>> {
    let le = b.starts_with(b"II");
    let mut out = b.to_vec();
    let mut t = Tiff {
        b: &mut out,
        le,
        wiped: Default::default(),
    };
    let mut off = t.u32(4)? as usize;
    let mut seen = Vec::new();
    while off != 0 && seen.len() < 64 && !seen.contains(&off) {
        seen.push(off);
        let (next, _) = t.clean_ifd(off)?;
        off = next;
    }
    // The file is deliberately not truncated: TIFF-based camera RAW keeps data
    // behind tags this walker doesn't model, and cutting at "the last byte we
    // know about" would destroy it.
    Some(out)
}

// ---- SVG --------------------------------------------------------------------

fn is_svg(b: &[u8]) -> bool {
    let head = &b[..b.len().min(1024)];
    let text = String::from_utf8_lossy(head);
    let t = text.trim_start_matches('\u{feff}').trim_start();
    (t.starts_with("<?xml")
        || t.starts_with("<svg")
        || t.starts_with("<!--")
        || t.starts_with("<!DOCTYPE svg"))
        && text.contains("<svg")
}

/// Removes XML comments, `<metadata>` blocks, editor state elements, editor
/// attributes (`inkscape:*`, `sodipodi:*`, `sketch:*`, `serif:*`) and `data-*`
/// attributes, and `xpacket` processing instructions. Drawing content is
/// untouched. None for non-UTF-8 input or a construct that is not terminated
/// (the file is then not confidently clean).
fn strip_svg(b: &[u8]) -> Option<Vec<u8>> {
    let mut s = std::str::from_utf8(b).ok()?.to_owned();
    s = remove_between(&s, "<!--", "-->")?;
    s = remove_between(&s, "<?xpacket", "?>")?;
    s = remove_element(&s, "metadata")?;
    let prefixes = editor_prefixes(&s);
    for p in &prefixes {
        s = remove_prefixed_elements(&s, p)?;
    }
    let mut s = filter_svg_attributes(&s, &prefixes)?;
    // Nothing after the closing tag belongs to the drawing.
    if let Some(i) = s.rfind("</svg>") {
        let tail = i + "</svg>".len();
        if !s[tail..].trim().is_empty() {
            s.truncate(tail);
            s.push('\n');
        }
    }
    Some(s.into_bytes())
}

/// Namespaces of editors that stash private state in the document.
const EDITOR_NAMESPACES: [&str; 6] = [
    "inkscape.org/namespaces/inkscape",
    "sodipodi.sourceforge.net",
    "bohemiancoding.com/sketch",
    "serif.com",
    "ns.adobe.com/AdobeIllustrator",
    "ns.adobe.com/AdobeSVGViewerExtensions",
];

/// The prefixes bound to an editor namespace, whatever they are called
/// (`xmlns:ink="http://www.inkscape.org/namespaces/inkscape"` makes `ink`
/// one), plus the conventional ones.
fn editor_prefixes(s: &str) -> Vec<String> {
    let mut v: Vec<String> = ["inkscape", "sodipodi", "sketch", "serif", "i"]
        .iter()
        .map(|p| p.to_string())
        .collect();
    let mut rest = s;
    while let Some(i) = rest.find("xmlns:") {
        rest = &rest[i + 6..];
        let Some(eq) = rest.find('=') else { break };
        let (name, after) = (rest[..eq].trim(), rest[eq + 1..].trim_start());
        let Some(q) = after.chars().next().filter(|c| *c == '"' || *c == '\'') else {
            continue;
        };
        let Some(close) = after[1..].find(q) else {
            break;
        };
        let uri = &after[1..1 + close];
        if !name.is_empty()
            && name
                .chars()
                .all(|c| c.is_alphanumeric() || c == '_' || c == '-' || c == '.')
            && EDITOR_NAMESPACES.iter().any(|ns| uri.contains(ns))
            && !v.iter().any(|p| p == name)
        {
            v.push(name.to_string());
        }
    }
    v
}

/// Removes every `<prefix:name ...>...</prefix:name>` / self-closing element.
fn remove_prefixed_elements(s: &str, prefix: &str) -> Option<String> {
    let mut cur = s.to_owned();
    let open = format!("<{prefix}:");
    for _ in 0..4096 {
        let Some(i) = cur.find(&open) else {
            return Some(cur);
        };
        let name_end =
            cur[i + 1..].find(|c: char| c.is_whitespace() || c == '/' || c == '>')? + i + 1;
        let name = cur[i + 1..name_end].to_owned();
        let next = remove_element(&cur, &name)?;
        if next.len() == cur.len() {
            return None; // could not make progress: not confidently clean
        }
        cur = next;
    }
    None
}

fn is_editor_attribute(name: &str, prefixes: &[String]) -> bool {
    name.starts_with("data-")
        || name
            .split_once(':')
            .is_some_and(|(p, _)| prefixes.iter().any(|x| x == p))
}

/// Walks the markup tag by tag (skipping CDATA, processing instructions,
/// declarations and end tags) and drops the editor attributes from start
/// tags, in one linear pass. None on an unterminated tag, quote or CDATA.
fn filter_svg_attributes(s: &str, prefixes: &[String]) -> Option<String> {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find('<') {
        out.push_str(&rest[..i]);
        rest = &rest[i..];
        if rest.starts_with("<![CDATA[") {
            let j = rest.find("]]>")? + 3;
            out.push_str(&rest[..j]);
            rest = &rest[j..];
            continue;
        }
        let start_tag = rest[1..].starts_with(|c: char| c.is_alphabetic() || c == '_' || c == ':');
        let end = tag_end(rest)?;
        if start_tag {
            out.push_str(&filter_start_tag(&rest[..=end], prefixes)?);
        } else {
            out.push_str(&rest[..=end]); // </x>, <?pi?>, <!DOCTYPE ...>
        }
        rest = &rest[end + 1..];
    }
    out.push_str(rest);
    Some(out)
}

/// Index of the `>` closing the tag that starts at `t[0] == '<'`, ignoring any
/// `>` inside quoted attribute values.
fn tag_end(t: &str) -> Option<usize> {
    let mut quote: Option<char> = None;
    for (i, c) in t.char_indices() {
        match (quote, c) {
            (Some(q), c) if c == q => quote = None,
            (Some(_), _) => {}
            (None, '"' | '\'') => quote = Some(c),
            (None, '>') => return Some(i),
            _ => {}
        }
    }
    None
}

/// Rebuilds one start tag (`<name a="1" b='2'/>`) without editor attributes.
fn filter_start_tag(tag: &str, prefixes: &[String]) -> Option<String> {
    let name_end = tag[1..].find(|c: char| c.is_whitespace() || c == '/' || c == '>')? + 1;
    let mut out = String::with_capacity(tag.len());
    out.push_str(&tag[..name_end]);
    let mut rest = &tag[name_end..];
    loop {
        let ws_len = rest.len() - rest.trim_start().len();
        let (ws, after_ws) = rest.split_at(ws_len);
        if after_ws.starts_with('/') || after_ws.starts_with('>') {
            out.push_str(after_ws);
            return Some(out);
        }
        let n = after_ws.find(|c: char| c.is_whitespace() || c == '=' || c == '/' || c == '>')?;
        let (name, after_name) = after_ws.split_at(n);
        // Optional `= value`.
        let after_eq = after_name.trim_start();
        let (attr_len, tail) = if let Some(v) = after_eq.strip_prefix('=') {
            let v = v.trim_start();
            let consumed_before_value = after_name.len() - v.len();
            let value_len = match v.chars().next()? {
                q @ ('"' | '\'') => v[1..].find(q)? + 2,
                _ => v
                    .find(|c: char| c.is_whitespace() || c == '/' || c == '>')
                    .unwrap_or(v.len()),
            };
            (n + consumed_before_value + value_len, &v[value_len..])
        } else {
            (n, after_name)
        };
        if !is_editor_attribute(name, prefixes) {
            out.push_str(ws);
            out.push_str(&after_ws[..attr_len]);
        }
        rest = tail;
    }
}

fn remove_between(s: &str, open: &str, close: &str) -> Option<String> {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find(open) {
        out.push_str(&rest[..i]);
        let j = rest[i + open.len()..].find(close)?; // unterminated: None
        rest = &rest[i + open.len() + j + close.len()..];
    }
    out.push_str(rest);
    Some(out)
}

/// Drops `<tag ...>...</tag>` and self-closing `<tag .../>` elements.
fn remove_element(s: &str, tag: &str) -> Option<String> {
    let open = format!("<{tag}");
    let close = format!("</{tag}>");
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find(&open) {
        let after = &rest[i + open.len()..];
        // Must be this tag, not a longer name sharing the prefix.
        if !after.starts_with(|c: char| c.is_whitespace() || c == '>' || c == '/') {
            out.push_str(&rest[..i + open.len()]);
            rest = after;
            continue;
        }
        out.push_str(&rest[..i]);
        let gt = after.find('>')?;
        if after[..gt].ends_with('/') {
            rest = &after[gt + 1..];
        } else {
            let j = after[gt + 1..].find(&close)?;
            rest = &after[gt + 1 + j + close.len()..];
        }
    }
    out.push_str(rest);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use img_parts::jpeg::JpegSegment;
    use img_parts::png::PngChunk;
    use img_parts::riff::{RiffChunk, RiffContent};
    use img_parts::webp::CHUNK_XMP;

    fn contains(hay: &[u8], needle: &[u8]) -> bool {
        hay.windows(needle.len()).any(|w| w == needle)
    }

    fn with_exif(mut img: Vec<u8>, ext: FileExtension, orientation: u16) -> Vec<u8> {
        let mut md = Metadata::new();
        md.set_tag(ExifTag::Orientation(vec![orientation]));
        md.set_tag(ExifTag::Make("SECRETMAKE".to_string()));
        md.write_to_vec(&mut img, ext).unwrap();
        img
    }

    fn jpeg_fixture() -> Vec<u8> {
        let segs = vec![
            JpegSegment::new(markers::SOI),
            JpegSegment::new_with_contents(
                markers::APP0,
                Bytes::from_static(b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0"),
            ),
            JpegSegment::new_with_contents(
                markers::APP1,
                Bytes::from_static(b"http://ns.adobe.com/xap/1.0/\0<x:xmpmeta>SECRETXMP"),
            ),
            JpegSegment::new_with_contents(
                markers::APP13,
                Bytes::from_static(b"Photoshop 3.0\0SECRETIPTC"),
            ),
            JpegSegment::new_with_contents(markers::COM, Bytes::from_static(b"SECRETCOMMENT")),
            JpegSegment::new_with_contents(
                markers::APP2,
                Bytes::from_static(b"ICC_PROFILE\0\x01\x01PROFILE"),
            ),
            JpegSegment::new_with_contents(markers::DQT, Bytes::from_static(&[0; 65])),
            JpegSegment::new_with_entropy(
                markers::SOS,
                Bytes::from_static(&[1, 1, 0, 0, 63, 0]),
                Bytes::from_static(&[0xAB, 0xCD]),
            ),
            JpegSegment::new(markers::EOI),
        ];
        let mut out = Vec::new();
        for s in segs {
            out.extend_from_slice(&s.encoder().bytes());
        }
        out
    }

    fn png_fixture() -> Vec<u8> {
        let mut png = Png::from_bytes(Bytes::from_static(&[
            0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, b'I', b'E', b'N', b'D',
            0xAE, 0x42, 0x60, 0x82,
        ]))
        .unwrap();
        let end = png.chunks().len() - 1;
        png.chunks_mut().insert(
            end,
            PngChunk::new(*b"tEXt", Bytes::from_static(b"Comment\0SECRETTEXT")),
        );
        png.chunks_mut()
            .insert(end, PngChunk::new(*b"IDAT", Bytes::from_static(b"pixels")));
        png.encoder().bytes().to_vec()
    }

    fn webp_fixture() -> Vec<u8> {
        let riff = RiffChunk::new(
            *b"RIFF",
            RiffContent::List {
                kind: Some(*b"WEBP"),
                subchunks: vec![
                    RiffChunk::new(
                        *b"VP8L",
                        RiffContent::Data(Bytes::from_static(b"\x2f\0\0\0\0pixels")),
                    ),
                    RiffChunk::new(
                        CHUNK_XMP,
                        RiffContent::Data(Bytes::from_static(b"SECRETXMP")),
                    ),
                ],
            },
        );
        WebP::new(riff).unwrap().encoder().bytes().to_vec()
    }

    #[test]
    fn jpeg_strips_everything_but_orientation_and_icc() {
        let src = with_exif(jpeg_fixture(), FileExtension::JPEG, 6);
        assert!(contains(&src, b"SECRETMAKE") && contains(&src, b"SECRETXMP"));
        let out = strip_image_metadata(&src, "image/jpeg");
        for secret in [
            &b"SECRETMAKE"[..],
            b"SECRETXMP",
            b"SECRETIPTC",
            b"SECRETCOMMENT",
        ] {
            assert!(
                !contains(&out, secret),
                "{}",
                String::from_utf8_lossy(secret)
            );
        }
        assert!(contains(&out, b"ICC_PROFILE") && contains(&out, &[0xAB, 0xCD]));
        assert_eq!(read_orientation(&out, &FileExtension::JPEG), Some(6));
    }

    #[test]
    fn declared_mime_cannot_skip_the_strip() {
        let src = with_exif(jpeg_fixture(), FileExtension::JPEG, 1);
        for mime in ["application/octet-stream", "", "image/png"] {
            let out = strip_image_metadata(&src, mime);
            assert!(
                !contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETXMP"),
                "{mime}"
            );
        }
        let png = png_fixture();
        assert!(!contains(
            &strip_image_metadata(&png, "application/octet-stream"),
            b"SECRETTEXT"
        ));
        let webp = webp_fixture();
        assert!(!contains(
            &strip_image_metadata(&webp, "image/jpeg"),
            b"SECRETXMP"
        ));
    }

    #[test]
    fn jpeg_upright_leaves_no_exif() {
        let src = with_exif(jpeg_fixture(), FileExtension::JPEG, 1);
        let out = strip_image_metadata(&src, "image/jpeg");
        assert!(!contains(&out, b"SECRETMAKE"));
        assert!(!contains(&out, b"Exif"));
    }

    #[test]
    fn png_strips_text_and_exif() {
        let ext = FileExtension::PNG {
            as_zTXt_chunk: false,
        };
        let src = with_exif(png_fixture(), ext, 8);
        assert!(contains(&src, b"SECRETTEXT") && contains(&src, b"Raw profile type exif"));
        let out = strip_image_metadata(&src, "image/png");
        assert!(!contains(&out, b"SECRETTEXT") && !contains(&out, b"Raw profile type exif"));
        assert!(contains(&out, b"pixels"));
    }

    #[test]
    fn webp_strips_exif_and_xmp_keeps_orientation() {
        let src = with_exif(webp_fixture(), FileExtension::WEBP, 3);
        assert!(contains(&src, b"SECRETXMP") && contains(&src, b"SECRETMAKE"));
        let out = strip_image_metadata(&src, "image/webp");
        assert!(!contains(&out, b"SECRETXMP") && !contains(&out, b"SECRETMAKE"));
        assert!(contains(&out, b"pixels"));
        assert_eq!(read_orientation(&out, &FileExtension::WEBP), Some(3));
    }

    const JXL_BROB: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.jxl");
    const JXL_RAW: &[u8] = include_bytes!("../../tests/fixtures/meta/meta_raw.jxl");
    const AVIF: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.avif");

    #[test]
    fn jxl_container_metadata_boxes_are_removed_compressed_or_not() {
        assert!(contains(JXL_RAW, b"SECRETMAKE"));
        for (src, mime) in [
            (JXL_BROB, "image/jxl"),
            (JXL_RAW, "image/jxl"),
            (JXL_RAW, "application/octet-stream"),
        ] {
            let out = strip_image_metadata(src, mime);
            assert!(out.len() < src.len());
            assert!(
                !contains(&out, b"SECRETMAKE")
                    && !contains(&out, b"Exif")
                    && !contains(&out, b"brob")
            );
            // Pixels are untouched: still a valid, same-size image.
            let d = crate::jxl::jxl_decoder_new(&out, 100, 100);
            assert!(d.valid());
            assert_eq!(d.width(), 8);
        }
    }

    fn iso_box(ty: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut v = ((payload.len() + 8) as u32).to_be_bytes().to_vec();
        v.extend_from_slice(ty);
        v.extend_from_slice(payload);
        v
    }

    #[test]
    fn lossless_jpeg_transcode_jbrd_is_removed_and_the_image_still_decodes() {
        let src = include_bytes!("../../tests/fixtures/meta/meta_jpeg.jxl");
        assert!(contains(src, b"jbrd"));
        let out = strip_image_metadata(src, "image/jxl");
        assert!(!contains(&out, b"jbrd") && !contains(&out, b"brob"));
        let d = crate::jxl::jxl_decoder_new(&out, 100, 100);
        assert!(d.valid());
        assert_eq!((d.width(), d.height()), (8, 4));
    }

    #[test]
    fn jxl_jumbf_boxes_are_removed() {
        let mut src = JXL_RAW.to_vec();
        src.extend(iso_box(b"jumb", b"SECRETC2PA"));
        let out = strip_image_metadata(&src, "image/jxl");
        assert!(!contains(&out, b"SECRETC2PA") && !contains(&out, b"SECRETMAKE"));
        assert!(crate::jxl::jxl_decoder_new(&out, 100, 100).valid());
    }

    #[test]
    fn avif_xmp_uuid_and_xml_boxes_are_zeroed() {
        let mut payload = XMP_UUID.to_vec();
        payload.extend_from_slice(b"<x:xmpmeta>SECRETUUIDXMP</x:xmpmeta>");
        let mut src = AVIF.to_vec();
        src.extend(iso_box(b"uuid", &payload));
        let out = strip_image_metadata(&src, "image/avif");
        assert_eq!(out.len(), src.len());
        assert!(!contains(&out, b"SECRETUUIDXMP") && !contains(&out, b"SECRETMAKE"));
        // A non-XMP uuid box is left alone.
        let mut other = AVIF.to_vec();
        other.extend(iso_box(b"uuid", &[7u8; 24]));
        assert!(contains(
            &strip_image_metadata(&other, "image/avif"),
            &[7u8; 24]
        ));
    }

    #[test]
    fn jxl_orientation_survives_the_strip() {
        let src = include_bytes!("../../tests/fixtures/meta/rot_meta.jxl");
        assert!(contains(src, b"SECRETMAKE") || contains(src, b"brob"));
        // Orientation 6 lives in the codestream header: the stored 8x4 image
        // is presented 4x8 before and after.
        assert_eq!(crate::jxl::jxl_decoder_new(src, 100, 100).width(), 4);
        let out = strip_image_metadata(src, "image/jxl");
        assert!(out.len() < src.len() && !contains(&out, b"SECRETMAKE"));
        let d = crate::jxl::jxl_decoder_new(&out, 100, 100);
        assert!(d.valid());
        assert_eq!((d.width(), d.height()), (4, 8));
    }

    #[test]
    fn avif_rotation_properties_survive_the_strip() {
        // Orientation is the irot property, not EXIF; the scrub must leave it.
        let src = include_bytes!("../../tests/fixtures/meta/rot_meta.avif");
        assert!(contains(src, b"irot") && contains(src, b"SECRETMAKE"));
        let out = strip_image_metadata(src, "image/avif");
        assert!(!contains(&out, b"SECRETMAKE"));
        let at = |b: &[u8]| b.windows(4).position(|w| w == b"irot").unwrap();
        assert_eq!(
            &out[at(&out) - 4..at(&out) + 5],
            &src[at(src) - 4..at(src) + 5]
        );
    }

    const TIFF: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.tif");
    const HEIC: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.heic");
    const JP2: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.jp2");
    const SVG: &[u8] = include_bytes!("../../tests/fixtures/meta/meta.svg");

    #[test]
    fn tiff_tags_ifds_and_xmp_are_removed_but_orientation_stays() {
        for s in [
            &b"SECRETMAKE"[..],
            b"SECRETARTIST",
            b"SECRETSOFT",
            b"SECRETDESC",
            b"SECRETXMP",
            b"SECRETIPTC",
        ] {
            assert!(
                contains(TIFF, s),
                "fixture lacks {}",
                String::from_utf8_lossy(s)
            );
        }
        for mime in ["image/tiff", "application/octet-stream"] {
            let out = strip_image_metadata(TIFF, mime);
            assert_eq!(out.len(), TIFF.len()); // pixel offsets untouched
            for s in [
                &b"SECRETMAKE"[..],
                b"SECRETARTIST",
                b"SECRETSOFT",
                b"SECRETDESC",
                b"SECRETXMP",
                b"SECRETIPTC",
            ] {
                assert!(!contains(&out, s), "{}", String::from_utf8_lossy(s));
            }
            // Orientation 6 (tag 274) survives; the GPS/Exif IFD pointers do not.
            let mut copy = out.to_vec();
            let t = Tiff {
                b: &mut copy,
                le: out.starts_with(b"II"),
                wiped: Default::default(),
            };
            let ifd = t.u32(4).unwrap() as usize;
            let tags: Vec<u16> = (0..t.u16(ifd).unwrap() as usize)
                .map(|i| t.u16(ifd + 2 + 12 * i).unwrap())
                .collect();
            assert!(
                tags.contains(&274)
                    && !tags.contains(&34853)
                    && !tags.contains(&34665)
                    && !tags.contains(&271)
            );
            assert!(
                tags.windows(2).all(|w| w[0] < w[1]),
                "IFD entries stay sorted"
            );
        }
    }

    #[test]
    fn tiff_without_metadata_and_bigtiff_are_unchanged() {
        let clean = strip_image_metadata(TIFF, "image/tiff").into_owned();
        assert_eq!(&*strip_image_metadata(&clean, "image/tiff"), &clean[..]);
        let big = b"II+\0\x08\0\0\0\x10\0\0\0\0\0\0\0".to_vec();
        assert_eq!(&*strip_image_metadata(&big, "image/tiff"), &big[..]);
        assert_eq!(
            &*strip_image_metadata(&TIFF[..30], "image/tiff"),
            &TIFF[..30]
        );
    }

    #[test]
    fn heic_exif_is_scrubbed_in_place_like_avif() {
        assert!(contains(HEIC, b"SECRETMAKE"));
        let out = strip_image_metadata(HEIC, "application/octet-stream");
        assert_eq!(out.len(), HEIC.len());
        assert!(!contains(&out, b"SECRETMAKE"));
        assert!(contains(&out, b"ftypheic") && contains(&out, b"irot") == contains(HEIC, b"irot"));
    }

    #[test]
    fn jp2_xml_and_uuid_boxes_are_removed() {
        assert!(contains(JP2, b"SECRETMAKE") && contains(JP2, b"SECRETXMP"));
        let out = strip_image_metadata(JP2, "image/jp2");
        assert!(out.len() < JP2.len());
        assert!(
            !contains(&out, b"SECRETMAKE")
                && !contains(&out, b"SECRETXMP")
                && !contains(&out, b"uuid")
        );
        assert!(contains(&out, b"jp2h") && contains(&out, b"jp2c"));
    }

    #[test]
    fn svg_metadata_comments_and_editor_state_are_removed() {
        let out =
            String::from_utf8(strip_image_metadata(SVG, "image/svg+xml").into_owned()).unwrap();
        for secret in [
            "SECRETCOMMENT",
            "SECRETUSER",
            "SECRETAUTHOR",
            "namedview",
            "<metadata",
        ] {
            assert!(!out.contains(secret), "{secret}");
        }
        // The drawing is intact.
        assert_eq!(out.matches("<rect").count(), 2);
        assert!(out.contains("viewBox=\"0 0 8 4\"") && out.contains("#c81e1e"));
        // Content-based: a wrong mime changes nothing.
        assert_eq!(&*strip_image_metadata(SVG, "text/plain"), out.as_bytes());
        // An element merely sharing the prefix is not removed.
        let keep = b"<svg xmlns=\"http://www.w3.org/2000/svg\"><metadataish/><rect/></svg>";
        assert_eq!(&*strip_image_metadata(keep, "image/svg+xml"), &keep[..]);
        // Unterminated comment: left alone.
        let bad = b"<svg xmlns=\"http://www.w3.org/2000/svg\"><!-- oops <rect/></svg>";
        assert_eq!(&*strip_image_metadata(bad, "image/svg+xml"), &bad[..]);
    }

    #[test]
    fn corrupted_and_truncated_files_never_panic() {
        // Direct calls (no catch_unwind): any panic fails the test.
        let files: [&[u8]; 9] = [
            TIFF,
            HEIC,
            JP2,
            SVG,
            AVIF,
            JXL_BROB,
            JXL_RAW,
            &gif_fixture(),
            include_bytes!("../../tests/fixtures/meta/rot_meta.avif"),
        ];
        for src in files {
            for cut in (0..src.len()).step_by(7) {
                let _ = strip_inner(&src[..cut], "image/jpeg");
            }
            for i in 0..src.len() {
                let mut b = src.to_vec();
                b[i] ^= 0xFF;
                let _ = strip_inner(&b, "application/octet-stream");
            }
        }
    }

    #[test]
    fn hostile_nested_tiff_ifds_do_not_blow_up() {
        // IFD0 holds one Exif-pointer entry -> sub-IFD A whose 3000 entries are
        // all interop pointers back to A and to B, B likewise to A: without a
        // visited set this fans out 3000^4 wipes.
        let mut b = b"II*\0\x08\0\0\0".to_vec();
        let entry = |tag: u16, val: u32| {
            let mut e = Vec::new();
            e.extend_from_slice(&tag.to_le_bytes());
            e.extend_from_slice(&4u16.to_le_bytes());
            e.extend_from_slice(&1u32.to_le_bytes());
            e.extend_from_slice(&val.to_le_bytes());
            e
        };
        let (a_off, b_off) = (8 + 2 + 12 + 4, 8 + 2 + 12 + 4 + 2 + 3000 * 12 + 4);
        b.extend_from_slice(&1u16.to_le_bytes());
        b.extend(entry(34665, a_off as u32));
        b.extend_from_slice(&0u32.to_le_bytes());
        for off in [a_off, b_off] {
            let other = if off == a_off { b_off } else { a_off };
            b.extend_from_slice(&3000u16.to_le_bytes());
            for i in 0..3000 {
                b.extend(entry(40965, if i % 2 == 0 { other } else { off } as u32));
            }
            b.extend_from_slice(&0u32.to_le_bytes());
        }
        let t0 = std::time::Instant::now();
        let out = strip_image_metadata(&b, "image/tiff");
        assert!(
            t0.elapsed() < std::time::Duration::from_secs(5),
            "took {:?}",
            t0.elapsed()
        );
        assert!(out.len() <= b.len()); // the wiped sub-IFDs are no longer referenced
    }

    #[test]
    fn svg_attribute_removal_is_linear_and_exact() {
        let mut svg = String::from("<svg xmlns=\"http://www.w3.org/2000/svg\">");
        for _ in 0..200_000 {
            svg.push_str("<g inkscape:export-xdpi=\"96\" id=\"a\"/>");
        }
        svg.push_str("<g xinkscape:export-xdpi=\"1\" inkscape:export-filename='/home/x'/></svg>");
        let t0 = std::time::Instant::now();
        let out =
            String::from_utf8(strip_image_metadata(svg.as_bytes(), "image/svg+xml").into_owned())
                .unwrap();
        assert!(
            t0.elapsed() < std::time::Duration::from_secs(5),
            "took {:?}",
            t0.elapsed()
        );
        assert!(!out.contains("inkscape:export-xdpi=\"96\"") && !out.contains("/home/x"));
        assert!(out.contains("xinkscape:export-xdpi=\"1\"")); // not preceded by whitespace: kept
        assert_eq!(out.matches("id=\"a\"").count(), 200_000);
    }

    #[test]
    fn strippable_means_parsed_or_known_metadata_free() {
        // Parsed formats, with and without metadata present.
        for (b, m) in [
            (TIFF, "image/tiff"),
            (HEIC, "image/heic"),
            (AVIF, "image/avif"),
            (JP2, "image/jp2"),
            (SVG, "image/svg+xml"),
            (JXL_BROB, "image/jxl"),
            (JXL_RAW, "image/jxl"),
        ] {
            assert!(is_strippable(b, m), "{m}");
        }
        assert!(is_strippable(&gif_fixture(), "image/gif"));
        assert!(is_strippable(&jpeg_fixture(), "image/jpeg"));
        assert!(is_strippable(&png_fixture(), "image/png"));
        assert!(is_strippable(&webp_fixture(), "image/webp"));
        // Known to carry no metadata.
        let mut bmp = b"BM".to_vec();
        bmp.extend_from_slice(&[0u8; 12]);
        bmp.extend_from_slice(&40u32.to_le_bytes());
        bmp.extend_from_slice(&[0u8; 40]);
        assert!(is_strippable(&bmp, "image/bmp"));
        assert!(is_strippable(
            include_bytes!("../../tests/fixtures/jxl/still_rgb.jxl"),
            "image/jxl"
        ));
        // QOI has no end we can bound, so it is not "known clean" (it gets recompressed).
        assert!(!is_strippable(
            b"qoif\0\0\0\x01\0\0\0\x01\x03\0",
            "image/qoi"
        ));
    }

    #[test]
    fn unrecognised_and_unparseable_images_are_not_strippable() {
        // No stripper for these (TGA-ish, PSD, RAW, random): never "confidently clean".
        for (b, m) in [
            (&b"8BPS\0\x01\0\0\0\0\0\0"[..], "image/vnd.adobe.photoshop"),
            (&[0u8; 64][..], "image/x-tga"),
            (b"IIRO\x08\0\0\0", "image/x-olympus-orf"),
            (b"anything", "application/octet-stream"),
            (b"", "image/png"),
        ] {
            assert!(!is_strippable(b, m), "{m}");
        }
        // Recognised but broken: truncated / corrupted structure is not clean.
        assert!(!is_strippable(&TIFF[..30], "image/tiff"));
        assert!(!is_strippable(&AVIF[..40], "image/avif"));
        assert!(!is_strippable(&JXL_RAW[..20], "image/jxl"));
        let g = gif_fixture();
        assert!(!is_strippable(&g[..g.len() - 5], "image/gif"));
        assert!(!is_strippable(&JPEG_TRUNC, "image/jpeg"));
        assert!(!is_strippable(
            b"<svg xmlns=\"http://www.w3.org/2000/svg\"><!-- oops</svg>",
            "image/svg+xml"
        ));
        // A declared image mime does not make unknown bytes strippable.
        assert!(!is_strippable(&[1u8; 64], "image/png"));
    }

    const JPEG_TRUNC: [u8; 6] = [0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10];

    fn jpeg_with_thumbnails() -> Vec<u8> {
        let mut jpeg = Jpeg::from_bytes(Bytes::from(jpeg_fixture())).unwrap();
        let segs = jpeg.segments_mut();
        // Replace the plain JFIF header with one carrying a 2x2 RGB thumbnail...
        segs[1] = JpegSegment::new_with_contents(
            markers::APP0,
            Bytes::from_static(b"JFIF\0\x01\x01\x01\0\x48\0\x48\x02\x02SECRETTHUMB!"),
        );
        // ...and add a JFXX extension thumbnail.
        segs.insert(
            2,
            JpegSegment::new_with_contents(
                markers::APP0,
                Bytes::from_static(b"JFXX\0\x10SECRETJFXXTHUMB"),
            ),
        );
        jpeg.encoder().bytes().to_vec()
    }

    #[test]
    fn jpeg_thumbnails_are_removed_but_jfif_density_stays() {
        let src = jpeg_with_thumbnails();
        assert!(contains(&src, b"SECRETTHUMB!") && contains(&src, b"SECRETJFXXTHUMB"));
        let out = strip_image_metadata(&src, "image/jpeg");
        assert!(
            !contains(&out, b"SECRETTHUMB!")
                && !contains(&out, b"SECRETJFXXTHUMB")
                && !contains(&out, b"JFXX")
        );
        // JFIF header kept: units=1, density 0x48 x 0x48, thumbnail size zeroed.
        assert!(contains(&out, b"JFIF\0\x01\x01\x01\0\x48\0\x48\0\0"));
        assert!(contains(&out, b"ICC_PROFILE") && contains(&out, &[0xAB, 0xCD]));
        // A JFIF without a thumbnail is left exactly as it was.
        let plain = jpeg_fixture();
        assert!(contains(
            &strip_image_metadata(&plain, "image/jpeg"),
            b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0"
        ));
    }

    fn png_with_exif(orientation: u16) -> Vec<u8> {
        // Big-endian EXIF: Make (ASCII, out of line) + Orientation.
        let mut e = b"MM\0*\0\0\0\x08".to_vec();
        e.extend_from_slice(&[0, 2]);
        e.extend_from_slice(&[0x01, 0x0F, 0, 2, 0, 0, 0, 11, 0, 0, 0, 38]); // Make -> offset 38
        e.extend_from_slice(&[0x01, 0x12, 0, 3, 0, 0, 0, 1]);
        e.extend_from_slice(&orientation.to_be_bytes());
        e.extend_from_slice(&[0, 0, 0, 0, 0, 0]);
        e.extend_from_slice(b"SECRETMAKE\0");
        let mut png = Png::from_bytes(Bytes::from(png_fixture())).unwrap();
        png.set_exif(Some(Bytes::from(e)));
        png.encoder().bytes().to_vec()
    }

    #[test]
    fn png_orientation_survives_as_a_standard_exif_chunk() {
        let src = png_with_exif(6);
        assert!(contains(&src, b"SECRETMAKE") && contains(&src, b"eXIf"));
        for mime in ["image/png", "application/octet-stream"] {
            let out = strip_image_metadata(&src, mime);
            assert!(!contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETTEXT"));
            let exif = Png::from_bytes(Bytes::copy_from_slice(&out))
                .unwrap()
                .exif()
                .expect("eXIf kept");
            assert_eq!(tiff_orientation(&exif), Some(6));
            assert!(exif.len() < 40, "only the orientation entry remains");
        }
        // Upright PNGs end up with no EXIF at all.
        let upright = png_with_exif(1);
        let out = strip_image_metadata(&upright, "image/png");
        assert!(Png::from_bytes(Bytes::copy_from_slice(&out))
            .unwrap()
            .exif()
            .is_none());
    }

    #[test]
    fn svg_editor_attributes_and_data_attributes_are_dropped_markup_is_respected() {
        let svg = br##"<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" inkscape:version="1.2 (SECRETVERSION)" data-author='SECRETAUTHOR' width="8">
  <defs><inkscape:path-effect id="e" effect="spiro" is_visible="true"/></defs>
  <g inkscape:label="SECRETLAYER" inkscape:groupmode="layer" id="g1"><path d="M0 0" sodipodi:nodetypes="cc" fill="#fff" title="a > b" data-x = "SECRETX"/></g>
  <text><![CDATA[ keep <b data-y="SECRETCDATA"> as text ]]></text>
  <rect id="r" serif:id="SECRETSERIF" sketch:type="MSPage"/>
</svg>"##;
        let out =
            String::from_utf8(strip_image_metadata(svg, "image/svg+xml").into_owned()).unwrap();
        for secret in [
            "SECRETVERSION",
            "SECRETAUTHOR",
            "SECRETLAYER",
            "SECRETX",
            "SECRETSERIF",
            "path-effect",
            "nodetypes",
            "sketch:type",
        ] {
            assert!(!out.contains(secret), "{secret} in {out}");
        }
        // Drawing, ids, a '>' inside a quoted value and CDATA text are intact.
        assert!(
            out.contains(r#"id="g1""#)
                && out.contains(r##"fill="#fff""##)
                && out.contains(r#"title="a > b""#)
        );
        assert!(out.contains(r#"<![CDATA[ keep <b data-y="SECRETCDATA"> as text ]]>"#));
        assert!(out.contains(r#"<rect id="r"/>"#));
        // An unterminated tag is not confidently clean.
        assert!(!is_strippable(
            b"<svg xmlns=\"http://www.w3.org/2000/svg\"><g data-a=\"x",
            "image/svg+xml"
        ));
    }

    #[test]
    fn data_appended_after_the_image_is_dropped() {
        let tail: &[u8] = b"SECRETTRAILINGDATA";
        let with_tail = |mut b: Vec<u8>| {
            b.extend_from_slice(tail);
            b
        };
        // JPEG: bytes after EOI (the cropped-out original, a motion-photo clip, ...).
        let jpeg = with_tail(jpeg_fixture());
        assert!(contains(&jpeg, tail));
        let out = strip_image_metadata(&jpeg, "image/jpeg");
        assert!(!contains(&out, tail) && contains(&out, &[0xAB, 0xCD]));
        assert_eq!(&out[out.len() - 2..], &[0xFF, 0xD9]); // ends at EOI
                                                          // PNG (after IEND), WebP (after the RIFF size), GIF (after the trailer).
        for (name, file, mime) in [
            ("png", with_tail(png_fixture()), "image/png"),
            ("webp", with_tail(webp_fixture()), "image/webp"),
            ("gif", with_tail(gif_fixture()), "image/gif"),
        ] {
            assert!(
                !contains(&strip_image_metadata(&file, mime), tail),
                "{name}"
            );
        }
        // TIFF is never truncated: camera RAW keeps data behind tags the walker
        // does not model, so bytes past the last known structure are kept.
        let tiff = with_tail(TIFF.to_vec());
        let out = strip_image_metadata(&tiff, "image/tiff");
        assert!(contains(&out, tail));
        assert_eq!(out.len(), tiff.len());
        // BMP: beyond the declared file size.
        let mut bmp = b"BM".to_vec();
        bmp.extend_from_slice(&80u32.to_le_bytes()); // declared size
        bmp.extend_from_slice(&[0u8; 8]);
        bmp.extend_from_slice(&40u32.to_le_bytes());
        bmp.resize(80, 0);
        let bmp_tail = with_tail(bmp.clone());
        let out = strip_image_metadata(&bmp_tail, "image/bmp");
        assert_eq!(&*out, &bmp[..]);
        // SVG: text after the closing tag.
        let svg = b"<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>SECRETTRAILINGDATA";
        assert!(!contains(&strip_image_metadata(svg, "image/svg+xml"), tail));
        // HEIF/JXL/JP2 box files must tile exactly: appended bytes make them
        // "not confidently clean" rather than being passed through.
        assert!(!is_strippable(&with_tail(AVIF.to_vec()), "image/avif"));
        assert!(!is_strippable(&with_tail(JXL_RAW.to_vec()), "image/jxl"));
        assert!(!is_strippable(&with_tail(JP2.to_vec()), "image/jp2"));
    }

    #[test]
    fn svg_editor_namespace_under_a_custom_prefix_is_still_stripped() {
        let svg = br##"<svg xmlns="http://www.w3.org/2000/svg" xmlns:ink="http://www.inkscape.org/namespaces/inkscape" xmlns:sd="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" ink:version="SECRETVERSION" width="8">
  <sd:namedview id="nv" ink:export-filename="/home/SECRETUSER/x.png"><ink:grid id="g"/></sd:namedview>
  <defs><ink:path-effect id="e"/></defs>
  <g ink:label="SECRETLAYER" id="g1"><rect id="r" width="4" height="4"/></g>
</svg>"##;
        let out =
            String::from_utf8(strip_image_metadata(svg, "image/svg+xml").into_owned()).unwrap();
        for secret in [
            "SECRETVERSION",
            "SECRETUSER",
            "SECRETLAYER",
            "namedview",
            "path-effect",
            "ink:grid",
        ] {
            assert!(!out.contains(secret), "{secret} in {out}");
        }
        assert!(
            out.contains(r#"<g id="g1">"#)
                && out.contains(r#"<rect id="r" width="4" height="4"/>"#)
        );
        // A prefix bound to an unrelated namespace is left alone.
        let other = br#"<svg xmlns="http://www.w3.org/2000/svg" xmlns:ink="http://example.com/ns"><rect ink:label="kept"/></svg>"#;
        assert_eq!(&*strip_image_metadata(other, "image/svg+xml"), &other[..]);
    }

    #[test]
    fn bmp_has_no_metadata_and_is_untouched() {
        let mut bmp = b"BM".to_vec();
        bmp.extend_from_slice(&[0u8; 60]);
        assert_eq!(&*strip_image_metadata(&bmp, "image/bmp"), &bmp[..]);
    }

    #[test]
    fn bare_jxl_codestream_is_unchanged() {
        let bare = include_bytes!("../../tests/fixtures/jxl/still_rgb.jxl");
        assert_eq!(&*strip_image_metadata(bare, "image/jxl"), &bare[..]);
    }

    #[test]
    fn avif_exif_and_xmp_items_are_scrubbed_in_place() {
        assert!(contains(AVIF, b"SECRETMAKE") && contains(AVIF, b"SECRETXMP"));
        for mime in ["image/avif", "application/octet-stream"] {
            let out = strip_image_metadata(AVIF, mime);
            assert_eq!(out.len(), AVIF.len()); // offsets stay valid
            assert!(!contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETXMP"));
            assert!(
                !contains(&out, b"Exif") && !contains(&out, b"rdf+xml") || contains(&out, b"free")
            );
            assert!(contains(&out, b"ftypavif") && contains(&out, b"mdat"));
        }
    }

    fn gif_fixture() -> Vec<u8> {
        let mut g = b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff".to_vec();
        g.extend_from_slice(b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"); // loop
        g.extend_from_slice(b"\x21\xFE\x0DSECRETCOMMENT\x00"); // comment
        g.extend_from_slice(b"\x21\xFF\x0BXMP DataXMP\x0DSECRETXMPDATA\x00"); // XMP
        for _ in 0..2 {
            g.extend_from_slice(b"\x21\xF9\x04\x00\x0A\x00\x00\x00"); // GCE keeps delay
            g.extend_from_slice(b"\x2C\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00");
        }
        g.push(0x3B);
        g
    }

    #[test]
    fn gif_drops_comments_and_xmp_keeps_animation() {
        let src = gif_fixture();
        for mime in ["image/gif", "application/octet-stream"] {
            let out = strip_image_metadata(&src, mime);
            assert!(!contains(&out, b"SECRETCOMMENT") && !contains(&out, b"SECRETXMPDATA"));
            assert!(contains(&out, b"NETSCAPE2.0"));
            assert_eq!(out.iter().filter(|&&b| b == 0x2C).count(), 2); // both frames
            assert_eq!(
                out.windows(8)
                    .filter(|w| w == b"\x21\xF9\x04\x00\x0A\x00\x00\x00")
                    .count(),
                2
            );
            assert_eq!(*out.last().unwrap(), 0x3B);
        }
    }

    #[test]
    fn truncated_gif_and_avif_pass_through() {
        let g = gif_fixture();
        assert_eq!(
            &*strip_image_metadata(&g[..g.len() - 5], "image/gif"),
            &g[..g.len() - 5]
        );
        assert_eq!(
            &*strip_image_metadata(&AVIF[..40], "image/avif"),
            &AVIF[..40]
        );
    }

    #[test]
    fn garbage_and_unknown_mime_pass_through() {
        let junk = b"definitely not an image";
        for mime in [
            "image/jpeg",
            "image/png",
            "image/webp",
            "image/gif",
            "video/mp4",
        ] {
            assert!(
                matches!(strip_image_metadata(junk, mime), Cow::Borrowed(_)),
                "{mime}"
            );
        }
        let jpeg = jpeg_fixture();
        let _ = strip_image_metadata(&jpeg[..20], "image/jpeg");
    }

    // ---- scope, allow-lists and bounds --------------------------------------

    #[test]
    fn file_scope_strips_photo_containers_only() {
        // A photo sent as a file loses its metadata.
        let jpeg = with_exif(jpeg_fixture(), FileExtension::JPEG, 6);
        assert!(contains(&jpeg, b"SECRETMAKE"));
        let out = strip_for_upload(&jpeg, "application/octet-stream", StripScope::File);
        assert!(!contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETCOMMENT"));
        let png_bytes = png_fixture();
        let png = strip_for_upload(&png_bytes, "application/octet-stream", StripScope::File);
        assert!(!contains(&png, b"SECRETTEXT"));

        // Everything else is byte-identical, whatever the declared mime says.
        for (name, bytes, mime) in [
            ("tiff", TIFF.to_vec(), "image/tiff"),
            ("jp2", JP2.to_vec(), "image/jp2"),
            ("svg", b"<?xml version=\"1.0\"?><!-- keep me --><svg xmlns=\"http://www.w3.org/2000/svg\" data-x=\"1\"/>".to_vec(), "image/svg+xml"),
            ("bmp", { let mut b = b"BM".to_vec(); b.extend_from_slice(&40u32.to_le_bytes()); b.resize(60, 0); b }, "image/bmp"),
        ] {
            let out = strip_for_upload(&bytes, mime, StripScope::File);
            assert!(matches!(out, Cow::Borrowed(_)), "{name} was rewritten");
            assert_eq!(&*out, &bytes[..], "{name}");
        }
        // The same TIFF *is* stripped when the user sent it as an image.
        assert_ne!(
            &*strip_for_upload(TIFF, "image/tiff", StripScope::Image),
            TIFF
        );
    }

    #[test]
    fn png_private_chunks_are_dropped_but_rendering_chunks_stay() {
        let mut png = Png::from_bytes(Bytes::from(png_fixture())).unwrap();
        let end = png.chunks().len() - 1;
        png.chunks_mut().insert(
            end,
            PngChunk::new(*b"caBX", Bytes::from_static(b"C2PA-MANIFEST-AUTHOR")),
        );
        png.chunks_mut().insert(
            end,
            PngChunk::new(
                *b"pHYs",
                Bytes::from_static(&[0, 0, 0x0b, 0x13, 0, 0, 0x0b, 0x13, 1]),
            ),
        );
        let bytes = png.encoder().bytes().to_vec();
        assert!(contains(&bytes, b"caBX"));
        let out = strip_image_metadata(&bytes, "image/png");
        assert!(!contains(&out, b"caBX") && !contains(&out, b"C2PA-MANIFEST"));
        assert!(contains(&out, b"pHYs") && contains(&out, b"IDAT"));
    }

    #[test]
    fn webp_unknown_chunks_are_dropped_but_icc_and_animation_stay() {
        let riff = RiffChunk::new(
            *b"RIFF",
            RiffContent::List {
                kind: Some(*b"WEBP"),
                subchunks: vec![
                    RiffChunk::new(
                        *b"VP8L",
                        RiffContent::Data(Bytes::from_static(b"\x2f\0\0\0\0pixels")),
                    ),
                    RiffChunk::new(*b"ICCP", RiffContent::Data(Bytes::from_static(b"ICCDATA"))),
                    RiffChunk::new(
                        *b"C2PA",
                        RiffContent::Data(Bytes::from_static(b"SECRETMANIFEST")),
                    ),
                ],
            },
        );
        let bytes = WebP::new(riff).unwrap().encoder().bytes().to_vec();
        let out = strip_image_metadata(&bytes, "image/webp");
        assert!(!contains(&out, b"SECRETMANIFEST"));
        assert!(contains(&out, b"ICCDATA"));
    }

    #[test]
    fn heif_with_zero_width_extents_is_rejected_quickly() {
        // iloc with all field widths zero and 65535 extents per item used to
        // spin for ~0.2 ms per item without consuming input.
        let mut ftyp = b"heic".to_vec();
        ftyp.extend_from_slice(&[0, 0, 0, 0]);
        ftyp.extend_from_slice(b"heic");
        let mut infe = vec![2, 0, 0, 0, 0, 1, 0, 0];
        infe.extend_from_slice(b"Exif");
        let mut iinf = vec![0, 0, 0, 0, 0, 1];
        iinf.extend_from_slice(&iso_box(b"infe", &infe));
        let items: u16 = 4000;
        let mut iloc = vec![0, 0, 0, 0, 0x00, 0x00];
        iloc.extend_from_slice(&items.to_be_bytes());
        for _ in 0..items {
            iloc.extend_from_slice(&[0, 1, 0, 0, 0xFF, 0xFF]);
        }
        let mut meta = vec![0, 0, 0, 0];
        meta.extend_from_slice(&iso_box(b"iinf", &iinf));
        meta.extend_from_slice(&iso_box(b"iloc", &iloc));
        let mut file = iso_box(b"ftyp", &ftyp);
        file.extend_from_slice(&iso_box(b"meta", &meta));

        let t = std::time::Instant::now();
        assert!(!is_strippable(&file, "image/heic"));
        assert!(
            t.elapsed() < std::time::Duration::from_secs(1),
            "{:?}",
            t.elapsed()
        );
    }

    // ---- WebP with a VP8X header (animation, alpha, ...) ---------------------

    const WEBP_ANIM: &[u8] = include_bytes!("../../tests/fixtures/meta/anim_meta.webp");
    const WEBP_ALPHA: &[u8] = include_bytes!("../../tests/fixtures/meta/alpha_lossy_meta.webp");

    fn webp_ids(b: &[u8]) -> Vec<[u8; 4]> {
        WebP::from_bytes(Bytes::copy_from_slice(b))
            .unwrap()
            .chunks()
            .iter()
            .map(|c| c.id())
            .collect()
    }

    fn vp8x_flags(b: &[u8]) -> u8 {
        let w = WebP::from_bytes(Bytes::copy_from_slice(b)).unwrap();
        w.chunk_by_id(*b"VP8X")
            .expect("VP8X chunk")
            .content()
            .data()
            .unwrap()[0]
    }

    #[test]
    fn animated_webp_stays_a_valid_animation_after_the_strip() {
        assert!(contains(WEBP_ANIM, b"SECRETMAKE") && contains(WEBP_ANIM, b"SECRETXMP"));
        let out = strip_image_metadata(WEBP_ANIM, "image/webp");
        assert!(!contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETXMP"));

        // The VP8X header (which marks the file as animated) must survive,
        // with only the EXIF/XMP flags cleared.
        let ids = webp_ids(&out);
        assert_eq!(ids[0], *b"VP8X");
        assert!(ids.contains(b"ANIM"));
        assert_eq!(ids.iter().filter(|i| **i == *b"ANMF").count(), 3);
        assert!(!ids.contains(b"XMP ") && !ids.contains(b"EXIF"));
        let flags = vp8x_flags(&out);
        assert!(flags & 0x02 != 0, "animation flag lost: {flags:#x}");
        assert!(flags & 0x04 == 0, "XMP flag still set: {flags:#x}");

        // An independent decoder still sees a 3-frame animation.
        use image::AnimationDecoder;
        let dec = image::codecs::webp::WebPDecoder::new(std::io::Cursor::new(&out[..])).unwrap();
        assert!(dec.has_animation());
        assert_eq!(
            dec.into_frames()
                .collect::<Result<Vec<_>, _>>()
                .unwrap()
                .len(),
            3
        );

        // (EXIF orientation is not read from animated WebP at all, before or
        // after, and renderers ignore it there.)
    }

    #[test]
    fn webp_with_alpha_keeps_its_vp8x_header() {
        assert!(contains(WEBP_ALPHA, b"SECRETMAKE"));
        let out = strip_image_metadata(WEBP_ALPHA, "image/webp");
        assert!(!contains(&out, b"SECRETMAKE") && !contains(&out, b"SECRETXMP"));
        let ids = webp_ids(&out);
        assert_eq!(ids[0], *b"VP8X");
        assert!(ids.contains(b"ALPH"));
        assert!(vp8x_flags(&out) & 0x10 != 0, "alpha flag lost");
        let img = image::load_from_memory_with_format(&out, image::ImageFormat::WebP).unwrap();
        assert!(img.color().has_alpha());
        // The orientation tag of a still image is kept.
        assert_eq!(read_orientation(&out, &FileExtension::WEBP), Some(6));
    }

    #[test]
    fn webp_strip_is_a_noop_in_size_terms_for_clean_animations() {
        // Stripping an already clean animation must not damage it either.
        let once = strip_image_metadata(WEBP_ANIM, "image/webp").into_owned();
        let twice = strip_image_metadata(&once, "image/webp");
        assert_eq!(webp_ids(&twice), webp_ids(&once));
    }

    // ---- JPEG 2000 codestream comments ---------------------------------------

    const JP2_COMMENT: &[u8] = include_bytes!("../../tests/fixtures/meta/jp2_comment.jp2");

    /// (marker, length) of the main-header segments of the first `jp2c` box.
    fn j2k_main_header(b: &[u8]) -> Vec<(u16, usize)> {
        let boxes = iso_boxes(b, 0, b.len()).unwrap();
        let cs = &b[boxes.iter().find(|x| &x.ty == b"jp2c").unwrap().payload..];
        assert_eq!(&cs[..2], &[0xFF, 0x4F]);
        let (mut pos, mut v) = (2usize, Vec::new());
        while be16(cs, pos).unwrap() != 0xFF90 {
            let m = be16(cs, pos).unwrap() as u16;
            let len = be16(cs, pos + 2).unwrap();
            v.push((m, len));
            pos += 2 + len;
        }
        v
    }

    #[test]
    fn jp2_codestream_comments_are_removed_and_the_file_stays_well_formed() {
        assert!(contains(JP2_COMMENT, b"SECRETCOMMENT"));
        assert!(j2k_main_header(JP2_COMMENT)
            .iter()
            .any(|(m, _)| *m == 0xFF64));
        let out = strip_image_metadata(JP2_COMMENT, "image/jp2");
        assert!(!contains(&out, b"SECRETCOMMENT"));
        // Still a valid box tiling whose codestream has no COM, and whose
        // other main-header segments (SIZ, COD, QCD) are untouched.
        let hdr = j2k_main_header(&out);
        assert!(!hdr.iter().any(|(m, _)| *m == 0xFF64), "{hdr:?}");
        let orig: Vec<_> = j2k_main_header(JP2_COMMENT)
            .into_iter()
            .filter(|(m, _)| *m != 0xFF64)
            .collect();
        assert_eq!(hdr, orig);
        // The jp2c box size matches its new payload.
        let boxes = iso_boxes(&out, 0, out.len()).unwrap();
        assert_eq!(boxes.last().unwrap().end, out.len());
        assert!(out.len() < JP2_COMMENT.len());
    }

    #[test]
    fn jp2_with_a_comment_in_a_tile_part_header_is_not_confidently_clean() {
        // SOT (Lsot 10, Isot 0, Psot 20, TPsot 0, TNsot 1), COM, SOD + 2 data bytes.
        let mut tile = vec![0xFF, 0x90, 0, 10, 0, 0, 0, 0, 0, 20, 0, 1];
        tile.extend_from_slice(&[0xFF, 0x64, 0, 4, 0, 1]); // COM
        tile.extend_from_slice(&[0xFF, 0x93, 0xAA, 0xBB]); // SOD + data
        let mut cs = vec![0xFF, 0x4F, 0xFF, 0x51, 0, 4, 0, 0];
        cs.extend_from_slice(&tile);
        cs.extend_from_slice(&[0xFF, 0xD9]);
        assert!(strip_j2k_comments(&cs).is_none());
    }
}
