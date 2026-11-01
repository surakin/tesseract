//! Sequential animated-WebP decoder exposed to C++ over the cxx bridge.
//!
//! Thin owner of libwebp's `WebPAnimDecoder` (from `libwebp-sys`, the same
//! libwebp that encodes `/myroomavatar` avatars). It exists for the macOS
//! canvas: ImageIO realizes frame N at a cost that grows with N, whereas
//! `WebPAnimDecoder` keeps a persistent compositing canvas across sequential
//! `next_frame` calls (see ui/macos/tk/canvas_cg_webp.cpp).
//!
//! Output is premultiplied BGRA (`MODE_bgrA`), which is what CoreGraphics
//! wants for `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little`.

use libwebp_sys as w;

pub struct WebpAnimDecoder {
    dec: *mut w::WebPAnimDecoder,
    /// libwebp keeps pointing into the compressed bytes, so they must outlive
    /// `dec`. Never resized after construction, so the heap pointer is stable.
    _data: Vec<u8>,
    info: w::WebPAnimInfo,
    /// Borrowed from libwebp's internal canvas; valid until the next
    /// `next_frame` / `reset` / drop.
    frame: *const u8,
    frame_len: usize,
    timestamp_ms: i32,
    /// Frames handed out since construction or the last `reset`.
    frames_taken: u32,
}

// Used by one C++ thread at a time; libwebp has no thread affinity.
unsafe impl Send for WebpAnimDecoder {}

impl Drop for WebpAnimDecoder {
    fn drop(&mut self) {
        if !self.dec.is_null() {
            unsafe { w::WebPAnimDecoderDelete(self.dec) };
        }
    }
}

