//! Turn an arbitrary fetched image into a square avatar (`/myroomavatar <url>`).
//!
//! Pure bytes-in/bytes-out so it is unit-testable without a client. The input
//! format is sniffed from the bytes (never the server's Content-Type). The
//! image is center-cropped to a square and downscaled to at most `max_side`
//! (never upscaled). Animated GIF / animated WebP / APNG stay animated and are
//! re-encoded as animated WebP; stills become JPEG (opaque) or PNG (alpha).

use image::codecs::{gif::GifDecoder, png::PngDecoder, webp::WebPDecoder};
use image::imageops::{self, FilterType};
use image::{AnimationDecoder, DynamicImage, ImageDecoder, ImageFormat, Limits, RgbaImage};
use std::io::Cursor;

/// Source images larger than this on either side are rejected up-front.
const MAX_SRC_DIM: u32 = 16384;
/// Cap on decoder allocations (decompression-bomb guard). Sized to take a 48 MP
/// phone photo; the avatar itself is at most `max_side` square, so the output
/// needs far less.
const MAX_ALLOC_BYTES: u64 = 256 * 1024 * 1024;
/// Cap on animation length; frames are held (already downscaled) in memory.
const MAX_FRAMES: usize = 200;
/// Cap on total *source* pixels decoded across an animation.
const MAX_TOTAL_SRC_PIXELS: u64 = 300_000_000;
/// GIF-style "0/1 cs" delays are rendered as 100 ms by browsers; match that.
const MIN_FRAME_DELAY_MS: u32 = 20;
const DEFAULT_FRAME_DELAY_MS: u32 = 100;
/// Animated output above this is re-encoded at the next lower quality.
const MAX_ANIM_BYTES: usize = 4 * 1024 * 1024;
const ANIM_QUALITIES: [f32; 3] = [80.0, 55.0, 30.0];
const JPEG_QUALITY: u8 = 90;

fn limits() -> Limits {
    let mut l = Limits::default();
    l.max_image_width = Some(MAX_SRC_DIM);
    l.max_image_height = Some(MAX_SRC_DIM);
    l.max_alloc = Some(MAX_ALLOC_BYTES);
    l
}

fn decode_err(e: image::ImageError) -> String {
    format!("could not decode image: {e}")
}

/// Center-crop to a square and downscale to at most `max_side`. A source that
/// is already square is taken as it is, without a crop copy.
fn square(img: RgbaImage, max_side: u32) -> RgbaImage {
    let (w, h) = img.dimensions();
    let s = w.min(h);
    let cropped = if w == h {
        img
    } else {
        imageops::crop_imm(&img, (w - s) / 2, (h - s) / 2, s, s).to_image()
    };
    let target = s.min(max_side);
    if target < s {
        imageops::resize(&cropped, target, target, FilterType::Lanczos3)
    } else {
        cropped
    }
}

fn delay_ms(d: image::Delay) -> u32 {
    let (n, den) = d.numer_denom_ms();
    let ms = if den == 0 { 0 } else { (n + den / 2) / den };
    if ms < MIN_FRAME_DELAY_MS {
        DEFAULT_FRAME_DELAY_MS
    } else {
        ms
    }
}

struct Anim {
    frames: Vec<(RgbaImage, u32)>,
}

fn collect_frames(frames: image::Frames<'_>, max_side: u32) -> Result<Anim, String> {
    let mut out: Vec<(RgbaImage, u32)> = Vec::new();
    let mut total_px: u64 = 0;
    let mut canvas: Option<(u32, u32)> = None;
    for frame in frames {
        let frame = frame.map_err(decode_err)?;
        let dims = frame.buffer().dimensions();
        match canvas {
            None => canvas = Some(dims),
            Some(c) if c != dims => return Err("animation frames differ in size".into()),
            _ => {}
        }
        total_px += u64::from(dims.0) * u64::from(dims.1);
        if out.len() >= MAX_FRAMES || total_px > MAX_TOTAL_SRC_PIXELS {
            return Err(format!("animation is too long (max {MAX_FRAMES} frames)"));
        }
        let delay = delay_ms(frame.delay());
        out.push((square(frame.into_buffer(), max_side), delay));
    }
    if out.is_empty() {
        return Err("image has no frames".into());
    }
    Ok(Anim { frames: out })
}

