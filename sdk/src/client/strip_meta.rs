//! Lossless removal of privacy-sensitive metadata (EXIF/GPS, XMP, IPTC,
//! text comments) from JPEG, PNG and WebP uploads. Pixel data and ICC
//! profiles are untouched; only the EXIF Orientation tag is kept so photos
//! still display upright. Anything unparseable is returned unchanged.

use img_parts::jpeg::{markers, Jpeg};
use img_parts::png::Png;
use img_parts::webp::{WebP, CHUNK_XMP};
use img_parts::{Bytes, ImageEXIF};
use little_exif::exif_tag::ExifTag;
use little_exif::filetype::FileExtension;
use little_exif::metadata::Metadata;
use std::borrow::Cow;

const ICC_PREFIX: &[u8] = b"ICC_PROFILE\0";
const PNG_TEXT_CHUNKS: [[u8; 4]; 4] = [*b"tEXt", *b"zTXt", *b"iTXt", *b"tIME"];

pub(super) fn strip_image_metadata<'a>(bytes: &'a [u8], mime: &str) -> Cow<'a, [u8]> {
    let ext = match mime {
        "image/jpeg" | "image/jpg" => FileExtension::JPEG,
        "image/png" => FileExtension::PNG { as_zTXt_chunk: false },
        "image/webp" => FileExtension::WEBP,
        _ => return Cow::Borrowed(bytes),
    };
    match strip(bytes, &ext) {
        Some(out) => Cow::Owned(out),
        None => Cow::Borrowed(bytes),
    }
}

fn strip(bytes: &[u8], ext: &FileExtension) -> Option<Vec<u8>> {
    // PNG orientation is never emitted by cameras, and little_exif can only
    // write it as a non-standard zTXt block, so PNG is stripped outright.
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
        markers::APP1 | markers::APP3..=markers::APP13 | markers::APP15 | markers::COM => false,
        markers::APP2 => s.contents().starts_with(ICC_PREFIX),
        _ => true,
    });
    Some(jpeg.encoder().bytes().to_vec())
}

fn strip_png(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut png = Png::from_bytes(Bytes::copy_from_slice(bytes)).ok()?;
    for kind in PNG_TEXT_CHUNKS {
        png.remove_chunks_by_type(kind);
    }
    png.set_exif(None);
    Some(png.encoder().bytes().to_vec())
}

fn strip_webp(bytes: &[u8]) -> Option<Vec<u8>> {
    let mut webp = WebP::from_bytes(Bytes::copy_from_slice(bytes)).ok()?;
    webp.remove_chunks_by_id(CHUNK_XMP);
    // Also re-derives the VP8X flags after the EXIF chunk is dropped.
    webp.set_exif(None);
    Some(webp.encoder().bytes().to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;
    use img_parts::jpeg::JpegSegment;
    use img_parts::png::PngChunk;
    use img_parts::riff::{RiffChunk, RiffContent};

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
            JpegSegment::new_with_contents(markers::APP0, Bytes::from_static(b"JFIF\0\x01\x01\0\0\x01\0\x01\0\0")),
            JpegSegment::new_with_contents(
                markers::APP1,
                Bytes::from_static(b"http://ns.adobe.com/xap/1.0/\0<x:xmpmeta>SECRETXMP"),
            ),
            JpegSegment::new_with_contents(markers::APP13, Bytes::from_static(b"Photoshop 3.0\0SECRETIPTC")),
            JpegSegment::new_with_contents(markers::COM, Bytes::from_static(b"SECRETCOMMENT")),
            JpegSegment::new_with_contents(markers::APP2, Bytes::from_static(b"ICC_PROFILE\0\x01\x01PROFILE")),
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
            0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, b'I', b'E', b'N', b'D', 0xAE, 0x42, 0x60, 0x82,
        ]))
        .unwrap();
        let end = png.chunks().len() - 1;
        png.chunks_mut().insert(end, PngChunk::new(*b"tEXt", Bytes::from_static(b"Comment\0SECRETTEXT")));
        png.chunks_mut().insert(end, PngChunk::new(*b"IDAT", Bytes::from_static(b"pixels")));
        png.encoder().bytes().to_vec()
    }

    fn webp_fixture() -> Vec<u8> {
        let riff = RiffChunk::new(
            *b"RIFF",
            RiffContent::List {
                kind: Some(*b"WEBP"),
                subchunks: vec![
                    RiffChunk::new(*b"VP8L", RiffContent::Data(Bytes::from_static(b"\x2f\0\0\0\0pixels"))),
                    RiffChunk::new(CHUNK_XMP, RiffContent::Data(Bytes::from_static(b"SECRETXMP"))),
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
        for secret in [&b"SECRETMAKE"[..], b"SECRETXMP", b"SECRETIPTC", b"SECRETCOMMENT"] {
            assert!(!contains(&out, secret), "{}", String::from_utf8_lossy(secret));
        }
        assert!(contains(&out, b"ICC_PROFILE") && contains(&out, &[0xAB, 0xCD]));
        assert_eq!(read_orientation(&out, &FileExtension::JPEG), Some(6));
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
        let ext = FileExtension::PNG { as_zTXt_chunk: false };
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

    #[test]
    fn garbage_and_unknown_mime_pass_through() {
        let junk = b"definitely not an image";
        for mime in ["image/jpeg", "image/png", "image/webp", "image/gif", "video/mp4"] {
            assert!(matches!(strip_image_metadata(junk, mime), Cow::Borrowed(_)), "{mime}");
        }
        let jpeg = jpeg_fixture();
        let _ = strip_image_metadata(&jpeg[..20], "image/jpeg");
    }
}