/// True iff `bytes` parse as a (still or animated) WebP bitstream.
pub fn webp_is_data(bytes: &[u8]) -> bool {
    unsafe {
        w::WebPGetInfo(
            bytes.as_ptr(),
            bytes.len(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        ) != 0
    }
}

/// Animated-WebP canvas budget; matches tk::kMaxAnimatedDecodePixels.
/// WebPAnimDecoderNew allocates two full canvases up front, so this must be
/// checked from the header before calling it.
const MAX_ANIM_CANVAS_PIXELS: u64 = 4096 * 4096;

/// Frame cap for one animation; matches tk::kAnimDecodeMaxFrames. The header
/// can declare far more frames, and each `next_frame` composites a full canvas.
const MAX_ANIM_FRAMES: u32 = 200;

fn canvas_within_limit(w: i32, h: i32) -> bool {
    w > 0 && h > 0 && (w as u64) * (h as u64) <= MAX_ANIM_CANVAS_PIXELS
}

/// Never fails outright; check `valid()` (cxx `Result` would throw into C++).
pub fn webp_anim_decoder_new(bytes: &[u8]) -> Box<WebpAnimDecoder> {
    let mut d = Box::new(WebpAnimDecoder {
        dec: std::ptr::null_mut(),
        _data: bytes.to_vec(),
        info: unsafe { std::mem::zeroed() },
        frame: std::ptr::null(),
        frame_len: 0,
        timestamp_ms: 0,
        frames_taken: 0,
    });
    unsafe {
        let (mut cw, mut ch) = (0i32, 0i32);
        if w::WebPGetInfo(d._data.as_ptr(), d._data.len(), &mut cw, &mut ch) == 0
            || !canvas_within_limit(cw, ch)
        {
            return d;
        }
        let mut opts: w::WebPAnimDecoderOptions = std::mem::zeroed();
        if w::WebPAnimDecoderOptionsInit(&mut opts) == 0 {
            return d;
        }
        opts.color_mode = w::WEBP_CSP_MODE::MODE_bgrA;
        let data = w::WebPData {
            bytes: d._data.as_ptr(),
            size: d._data.len(),
        };
        let dec = w::WebPAnimDecoderNew(&data, &opts);
        if dec.is_null() {
            return d;
        }
        if w::WebPAnimDecoderGetInfo(dec, &mut d.info) == 0 {
            w::WebPAnimDecoderDelete(dec);
            return d;
        }
        d.dec = dec;
    }
    d
}

impl WebpAnimDecoder {
    pub fn valid(&self) -> bool {
        !self.dec.is_null()
    }

    pub fn canvas_width(&self) -> u32 {
        self.info.canvas_width
    }

    pub fn canvas_height(&self) -> u32 {
        self.info.canvas_height
    }

    pub fn frame_count(&self) -> u32 {
        self.info.frame_count.min(MAX_ANIM_FRAMES)
    }

    pub fn has_more_frames(&self) -> bool {
        !self.dec.is_null()
            && self.frames_taken < MAX_ANIM_FRAMES
            && unsafe { w::WebPAnimDecoderHasMoreFrames(self.dec) != 0 }
    }

    /// Decode the next frame into the decoder's canvas. On false the previous
    /// `frame_pixels` is invalidated (empty).
    pub fn next_frame(&mut self) -> bool {
        self.frame = std::ptr::null();
        self.frame_len = 0;
        if !self.has_more_frames() {
            return false;
        }
        let mut buf: *mut u8 = std::ptr::null_mut();
        let mut ts: std::ffi::c_int = 0;
        if unsafe { w::WebPAnimDecoderGetNext(self.dec, &mut buf, &mut ts) } == 0 || buf.is_null() {
            return false;
        }
        self.frame = buf;
        self.frame_len = self.info.canvas_width as usize * self.info.canvas_height as usize * 4;
        self.timestamp_ms = ts;
        self.frames_taken += 1;
        true
    }

    /// Premultiplied BGRA, `canvas_width * canvas_height * 4` bytes, of the
    /// frame last returned by `next_frame`. Empty if there is none.
    pub fn frame_pixels(&self) -> &[u8] {
        if self.frame.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.frame, self.frame_len) }
        }
    }

    /// Cumulative ms since the animation start at the end of the last frame
    /// (not that frame's own duration).
    pub fn frame_timestamp_ms(&self) -> i32 {
        self.timestamp_ms
    }

    pub fn reset(&mut self) {
        self.frame = std::ptr::null();
        self.frame_len = 0;
        self.timestamp_ms = 0;
        self.frames_taken = 0;
        if !self.dec.is_null() {
            unsafe { w::WebPAnimDecoderReset(self.dec) };
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn canvas_limit_matches_tk_animated_budget() {
        assert!(canvas_within_limit(4096, 4096));
        assert!(canvas_within_limit(1, 1));
        assert!(!canvas_within_limit(4097, 4096));
        assert!(!canvas_within_limit(16383, 16383));
        assert!(!canvas_within_limit(0, 10));
        assert!(!canvas_within_limit(-1, 10));
    }

    use super::*;

    fn solid_anim() -> Vec<u8> {
        let mut cfg = webp::WebPConfig::new().unwrap();
        cfg.lossless = 1;
        let frames: Vec<Vec<u8>> = [[200u8, 30, 30, 255], [10, 20, 30, 255], [0, 0, 255, 255]]
            .iter()
            .map(|c| c.repeat(16 * 16))
            .collect();
        let mut enc = webp::AnimEncoder::new(16, 16, &cfg);
        for (i, f) in frames.iter().enumerate() {
            enc.add_frame(webp::AnimFrame::from_rgba(f, 16, 16, i as i32 * 50));
        }
        enc.encode().to_vec()
    }

    #[test]
    fn decodes_sequentially_as_bgra_and_resets() {
        let bytes = solid_anim();
        assert!(webp_is_data(&bytes));
        assert!(!webp_is_data(b"not a webp"));
        let mut d = webp_anim_decoder_new(&bytes);
        assert!(d.valid());
        assert_eq!(
            (d.canvas_width(), d.canvas_height(), d.frame_count()),
            (16, 16, 3)
        );
        assert!(d.next_frame());
        assert_eq!(d.frame_pixels().len(), 16 * 16 * 4);
        // RGBA (200,30,30) → premultiplied BGRA (30,30,200,255), opaque.
        assert_eq!(&d.frame_pixels()[..4], &[30, 30, 200, 255]);
        assert!(d.next_frame() && d.next_frame());
        assert_eq!(d.frame_timestamp_ms(), 150);
        assert!(!d.has_more_frames());
        assert!(!d.next_frame());
        assert!(d.frame_pixels().is_empty());
        d.reset();
        assert!(d.has_more_frames());
        assert!(d.next_frame());
        assert_eq!(&d.frame_pixels()[..4], &[30, 30, 200, 255]);
    }

    #[test]
    fn garbage_is_invalid() {
        let d = webp_anim_decoder_new(b"garbage");
        assert!(!d.valid());
        assert!(!d.has_more_frames());
    }

    #[test]
    fn decoding_stops_at_the_frame_cap() {
        // One more frame than the cap, so the header declares more than is decoded.
        let mut cfg = webp::WebPConfig::new().unwrap();
        cfg.lossless = 1;
        // Each frame differs from the last; the encoder would merge repeats.
        let frames: Vec<Vec<u8>> = (0..=MAX_ANIM_FRAMES as i32)
            .map(|i| [i as u8, (i >> 8) as u8, 0, 255].repeat(4 * 4))
            .collect();
        let mut enc = webp::AnimEncoder::new(4, 4, &cfg);
        for (i, f) in frames.iter().enumerate() {
            enc.add_frame(webp::AnimFrame::from_rgba(f, 4, 4, i as i32 * 10));
        }
        let bytes = enc.encode().to_vec();
        let mut d = webp_anim_decoder_new(&bytes);
        assert!(d.valid());
        assert_eq!(d.frame_count(), MAX_ANIM_FRAMES);
        let mut taken = 0;
        while d.next_frame() {
            taken += 1;
        }
        assert_eq!(taken, MAX_ANIM_FRAMES);
        assert!(!d.has_more_frames());
        d.reset();
        assert!(d.next_frame());
    }
}
