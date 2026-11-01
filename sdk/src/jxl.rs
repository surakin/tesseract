//! JPEG XL decoder exposed to C++ over the cxx bridge.
//!
//! Pure-Rust `jxl-oxide`; output is straight (non-premultiplied) RGBA8 in
//! sRGB, orientation applied, downscaled to fit `max_w x max_h` so C++ never
//! holds an oversized intermediate. Frames are rendered lazily, one per
//! `next_frame`.

use jxl_oxide::{AllocTracker, EnumColourEncoding, JxlImage, RenderingIntent};

/// Mirrors tk::kMaxStillDecodePixels / kMaxAnimatedDecodePixels / kMaxDecodeSide.
const MAX_STILL_PIXELS: u64 = 100_000_000;
const MAX_ANIM_PIXELS: u64 = 4096 * 4096;
const MAX_SIDE: u32 = 32768;
/// Mirrors tk::kAnimDecodeMaxFrames.
const MAX_FRAMES: usize = 200;
/// Hard ceiling on jxl-oxide's internal allocations.
const ALLOC_LIMIT: usize = 1 << 30;

/// Runs a decode step, turning a panic into `fallback`. jxl-oxide parses
/// untrusted bytes, and a panic unwinding into the cxx boundary would abort
/// the whole process; a malformed image must only fail to decode.
fn guarded<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
}

pub struct JxlDecoder {
    image: Option<JxlImage>,
    out_w: u32,
    out_h: u32,
    /// Source (post-orientation, pre-downscale) size every frame must match.
    src_w: usize,
    src_h: usize,
    frames: usize,
    next: usize,
    /// Ticks to ms: delay_ms = ticks * 1000 * tps_den / tps_num.
    tps_num: u64,
    tps_den: u64,
    pixels: Vec<u8>,
    delay_ms: i32,
}

/// True for a bare codestream (`FF 0A`) or an ISOBMFF container (`JXL ` box).
pub fn jxl_is_data(b: &[u8]) -> bool {
    b.starts_with(&[0xFF, 0x0A]) || b.starts_with(b"\0\0\0\x0cJXL \r\n\x87\n")
}

/// True when the JXL header declares an animation (frame count is not read).
/// Header-only: parses the image header, never renders a frame.
pub fn jxl_is_animated(bytes: &[u8]) -> bool {
    guarded(false, || is_animated_inner(bytes))
}

fn is_animated_inner(bytes: &[u8]) -> bool {
    use jxl_oxide::InitializeResult;
    if !jxl_is_data(bytes) {
        return false;
    }
    let mut img = JxlImage::builder().build_uninit();
    if img.feed_bytes(bytes).is_err() {
        return false;
    }
    match img.try_init() {
        Ok(InitializeResult::Initialized(image)) => {
            image.image_header().metadata.animation.is_some()
        }
        _ => false,
    }
}

fn within_budget(w: u32, h: u32, frames: usize) -> bool {
    if w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE {
        return false;
    }
    let px = w as u64 * h as u64;
    px <= if frames > 1 {
        MAX_ANIM_PIXELS
    } else {
        MAX_STILL_PIXELS
    }
}

/// Never fails outright; check `valid()` (cxx `Result` would throw into C++).
pub fn jxl_decoder_new(bytes: &[u8], max_w: u32, max_h: u32) -> Box<JxlDecoder> {
    guarded(empty_decoder(), || decoder_new_inner(bytes, max_w, max_h))
}

fn empty_decoder() -> Box<JxlDecoder> {
    Box::new(JxlDecoder {
        image: None,
        out_w: 0,
        out_h: 0,
        src_w: 0,
        src_h: 0,
        frames: 0,
        next: 0,
        tps_num: 1,
        tps_den: 1,
        pixels: Vec::new(),
        delay_ms: 0,
    })
}