/// Frames of `bytes` when it is a multi-frame animation; `None` for stills.
fn animation(bytes: &[u8], fmt: ImageFormat, max_side: u32) -> Result<Option<Anim>, String> {
    let anim = match fmt {
        ImageFormat::Gif => {
            let mut d = GifDecoder::new(Cursor::new(bytes)).map_err(decode_err)?;
            d.set_limits(limits()).map_err(decode_err)?;
            collect_frames(d.into_frames(), max_side)?
        }
        ImageFormat::WebP => {
            let mut d = WebPDecoder::new(Cursor::new(bytes)).map_err(decode_err)?;
            if !d.has_animation() {
                return Ok(None);
            }
            d.set_limits(limits()).map_err(decode_err)?;
            collect_frames(d.into_frames(), max_side)?
        }
        ImageFormat::Png => {
            let mut d = PngDecoder::new(Cursor::new(bytes)).map_err(decode_err)?;
            if !d.is_apng().map_err(decode_err)? {
                return Ok(None);
            }
            d.set_limits(limits()).map_err(decode_err)?;
            collect_frames(d.apng().map_err(decode_err)?.into_frames(), max_side)?
        }
        _ => return Ok(None),
    };
    // A one-frame GIF/WebP/APNG is just a still; let the still path pick the
    // output format (it re-decodes, which is cheap for a single frame).
    Ok(if anim.frames.len() > 1 {
        Some(anim)
    } else {
        None
    })
}

fn encode_animated(anim: &Anim) -> Result<Vec<u8>, String> {
    let side = anim.frames[0].0.width();
    let mut best: Option<Vec<u8>> = None;
    for q in ANIM_QUALITIES {
        let mut cfg = webp::WebPConfig::new().map_err(|_| "webp config init failed".to_string())?;
        cfg.lossless = 0;
        cfg.quality = q;
        let mut enc = webp::AnimEncoder::new(side, side, &cfg);
        enc.set_loop_count(0);
        let mut ts: i32 = 0;
        for (img, delay) in &anim.frames {
            enc.add_frame(webp::AnimFrame::from_rgba(img.as_raw(), side, side, ts));
            ts = ts.saturating_add(*delay as i32);
        }
        let data = enc
            .try_encode()
            .map_err(|e| format!("webp encode failed: {e:?}"))?
            .to_vec();
        let fits = data.len() <= MAX_ANIM_BYTES;
        best = Some(data);
        if fits {
            break;
        }
    }
    best.ok_or_else(|| "webp encode failed".to_string())
}

fn encode_still(img: DynamicImage, max_side: u32) -> Result<(Vec<u8>, &'static str), String> {
    let sq = square(img.into_rgba8(), max_side);
    let opaque = sq.pixels().all(|p| p.0[3] == 255);
    let mut out = Vec::new();
    if opaque {
        let rgb = DynamicImage::ImageRgba8(sq).to_rgb8();
        let enc = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, JPEG_QUALITY);
        rgb.write_with_encoder(enc).map_err(decode_err)?;
        Ok((out, "image/jpeg"))
    } else {
        sq.write_with_encoder(image::codecs::png::PngEncoder::new(&mut out))
            .map_err(decode_err)?;
        Ok((out, "image/png"))
    }
}

