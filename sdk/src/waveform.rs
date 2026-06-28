use audiopus::coder::Decoder;
use audiopus::{Channels, SampleRate};

/// Decode an Ogg/Opus byte buffer and compute an MSC1767-compatible waveform.
/// Returns up to 200 amplitude samples in the range 0..=1024. Returns an
/// empty vec when the input is not valid Ogg/Opus or contains no audio frames.
pub fn compute_waveform_from_ogg(bytes: &[u8]) -> Vec<u16> {
    let cursor = std::io::Cursor::new(bytes);
    let mut reader = ogg::reading::PacketReader::new(cursor);

    let mut decoder = match Decoder::new(SampleRate::Hz48000, Channels::Mono) {
        Ok(d) => d,
        Err(_) => return vec![],
    };

    // 120 ms at 48 kHz is the maximum Opus frame size.
    let mut pcm = vec![0i16; 5760];
    let mut frame_peaks: Vec<f32> = Vec::new();
    let mut header_count = 0usize;

    loop {
        match reader.read_packet() {
            Ok(Some(pkt)) => {
                // First two packets are OpusHead and OpusTags headers.
                if header_count < 2 {
                    header_count += 1;
                    continue;
                }
                let data: &[u8] = &pkt.data;
                if let Ok(n) = decoder.decode(Some(data), &mut pcm, false) {
                    if n > 0 {
                        let peak = pcm[..n].iter().map(|s| s.unsigned_abs()).max().unwrap_or(0);
                        frame_peaks.push(peak as f32 / i16::MAX as f32);
                    }
                }
            }
            Ok(None) => break,
            Err(_) => break,
        }
    }

    if frame_peaks.is_empty() {
        return vec![];
    }

    // Downsample to at most 200 buckets using per-bucket peak.
    let target = frame_peaks.len().min(200).max(1);
    let chunk = frame_peaks.len() as f32 / target as f32;
    (0..target)
        .map(|i| {
            let start = (i as f32 * chunk) as usize;
            let end = ((i + 1) as f32 * chunk) as usize;
            let end = end.min(frame_peaks.len());
            let peak = frame_peaks[start..end].iter().copied().fold(0f32, f32::max);
            (peak * 1024.0).round().min(1024.0) as u16
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::encode_voice_ogg;

    #[test]
    fn roundtrip_encode_decode_waveform() {
        // 480 ms of a 440 Hz sine at 48 kHz mono (23040 samples).
        let n_samples: usize = 48000 / 2;
        let pcm: Vec<i16> = (0..n_samples)
            .map(|i| {
                let t = i as f32 / 48000.0;
                (f32::sin(2.0 * std::f32::consts::PI * 440.0 * t) * 16000.0) as i16
            })
            .collect();

        let ogg_bytes = encode_voice_ogg(&pcm, &[], 480).expect("encode succeeded");
        let waveform = compute_waveform_from_ogg(&ogg_bytes);

        assert!(!waveform.is_empty(), "waveform must be non-empty");
        assert!(waveform.len() <= 200, "at most 200 samples");
        assert!(
            waveform.iter().all(|&v| v <= 1024),
            "all samples in 0..=1024"
        );
        // A 440 Hz sine should produce non-trivial amplitudes.
        assert!(
            waveform.iter().any(|&v| v > 100),
            "expected audible amplitudes"
        );
    }

    #[test]
    fn empty_bytes_returns_empty() {
        assert!(compute_waveform_from_ogg(&[]).is_empty());
    }

    #[test]
    fn garbage_returns_empty() {
        assert!(compute_waveform_from_ogg(b"not ogg data at all").is_empty());
    }
}