fn decoder_new_inner(bytes: &[u8], max_w: u32, max_h: u32) -> Box<JxlDecoder> {
    let mut d = empty_decoder();
    if !jxl_is_data(bytes) {
        return d;
    }
    let Ok(mut image) = JxlImage::builder()
        .alloc_tracker(AllocTracker::with_limit(ALLOC_LIMIT))
        .read(std::io::Cursor::new(bytes))
    else {
        return d;
    };
    let frames = image.num_loaded_keyframes().min(MAX_FRAMES);
    let (w, h) = (image.width(), image.height());
    if frames == 0 || !within_budget(w, h, frames) {
        return d;
    }
    image.request_color_encoding(EnumColourEncoding::srgb(RenderingIntent::Relative));
    // width()/height() already include the header orientation.
    let (tw, th) = fit(w, h, max_w.max(1), max_h.max(1));
    if let Some(a) = image.image_header().metadata.animation.as_ref() {
        d.tps_num = (a.tps_numerator as u64).max(1);
        d.tps_den = (a.tps_denominator as u64).max(1);
    }
    d.out_w = tw;
    d.out_h = th;
    d.src_w = w as usize;
    d.src_h = h as usize;
    d.frames = frames;
    d.image = Some(image);
    d
}

/// Largest size <= (max_w, max_h) with the source aspect ratio; never upscales.
fn fit(w: u32, h: u32, max_w: u32, max_h: u32) -> (u32, u32) {
    if w <= max_w && h <= max_h {
        return (w, h);
    }
    let s = (max_w as f64 / w as f64).min(max_h as f64 / h as f64);
    (
        ((w as f64 * s).round() as u32).max(1),
        ((h as f64 * s).round() as u32).max(1),
    )
}

impl JxlDecoder {
    pub fn valid(&self) -> bool {
        self.image.is_some()
    }

    pub fn width(&self) -> u32 {
        self.out_w
    }

    pub fn height(&self) -> u32 {
        self.out_h
    }

    pub fn frame_count(&self) -> u32 {
        self.frames as u32
    }

    pub fn has_more_frames(&self) -> bool {
        self.image.is_some() && self.next < self.frames
    }

    /// Render the next keyframe into `frame_pixels`. On false the previous
    /// pixels are cleared.
    pub fn next_frame(&mut self) -> bool {
        self.pixels.clear();
        if !self.has_more_frames() {
            return false;
        }
        if guarded(false, || self.next_frame_inner()) {
            return true;
        }
        // Failed or panicked: end the animation, leave no partial frame.
        self.next = self.frames;
        self.pixels.clear();
        false
    }

    fn next_frame_inner(&mut self) -> bool {
        let image = self.image.as_ref().unwrap();
        let Ok(render) = image.render_frame(self.next) else {
            return false;
        };
        let mut stream = render.stream();
        let (sw, sh, ch) = (
            stream.width() as usize,
            stream.height() as usize,
            stream.channels() as usize,
        );
        // Only 1..=4 interleaved channels map to RGBA; refuse before allocating.
        if !(1..=4).contains(&ch) || sw != self.src_w || sh != self.src_h {
            return false;
        }
        let mut raw = vec![0u8; sw * sh * ch];
        stream.write_to_buffer::<u8>(&mut raw);
        let Some(rgba) = to_rgba(&raw, ch, sw * sh) else {
            return false;
        };
        drop(raw); // keep peak memory at two full-size buffers, not three
        self.pixels = box_resize(rgba, sw, sh, self.out_w as usize, self.out_h as usize);
        self.delay_ms = ((render.duration() as u64 * 1000 * self.tps_den) / self.tps_num)
            .min(i32::MAX as u64) as i32;
        self.next += 1;
        true
    }

    /// Straight RGBA8, `width * height * 4` bytes of the frame last returned
    /// by `next_frame`. Empty if there is none.
    pub fn frame_pixels(&self) -> &[u8] {
        &self.pixels
    }

    /// This frame's own display duration in ms (0 for a still).
    pub fn frame_delay_ms(&self) -> i32 {
        self.delay_ms
    }