/// Produce a square avatar of side `min(source short side, max_side)`.
/// Returns the encoded bytes and their MIME type.
pub(crate) fn make_square_avatar(
    bytes: &[u8],
    max_side: u32,
) -> Result<(Vec<u8>, &'static str), String> {
    let fmt = image::guess_format(bytes).map_err(|_| "URL is not an image".to_string())?;
    if !matches!(
        fmt,
        ImageFormat::Png | ImageFormat::Jpeg | ImageFormat::Gif | ImageFormat::WebP
    ) {
        return Err("unsupported image format (need PNG, JPEG, GIF or WebP)".into());
    }
    if let Some(anim) = animation(bytes, fmt, max_side)? {
        return Ok((encode_animated(&anim)?, "image/webp"));
    }
    let mut reader = image::ImageReader::with_format(Cursor::new(bytes), fmt);
    reader.limits(limits());
    let mut decoder = reader.into_decoder().map_err(decode_err)?;
    let orientation = decoder.orientation().map_err(decode_err)?;
    let mut img = DynamicImage::from_decoder(decoder).map_err(decode_err)?;
    img.apply_orientation(orientation);
    encode_still(img, max_side)
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{codecs::gif::GifEncoder, Delay, Frame, Rgba};

    fn png_of(w: u32, h: u32, alpha: u8) -> Vec<u8> {
        let img = RgbaImage::from_pixel(w, h, Rgba([200, 30, 30, alpha]));
        let mut out = Vec::new();
        img.write_with_encoder(image::codecs::png::PngEncoder::new(&mut out))
            .unwrap();
        out
    }

    fn dims(bytes: &[u8]) -> (u32, u32) {
        image::load_from_memory(bytes)
            .unwrap()
            .to_rgba8()
            .dimensions()
    }

    #[test]
    fn large_landscape_becomes_512_square_jpeg() {
        let (out, mime) = make_square_avatar(&png_of(800, 600, 255), 512).unwrap();
        assert_eq!(mime, "image/jpeg");
        assert_eq!(dims(&out), (512, 512));
    }

    #[test]
    fn small_image_is_cropped_not_upscaled() {
        let (out, _) = make_square_avatar(&png_of(100, 60, 255), 512).unwrap();
        assert_eq!(dims(&out), (60, 60));
    }

    #[test]
    fn alpha_is_kept_as_png() {
        let (out, mime) = make_square_avatar(&png_of(64, 64, 128), 512).unwrap();
        assert_eq!(mime, "image/png");
        assert_eq!(dims(&out), (64, 64));
    }

    #[test]
    fn garbage_is_not_an_image() {
        let err = make_square_avatar(b"<html>nope</html>", 512).unwrap_err();
        assert_eq!(err, "URL is not an image");
    }

    #[test]
    fn animated_gif_stays_animated_webp() {
        let mut gif = Vec::new();
        {
            let mut enc = GifEncoder::new(&mut gif);
            for shade in [10u8, 120, 240] {
                let img = RgbaImage::from_pixel(700, 500, Rgba([shade, 50, 50, 255]));
                enc.encode_frame(Frame::from_parts(
                    img,
                    0,
                    0,
                    Delay::from_numer_denom_ms(200, 1),
                ))
                .unwrap();
            }
        }
        let (out, mime) = make_square_avatar(&gif, 512).unwrap();
        assert_eq!(mime, "image/webp");
        let dec = WebPDecoder::new(Cursor::new(&out[..])).unwrap();
        assert!(dec.has_animation());
        assert_eq!(dec.dimensions(), (500, 500));
        let frames = dec.into_frames().collect_frames().unwrap();
        assert_eq!(frames.len(), 3);
        assert_eq!(delay_ms(frames[0].delay()), 200);
    }

    #[test]
    fn a_square_source_is_downscaled_without_a_crop() {
        let img = RgbaImage::from_pixel(64, 64, Rgba([1, 2, 3, 255]));
        let out = square(img, 32);
        assert_eq!(out.dimensions(), (32, 32));
        assert_eq!(out.get_pixel(0, 0).0, [1, 2, 3, 255]);
    }
}