    pub fn reset(&mut self) {
        self.pixels.clear();
        self.next = 0;
        self.delay_ms = 0;
    }
}

/// Expand 1..=4 interleaved channels to RGBA. >4 (CMYK / spot) is unsupported.
fn to_rgba(raw: &[u8], ch: usize, npx: usize) -> Option<Vec<u8>> {
    if !(1..=4).contains(&ch) || raw.len() != npx * ch {
        return None;
    }
    let mut out = Vec::with_capacity(npx * 4);
    for p in raw.chunks_exact(ch) {
        match ch {
            1 => out.extend_from_slice(&[p[0], p[0], p[0], 255]),
            2 => out.extend_from_slice(&[p[0], p[0], p[0], p[1]]),
            3 => out.extend_from_slice(&[p[0], p[1], p[2], 255]),
            _ => out.extend_from_slice(p),
        }
    }
    Some(out)
}

/// Area-average resize of RGBA8 (alpha-weighted so transparent pixels don't
/// bleed colour). Returns the input unchanged when sizes match.
fn box_resize(src: Vec<u8>, sw: usize, sh: usize, dw: usize, dh: usize) -> Vec<u8> {
    if sw == dw && sh == dh {
        return src; // no copy
    }
    let mut out = vec![0u8; dw * dh * 4];
    for dy in 0..dh {
        let y0 = dy * sh / dh;
        let y1 = ((dy + 1) * sh / dh).max(y0 + 1).min(sh);
        for dx in 0..dw {
            let x0 = dx * sw / dw;
            let x1 = ((dx + 1) * sw / dw).max(x0 + 1).min(sw);
            let (mut r, mut g, mut b, mut a, mut n) = (0u64, 0u64, 0u64, 0u64, 0u64);
            for y in y0..y1 {
                for x in x0..x1 {
                    let i = (y * sw + x) * 4;
                    let al = src[i + 3] as u64;
                    r += src[i] as u64 * al;
                    g += src[i + 1] as u64 * al;
                    b += src[i + 2] as u64 * al;
                    a += al;
                    n += 1;
                }
            }
            let o = (dy * dw + dx) * 4;
            if a > 0 {
                out[o] = (r / a) as u8;
                out[o + 1] = (g / a) as u8;
                out[o + 2] = (b / a) as u8;
            }
            out[o + 3] = (a / n.max(1)) as u8;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    const RGB: &[u8] = include_bytes!("../tests/fixtures/jxl/still_rgb.jxl");
    const GRAY: &[u8] = include_bytes!("../tests/fixtures/jxl/still_gray.jxl");
    const ANIM: &[u8] = include_bytes!("../tests/fixtures/jxl/anim3.jxl");
    const ROT: &[u8] = include_bytes!("../tests/fixtures/jxl/rotated.jxl");

    #[test]
    fn sniffs_codestream_and_container() {
        assert!(jxl_is_data(RGB));
        assert!(jxl_is_data(&[0xFF, 0x0A, 0x00]));
        assert!(jxl_is_data(b"\0\0\0\x0cJXL \r\n\x87\n"));
        assert!(!jxl_is_data(b"\x89PNG\r\n\x1a\n"));
        assert!(!jxl_is_data(b""));
    }

    #[test]
    fn container_wrapped_file_decodes() {
        let c = include_bytes!("../tests/fixtures/jxl/still_container.jxl");
        assert!(c.starts_with(b"\0\0\0\x0cJXL "));
        let mut d = jxl_decoder_new(c, 4096, 4096);
        assert!(d.valid());
        assert_eq!((d.width(), d.height()), (8, 4));
        assert!(d.next_frame());
        assert_eq!(d.frame_pixels().len(), 8 * 4 * 4);
    }

    #[test]
    fn header_only_animation_probe() {
        assert!(jxl_is_animated(ANIM));
        assert!(!jxl_is_animated(RGB));
        assert!(!jxl_is_animated(include_bytes!(
            "../tests/fixtures/jxl/still_container.jxl"
        )));
        assert!(!jxl_is_animated(b"garbage"));
        assert!(!jxl_is_animated(&ANIM[..4]));
    }

    #[test]
    fn still_rgb_decodes_to_straight_rgba() {
        let mut d = jxl_decoder_new(RGB, 4096, 4096);
        assert!(d.valid());
        assert_eq!((d.width(), d.height(), d.frame_count()), (8, 4, 1));
        assert!(d.next_frame());
        assert_eq!(d.frame_pixels().len(), 8 * 4 * 4);
        let p = d.frame_pixels();
        assert!((p[0] as i32 - 200).abs() <= 2 && p[3] == 255); // left px red
        assert!((p[7 * 4 + 2] as i32 - 255).abs() <= 2); // right px blue
        assert!(!d.next_frame());
    }

    #[test]
    fn grayscale_expands_to_rgba() {
        let mut d = jxl_decoder_new(GRAY, 4096, 4096);
        assert!(d.valid() && d.next_frame());
        let p = &d.frame_pixels()[..4];
        assert!((p[0] as i32 - 100).abs() <= 2);
        assert_eq!((p[0], p[1], p[2], p[3]), (p[0], p[0], p[0], 255));
    }

    #[test]
    fn orientation_is_applied() {
        let d = jxl_decoder_new(ROT, 4096, 4096);
        assert!(d.valid());
        assert_eq!((d.width(), d.height()), (4, 8));
    }

    #[test]
    fn animation_frames_delays_and_reset() {
        let mut d = jxl_decoder_new(ANIM, 4096, 4096);
        assert!(d.valid());
        assert_eq!((d.width(), d.height(), d.frame_count()), (16, 16, 3));
        for i in 0..3 {
            assert!(d.next_frame(), "frame {i}");
            assert_eq!(d.frame_delay_ms(), 50);
        }
        assert!(!d.has_more_frames() && !d.next_frame());
        assert!(d.frame_pixels().is_empty());
        d.reset();
        assert!(d.has_more_frames() && d.next_frame());
    }

    #[test]
    fn downscales_to_fit_preserving_aspect() {
        let mut d = jxl_decoder_new(ANIM, 8, 8); // 16x16 -> 8x8
        assert!(d.valid());
        assert_eq!((d.width(), d.height()), (8, 8));
        assert!(d.next_frame());
        assert_eq!(d.frame_pixels().len(), 8 * 8 * 4);
    }

    #[test]
    fn a_panic_becomes_the_fallback() {
        assert!(!guarded(false, || -> bool { panic!("decoder bug") }));
        assert_eq!(guarded(7, || 3), 3);
    }

    #[test]
    fn corrupted_bytes_never_panic() {
        // Flip every byte of valid files in turn; each must yield a decoder
        // (valid or not) and drain without panicking.
        for src in [
            RGB,
            ANIM,
            ROT,
            include_bytes!("../tests/fixtures/jxl/still_container.jxl"),
        ] {
            for i in 0..src.len() {
                let mut b = src.to_vec();
                b[i] ^= 0xFF;
                let mut d = jxl_decoder_new(&b, 64, 64);
                while d.next_frame() {}
                let _ = jxl_is_animated(&b);
            }
        }
    }

    #[test]
    fn garbage_and_truncated_are_invalid() {
        assert!(!jxl_decoder_new(b"garbage", 100, 100).valid());
        assert!(!jxl_decoder_new(&RGB[..RGB.len() / 2], 100, 100).valid());
        assert!(!jxl_decoder_new(b"", 100, 100).valid());
    }

    #[test]
    fn oversize_header_rejected_before_decode() {
        assert!(!within_budget(60000, 60000, 1)); // > 100 MP still
        assert!(!within_budget(5000, 5000, 2)); // > 16 MP animated
        assert!(within_budget(4096, 4096, 2));
        assert!(!within_budget(40000, 10, 1)); // > 32768 side
    }
}
